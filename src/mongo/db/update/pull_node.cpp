/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/update/pull_node.h"

#include "mongo/db/matcher/copyable_match_expression.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/query/collation/collator_interface.h"

namespace mongo {

/**
 * The ObjectMatcher is used when the $pull condition is specified as an object and the first field
 * of that object is not an operator (like $gt).
 */
class PullNode::ObjectMatcher : public PullNode::ElementMatcher {
public:
    ObjectMatcher(BSONObj matchCondition, const CollatorInterface* collator)
        : _matchExpr(
              matchCondition, stdx::make_unique<ExtensionsCallbackDisallowExtensions>(), collator) {
    }

    std::unique_ptr<ElementMatcher> clone() const final {
        return stdx::make_unique<ObjectMatcher>(*this);
    }

    bool match(mutablebson::ConstElement element) final {
        if (element.getType() == mongo::Object) {
            return _matchExpr->matchesBSON(element.getValueObject());
        } else {
            return false;
        }
    }

    void setCollator(const CollatorInterface* collator) final {
        _matchExpr.setCollator(collator);
    }

private:
    CopyableMatchExpression _matchExpr;
};

/**
 * The WrappedObjectMatcher is used when the condition is a regex or an object with an operator as
 * its first field (e.g., {$gt: ...}). It is possible that the element we want to compare is not an
 * object, so we wrap it in an object before comparing it. We also wrap the MatchExpression in an
 * empty object so that we are comparing the MatchCondition and the array element at the same level.
 * This hack allows us to use a MatchExpression to check a BSONElement.
 */
class PullNode::WrappedObjectMatcher : public PullNode::ElementMatcher {
public:
    WrappedObjectMatcher(BSONElement matchCondition, const CollatorInterface* collator)
        : _matchExpr(matchCondition.wrap(""),
                     stdx::make_unique<ExtensionsCallbackDisallowExtensions>(),
                     collator) {}

    std::unique_ptr<ElementMatcher> clone() const final {
        return stdx::make_unique<WrappedObjectMatcher>(*this);
    }

    bool match(mutablebson::ConstElement element) final {
        BSONObj candidate = element.getValue().wrap("");
        return _matchExpr->matchesBSON(candidate);
    }

    void setCollator(const CollatorInterface* collator) final {
        _matchExpr.setCollator(collator);
    }

private:
    CopyableMatchExpression _matchExpr;
};

/**
 * The EqualityMatcher is used when the condition is a primitive value or an array value. We require
 * an exact match.
 */
class PullNode::EqualityMatcher : public PullNode::ElementMatcher {
public:
    EqualityMatcher(BSONElement modExpr, const CollatorInterface* collator)
        : _modExpr(modExpr), _collator(collator) {}

    std::unique_ptr<ElementMatcher> clone() const final {
        return stdx::make_unique<EqualityMatcher>(*this);
    }

    bool match(mutablebson::ConstElement element) final {
        return (element.compareWithBSONElement(_modExpr, _collator, false) == 0);
    }

    void setCollator(const CollatorInterface* collator) final {
        _collator = collator;
    }

private:
    BSONElement _modExpr;
    const CollatorInterface* _collator;
};

Status PullNode::init(BSONElement modExpr, const CollatorInterface* collator) {
    invariant(modExpr.ok());

    try {
        if (modExpr.type() == mongo::Object &&
            modExpr.embeddedObject().firstElement().getGtLtOp(-1) == -1) {
            _matcher = stdx::make_unique<ObjectMatcher>(modExpr.embeddedObject(), collator);
        } else if (modExpr.type() == mongo::Object || modExpr.type() == mongo::RegEx) {
            _matcher = stdx::make_unique<WrappedObjectMatcher>(modExpr, collator);
        } else {
            _matcher = stdx::make_unique<EqualityMatcher>(modExpr, collator);
        }
    } catch (UserException& exception) {
        return exception.toStatus();
    }

    return Status::OK();
}

void PullNode::apply(mutablebson::Element element,
                     FieldRef* pathToCreate,
                     FieldRef* pathTaken,
                     StringData matchedField,
                     bool fromReplication,
                     bool validateForStorage,
                     const FieldRefSet& immutablePaths,
                     const UpdateIndexData* indexData,
                     LogBuilder* logBuilder,
                     bool* indexesAffected,
                     bool* noop) const {
    *indexesAffected = false;
    *noop = false;

    if (!pathToCreate->empty()) {
        // There were path components we could not traverse. We treat this as a no-op, unless it
        // would have been impossible to create those elements, which we check with
        // checkViability().
        UpdateLeafNode::checkViability(element, *pathToCreate, *pathTaken);

        *noop = true;
        return;
    }

    // This operation only applies to arrays
    uassert(ErrorCodes::BadValue,
            "Cannot apply $pull to a non-array value",
            element.getType() == mongo::Array);

    size_t numRemoved = 0;
    auto cursor = element.leftChild();
    while (cursor.ok()) {
        // Make sure to get the next array element now, because if we remove the 'cursor' element,
        // the rightSibling pointer will be invalidated.
        auto nextElement = cursor.rightSibling();
        if (_matcher->match(cursor)) {
            invariantOK(cursor.remove());
            numRemoved++;
        }
        cursor = nextElement;
    }

    if (numRemoved == 0) {
        *noop = true;
        return;  // Skip the index check and logging steps.
    }

    // Determine if indexes are affected.
    if (indexData && indexData->mightBeIndexed(pathTaken->dottedField())) {
        *indexesAffected = true;
    }

    if (logBuilder) {
        auto& doc = logBuilder->getDocument();
        auto logElement = doc.makeElementArray(pathTaken->dottedField());

        for (auto cursor = element.leftChild(); cursor.ok(); cursor = cursor.rightSibling()) {
            dassert(cursor.hasValue());

            auto copy = doc.makeElementWithNewFieldName(StringData(), cursor.getValue());
            uassert(ErrorCodes::InternalError, "could not create copy element", copy.ok());
            uassertStatusOK(logElement.pushBack(copy));
        }

        uassertStatusOK(logBuilder->addToSets(logElement));
    }
}

}  // namespace mongo
