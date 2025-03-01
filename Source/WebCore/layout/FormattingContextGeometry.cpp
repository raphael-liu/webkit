/*
 * Copyright (C) 2018 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "FormattingContext.h"

#if ENABLE(LAYOUT_FORMATTING_CONTEXT)

#include "FloatingState.h"
#include "InlineFormattingState.h"
#include "TableFormattingState.h"

namespace WebCore {
namespace Layout {

static inline bool isHeightAuto(const Box& layoutBox)
{
    // 10.5 Content height: the 'height' property
    //
    // The percentage is calculated with respect to the height of the generated box's containing block.
    // If the height of the containing block is not specified explicitly (i.e., it depends on content height),
    // and this element is not absolutely positioned, the used height is calculated as if 'auto' was specified.

    auto height = layoutBox.style().logicalHeight();
    if (height.isAuto())
        return true;

    if (height.isPercent()) {
        if (layoutBox.isOutOfFlowPositioned())
            return false;

        return !layoutBox.containingBlock()->style().logicalHeight().isFixed();
    }

    return false;
}

Optional<LayoutUnit> FormattingContext::Geometry::computedHeightValue(const Box& layoutBox, HeightType heightType, Optional<LayoutUnit> containingBlockHeight) const
{
    auto& style = layoutBox.style();
    auto height = heightType == HeightType::Normal ? style.logicalHeight() : heightType == HeightType::Min ? style.logicalMinHeight() : style.logicalMaxHeight();
    if (height.isUndefined() || height.isAuto())
        return { };

    if (height.isFixed())
        return LayoutUnit { height.value() };

    if (!containingBlockHeight) {
        // Containing block's height is already computed since we layout the out-of-flow boxes as the last step.
        ASSERT(!layoutBox.isOutOfFlowPositioned());
        if (layoutState().inQuirksMode())
            containingBlockHeight = formattingContext().quirks().heightValueOfNearestContainingBlockWithFixedHeight(layoutBox);
        else {
            auto containingBlockHeightFromStyle = layoutBox.containingBlock()->style().logicalHeight();
            if (containingBlockHeightFromStyle.isFixed())
                containingBlockHeight = LayoutUnit { containingBlockHeightFromStyle.value() };
        }
    }

    if (!containingBlockHeight)
        return { };

    return valueForLength(height, *containingBlockHeight);
}

Optional<LayoutUnit> FormattingContext::Geometry::computedContentHeight(const Box& layoutBox, Optional<LayoutUnit> containingBlockHeight) const
{
    if (auto height = computedHeightValue(layoutBox, HeightType::Normal, containingBlockHeight)) {
        if (layoutBox.style().boxSizing() == BoxSizing::ContentBox)
            return height;
        auto& boxGeometry = formattingContext().geometryForBox(layoutBox);
        return *height - (boxGeometry.verticalBorder() + boxGeometry.verticalPadding().valueOr(0));
    }
    return { };
}

Optional<LayoutUnit> FormattingContext::Geometry::computedContentWidth(const Box& layoutBox, LayoutUnit containingBlockWidth) const
{
    if (auto width = computedValueIfNotAuto(layoutBox.style().logicalWidth(), containingBlockWidth)) {
        if (layoutBox.style().boxSizing() == BoxSizing::ContentBox)
            return width;
        auto& boxGeometry = formattingContext().geometryForBox(layoutBox);
        return *width - (boxGeometry.horizontalBorder() + boxGeometry.horizontalPadding().valueOr(0));
    }
    return { };
}

LayoutUnit FormattingContext::Geometry::contentHeightForFormattingContextRoot(const Box& layoutBox) const
{
    ASSERT(isHeightAuto(layoutBox) && (layoutBox.establishesFormattingContext() || layoutBox.isDocumentBox()));

    // 10.6.7 'Auto' heights for block formatting context roots

    // If it only has inline-level children, the height is the distance between the top of the topmost line box and the bottom of the bottommost line box.
    // If it has block-level children, the height is the distance between the top margin-edge of the topmost block-level
    // child box and the bottom margin-edge of the bottommost block-level child box.

    // In addition, if the element has any floating descendants whose bottom margin edge is below the element's bottom content edge,
    // then the height is increased to include those edges. Only floats that participate in this block formatting context are taken
    // into account, e.g., floats inside absolutely positioned descendants or other floats are not.
    if (!is<Container>(layoutBox) || !downcast<Container>(layoutBox).hasInFlowOrFloatingChild())
        return { };

    auto& layoutState = this->layoutState();
    auto& formattingContext = this->formattingContext();
    auto& boxGeometry = formattingContext.geometryForBox(layoutBox);
    auto borderAndPaddingTop = boxGeometry.borderTop() + boxGeometry.paddingTop().valueOr(0);
    auto top = borderAndPaddingTop;
    auto bottom = borderAndPaddingTop;
    auto& formattingRootContainer = downcast<Container>(layoutBox);
    if (formattingRootContainer.establishesInlineFormattingContext()) {
        auto& lineBoxes = downcast<InlineFormattingState>(layoutState.establishedFormattingState(formattingRootContainer)).lineBoxes();
        // Even empty containers generate one line. 
        ASSERT(!lineBoxes.isEmpty());
        top = lineBoxes.first()->logicalTop();
        bottom = lineBoxes.last()->logicalBottom();
    } else if (formattingRootContainer.establishesBlockFormattingContext() || formattingRootContainer.establishesTableFormattingContext() || formattingRootContainer.isDocumentBox()) {
        if (formattingRootContainer.hasInFlowChild()) {
            auto& firstBoxGeometry = formattingContext.geometryForBox(*formattingRootContainer.firstInFlowChild(), EscapeType::AccessChildFormattingContext);
            auto& lastBoxGeometry = formattingContext.geometryForBox(*formattingRootContainer.lastInFlowChild(), EscapeType::AccessChildFormattingContext);
            top = firstBoxGeometry.rectWithMargin().top();
            bottom = lastBoxGeometry.rectWithMargin().bottom();
        }
    } else
        ASSERT_NOT_REACHED();

    auto* formattingContextRoot = &formattingRootContainer;
    // TODO: The document renderer is not a formatting context root by default at all. Need to find out what it is.
    if (!layoutBox.establishesFormattingContext()) {
        ASSERT(layoutBox.isDocumentBox());
        formattingContextRoot = &layoutBox.formattingContextRoot();
    }

    auto& floatingState = layoutState.establishedFormattingState(*formattingContextRoot).floatingState();
    auto floatBottom = floatingState.bottom(*formattingContextRoot);
    if (floatBottom) {
        bottom = std::max<LayoutUnit>(*floatBottom, bottom);
        auto floatTop = floatingState.top(*formattingContextRoot);
        ASSERT(floatTop);
        top = std::min<LayoutUnit>(*floatTop, top);
    }

    auto computedHeight = bottom - top;
    LOG_WITH_STREAM(FormattingContextLayout, stream << "[Height] -> content height for formatting context root -> height(" << computedHeight << "px) layoutBox("<< &layoutBox << ")");
    return computedHeight;
}

Optional<LayoutUnit> FormattingContext::Geometry::computedValueIfNotAuto(const Length& geometryProperty, LayoutUnit containingBlockWidth) const
{
    if (geometryProperty.isUndefined())
        return WTF::nullopt;

    if (geometryProperty.isAuto())
        return WTF::nullopt;

    return valueForLength(geometryProperty, containingBlockWidth);
}

Optional<LayoutUnit> FormattingContext::Geometry::fixedValue(const Length& geometryProperty) const
{
    if (!geometryProperty.isFixed())
        return WTF::nullopt;
    return LayoutUnit(geometryProperty.value());
}

// https://www.w3.org/TR/CSS22/visudet.html#min-max-heights
// Specifies a percentage for determining the used value. The percentage is calculated with respect to the height of the generated box's containing block.
// If the height of the containing block is not specified explicitly (i.e., it depends on content height), and this element is not absolutely positioned,
// the percentage value is treated as '0' (for 'min-height') or 'none' (for 'max-height').
Optional<LayoutUnit> FormattingContext::Geometry::computedMaxHeight(const Box& layoutBox, Optional<LayoutUnit> containingBlockHeight) const
{
    return computedHeightValue(layoutBox, HeightType::Max, containingBlockHeight);
}

Optional<LayoutUnit> FormattingContext::Geometry::computedMinHeight(const Box& layoutBox, Optional<LayoutUnit> containingBlockHeight) const
{
    if (auto minHeightValue = computedHeightValue(layoutBox, HeightType::Min, containingBlockHeight))
        return minHeightValue;

    return { LayoutUnit { } };
}

Optional<LayoutUnit> FormattingContext::Geometry::computedMinWidth(const Box& layoutBox, LayoutUnit containingBlockWidth) const
{
    return computedValueIfNotAuto(layoutBox.style().logicalMinWidth(), containingBlockWidth);
}

Optional<LayoutUnit> FormattingContext::Geometry::computedMaxWidth(const Box& layoutBox, LayoutUnit containingBlockWidth) const
{
    return computedValueIfNotAuto(layoutBox.style().logicalMaxWidth(), containingBlockWidth);
}

LayoutUnit FormattingContext::Geometry::staticVerticalPositionForOutOfFlowPositioned(const Box& layoutBox, const UsedVerticalValues& usedVerticalValues) const
{
    ASSERT(layoutBox.isOutOfFlowPositioned());

    // For the purposes of this section and the next, the term "static position" (of an element) refers, roughly, to the position an element would have
    // had in the normal flow. More precisely, the static position for 'top' is the distance from the top edge of the containing block to the top margin
    // edge of a hypothetical box that would have been the first box of the element if its specified 'position' value had been 'static' and its specified
    // 'float' had been 'none' and its specified 'clear' had been 'none'. (Note that due to the rules in section 9.7 this might require also assuming a different
    // computed value for 'display'.) The value is negative if the hypothetical box is above the containing block.

    // Start with this box's border box offset from the parent's border box.
    auto& formattingContext = this->formattingContext();
    LayoutUnit top;
    if (auto* previousInFlowSibling = layoutBox.previousInFlowSibling()) {
        // Add sibling offset
        auto& previousInFlowBoxGeometry = formattingContext.geometryForBox(*previousInFlowSibling, FormattingContext::EscapeType::AccessChildFormattingContext);
        top += previousInFlowBoxGeometry.bottom() + previousInFlowBoxGeometry.nonCollapsedMarginAfter();
    } else {
        ASSERT(layoutBox.parent());
        top = formattingContext.geometryForBox(*layoutBox.parent(), FormattingContext::EscapeType::AccessChildFormattingContext).contentBoxTop();
    }

    // Resolve top all the way up to the containing block.
    auto& containingBlock = *layoutBox.containingBlock();
    // Start with the parent since we pretend that this box is normal flow.
    for (auto* container = layoutBox.parent(); container != &containingBlock; container = container->containingBlock()) {
        auto& boxGeometry = formattingContext.geometryForBox(*container, FormattingContext::EscapeType::AccessChildFormattingContext);
        // Display::Box::top is the border box top position in its containing block's coordinate system.
        top += boxGeometry.top();
        ASSERT(!container->isPositioned() || layoutBox.isFixedPositioned());
    }
    // Move the static position relative to the padding box. This is very specific to abolutely positioned boxes.
    return top - usedVerticalValues.constraints.contentBoxTop;
}

LayoutUnit FormattingContext::Geometry::staticHorizontalPositionForOutOfFlowPositioned(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues) const
{
    ASSERT(layoutBox.isOutOfFlowPositioned());
    // See staticVerticalPositionForOutOfFlowPositioned for the definition of the static position.

    // Start with this box's border box offset from the parent's border box.
    auto& formattingContext = this->formattingContext();
    ASSERT(layoutBox.parent());
    auto left = formattingContext.geometryForBox(*layoutBox.parent(), FormattingContext::EscapeType::AccessChildFormattingContext).contentBoxLeft();

    // Resolve left all the way up to the containing block.
    auto& containingBlock = *layoutBox.containingBlock();
    // Start with the parent since we pretend that this box is normal flow.
    for (auto* container = layoutBox.parent(); container != &containingBlock; container = container->containingBlock()) {
        auto& boxGeometry = formattingContext.geometryForBox(*container, FormattingContext::EscapeType::AccessChildFormattingContext);
        // Display::Box::left is the border box left position in its containing block's coordinate system.
        left += boxGeometry.left();
        ASSERT(!container->isPositioned() || layoutBox.isFixedPositioned());
    }
    // Move the static position relative to the padding box. This is very specific to abolutely positioned boxes.
    return left - usedHorizontalValues.constraints.contentBoxLeft;
}

LayoutUnit FormattingContext::Geometry::shrinkToFitWidth(const Box& formattingRoot, LayoutUnit availableWidth)
{
    LOG_WITH_STREAM(FormattingContextLayout, stream << "[Width] -> shrink to fit -> unsupported -> width(" << LayoutUnit { } << "px) layoutBox: " << &formattingRoot << ")");
    ASSERT(formattingRoot.establishesFormattingContext());

    // Calculation of the shrink-to-fit width is similar to calculating the width of a table cell using the automatic table layout algorithm.
    // Roughly: calculate the preferred width by formatting the content without breaking lines other than where explicit line breaks occur,
    // and also calculate the preferred minimum width, e.g., by trying all possible line breaks. CSS 2.2 does not define the exact algorithm.
    // Thirdly, find the available width: in this case, this is the width of the containing block minus the used values of 'margin-left', 'border-left-width',
    // 'padding-left', 'padding-right', 'border-right-width', 'margin-right', and the widths of any relevant scroll bars.

    // Then the shrink-to-fit width is: min(max(preferred minimum width, available width), preferred width).
    auto intrinsicWidthConstraints = IntrinsicWidthConstraints { };
    if (is<Container>(formattingRoot)) {
        auto& root = downcast<Container>(formattingRoot);
        auto& formattingStateForRoot = layoutState().createFormattingStateForFormattingRootIfNeeded(root);
        auto precomputedIntrinsicWidthConstraints = formattingStateForRoot.intrinsicWidthConstraints();
        if (!precomputedIntrinsicWidthConstraints)
            intrinsicWidthConstraints = LayoutContext::createFormattingContext(root, layoutState())->computedIntrinsicWidthConstraints();
        else
            intrinsicWidthConstraints = *precomputedIntrinsicWidthConstraints;
    }
    return std::min(std::max(intrinsicWidthConstraints.minimum, availableWidth), intrinsicWidthConstraints.maximum);
}

VerticalGeometry FormattingContext::Geometry::outOfFlowNonReplacedVerticalGeometry(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues, const UsedVerticalValues& usedVerticalValues) const
{
    ASSERT(layoutBox.isOutOfFlowPositioned() && !layoutBox.replaced());
    ASSERT(usedVerticalValues.constraints.height);

    // 10.6.4 Absolutely positioned, non-replaced elements
    //
    // For absolutely positioned elements, the used values of the vertical dimensions must satisfy this constraint:
    // 'top' + 'margin-top' + 'border-top-width' + 'padding-top' + 'height' + 'padding-bottom' + 'border-bottom-width' + 'margin-bottom' + 'bottom'
    // = height of containing block

    // If all three of 'top', 'height', and 'bottom' are auto, set 'top' to the static position and apply rule number three below.

    // If none of the three are 'auto': If both 'margin-top' and 'margin-bottom' are 'auto', solve the equation under the extra
    // constraint that the two margins get equal values. If one of 'margin-top' or 'margin-bottom' is 'auto', solve the equation for that value.
    // If the values are over-constrained, ignore the value for 'bottom' and solve for that value.

    // Otherwise, pick the one of the following six rules that applies.

    // 1. 'top' and 'height' are 'auto' and 'bottom' is not 'auto', then the height is based on the content per 10.6.7,
    //     set 'auto' values for 'margin-top' and 'margin-bottom' to 0, and solve for 'top'
    // 2. 'top' and 'bottom' are 'auto' and 'height' is not 'auto', then set 'top' to the static position, set 'auto' values for
    //    'margin-top' and 'margin-bottom' to 0, and solve for 'bottom'
    // 3. 'height' and 'bottom' are 'auto' and 'top' is not 'auto', then the height is based on the content per 10.6.7, set 'auto'
    //     values for 'margin-top' and 'margin-bottom' to 0, and solve for 'bottom'
    // 4. 'top' is 'auto', 'height' and 'bottom' are not 'auto', then set 'auto' values for 'margin-top' and 'margin-bottom' to 0, and solve for 'top'
    // 5. 'height' is 'auto', 'top' and 'bottom' are not 'auto', then 'auto' values for 'margin-top' and 'margin-bottom' are set to 0 and solve for 'height'
    // 6. 'bottom' is 'auto', 'top' and 'height' are not 'auto', then set 'auto' values for 'margin-top' and 'margin-bottom' to 0 and solve for 'bottom'

    auto& formattingContext = this->formattingContext();
    auto& style = layoutBox.style();
    auto& boxGeometry = formattingContext.geometryForBox(layoutBox);
    auto containingBlockHeight = *usedVerticalValues.constraints.height;
    auto containingBlockWidth = usedHorizontalValues.constraints.width;

    auto top = computedValueIfNotAuto(style.logicalTop(), containingBlockWidth);
    auto bottom = computedValueIfNotAuto(style.logicalBottom(), containingBlockWidth);
    auto height = usedVerticalValues.height ? usedVerticalValues.height.value() : computedContentHeight(layoutBox, containingBlockHeight);
    auto computedVerticalMargin = Geometry::computedVerticalMargin(layoutBox, usedHorizontalValues);
    UsedVerticalMargin::NonCollapsedValues usedVerticalMargin; 
    auto paddingTop = boxGeometry.paddingTop().valueOr(0);
    auto paddingBottom = boxGeometry.paddingBottom().valueOr(0);
    auto borderTop = boxGeometry.borderTop();
    auto borderBottom = boxGeometry.borderBottom();

    if (!top && !height && !bottom)
        top = staticVerticalPositionForOutOfFlowPositioned(layoutBox, usedVerticalValues);

    if (top && height && bottom) {
        if (!computedVerticalMargin.before && !computedVerticalMargin.after) {
            auto marginBeforeAndAfter = containingBlockHeight - (*top + borderTop + paddingTop + *height + paddingBottom + borderBottom + *bottom);
            usedVerticalMargin = { marginBeforeAndAfter / 2, marginBeforeAndAfter / 2 };
        } else if (!computedVerticalMargin.before) {
            usedVerticalMargin.after = *computedVerticalMargin.after;
            usedVerticalMargin.before = containingBlockHeight - (*top + borderTop + paddingTop + *height + paddingBottom + borderBottom + usedVerticalMargin.after + *bottom);
        } else if (!computedVerticalMargin.after) {
            usedVerticalMargin.before = *computedVerticalMargin.before;
            usedVerticalMargin.after = containingBlockHeight - (*top + usedVerticalMargin.before + borderTop + paddingTop + *height + paddingBottom + borderBottom + *bottom);
        } else
            usedVerticalMargin = { *computedVerticalMargin.before, *computedVerticalMargin.after };
        // Over-constrained?
        auto boxHeight = *top + usedVerticalMargin.before + borderTop + paddingTop + *height + paddingBottom + borderBottom + usedVerticalMargin.after + *bottom;
        if (boxHeight != containingBlockHeight)
            bottom = containingBlockHeight - (*top + usedVerticalMargin.before + borderTop + paddingTop + *height + paddingBottom + borderBottom + usedVerticalMargin.after);
    }

    if (!top && !height && bottom) {
        // #1
        height = contentHeightForFormattingContextRoot(layoutBox);
        usedVerticalMargin = { computedVerticalMargin.before.valueOr(0), computedVerticalMargin.after.valueOr(0) };
        top = containingBlockHeight - (usedVerticalMargin.before + borderTop + paddingTop + *height + paddingBottom + borderBottom + usedVerticalMargin.after + *bottom); 
    }

    if (!top && !bottom && height) {
        // #2
        top = staticVerticalPositionForOutOfFlowPositioned(layoutBox, usedVerticalValues);
        usedVerticalMargin = { computedVerticalMargin.before.valueOr(0), computedVerticalMargin.after.valueOr(0) };
        bottom = containingBlockHeight - (*top + usedVerticalMargin.before + borderTop + paddingTop + *height + paddingBottom + borderBottom + usedVerticalMargin.after);
    }

    if (!height && !bottom && top) {
        // #3
        height = contentHeightForFormattingContextRoot(layoutBox);
        usedVerticalMargin = { computedVerticalMargin.before.valueOr(0), computedVerticalMargin.after.valueOr(0) };
        bottom = containingBlockHeight - (*top + usedVerticalMargin.before + borderTop + paddingTop + *height + paddingBottom + borderBottom + usedVerticalMargin.after);
    }

    if (!top && height && bottom) {
        // #4
        usedVerticalMargin = { computedVerticalMargin.before.valueOr(0), computedVerticalMargin.after.valueOr(0) };
        top = containingBlockHeight - (usedVerticalMargin.before + borderTop + paddingTop + *height + paddingBottom + borderBottom + usedVerticalMargin.after + *bottom);
    }

    if (!height && top && bottom) {
        // #5
        usedVerticalMargin = { computedVerticalMargin.before.valueOr(0), computedVerticalMargin.after.valueOr(0) };
        height = containingBlockHeight - (*top + usedVerticalMargin.before + borderTop + paddingTop + paddingBottom + borderBottom + usedVerticalMargin.after + *bottom);
    }

    if (!bottom && top && height) {
        // #6
        usedVerticalMargin = { computedVerticalMargin.before.valueOr(0), computedVerticalMargin.after.valueOr(0) };
        bottom = containingBlockHeight - (*top + usedVerticalMargin.before + borderTop + paddingTop + *height + paddingBottom + borderBottom + usedVerticalMargin.after);
    }

    ASSERT(top);
    ASSERT(bottom);
    ASSERT(height);

    // For out-of-flow elements the containing block is formed by the padding edge of the ancestor.
    // At this point the positioned value is in the coordinate system of the padding box. Let's convert it to border box coordinate system.
    auto containingBlockPaddingVerticalEdge = usedVerticalValues.constraints.contentBoxTop;
    *top += containingBlockPaddingVerticalEdge;
    *bottom += containingBlockPaddingVerticalEdge;

    LOG_WITH_STREAM(FormattingContextLayout, stream << "[Position][Height][Margin] -> out-of-flow non-replaced -> top(" << *top << "px) bottom("  << *bottom << "px) height(" << *height << "px) margin(" << usedVerticalMargin.before << "px, "  << usedVerticalMargin.after << "px) layoutBox(" << &layoutBox << ")");
    return { *top, *bottom, { *height, usedVerticalMargin } };
}

HorizontalGeometry FormattingContext::Geometry::outOfFlowNonReplacedHorizontalGeometry(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues)
{
    ASSERT(layoutBox.isOutOfFlowPositioned() && !layoutBox.replaced());
    
    // 10.3.7 Absolutely positioned, non-replaced elements
    //
    // 'left' + 'margin-left' + 'border-left-width' + 'padding-left' + 'width' + 'padding-right' + 'border-right-width' + 'margin-right' + 'right'
    // = width of containing block

    // If all three of 'left', 'width', and 'right' are 'auto': First set any 'auto' values for 'margin-left' and 'margin-right' to 0.
    // Then, if the 'direction' property of the element establishing the static-position containing block is 'ltr' set 'left' to the static
    // position and apply rule number three below; otherwise, set 'right' to the static position and apply rule number one below.
    //
    // If none of the three is 'auto': If both 'margin-left' and 'margin-right' are 'auto', solve the equation under the extra constraint that the two margins get equal values,
    // unless this would make them negative, in which case when direction of the containing block is 'ltr' ('rtl'), set 'margin-left' ('margin-right') to zero and
    // solve for 'margin-right' ('margin-left'). If one of 'margin-left' or 'margin-right' is 'auto', solve the equation for that value.
    // If the values are over-constrained, ignore the value for 'left' (in case the 'direction' property of the containing block is 'rtl') or 'right'
    // (in case 'direction' is 'ltr') and solve for that value.
    //
    // Otherwise, set 'auto' values for 'margin-left' and 'margin-right' to 0, and pick the one of the following six rules that applies.
    //
    // 1. 'left' and 'width' are 'auto' and 'right' is not 'auto', then the width is shrink-to-fit. Then solve for 'left'
    // 2. 'left' and 'right' are 'auto' and 'width' is not 'auto', then if the 'direction' property of the element establishing the static-position 
    //    containing block is 'ltr' set 'left' to the static position, otherwise set 'right' to the static position.
    //    Then solve for 'left' (if 'direction is 'rtl') or 'right' (if 'direction' is 'ltr').
    // 3. 'width' and 'right' are 'auto' and 'left' is not 'auto', then the width is shrink-to-fit . Then solve for 'right'
    // 4. 'left' is 'auto', 'width' and 'right' are not 'auto', then solve for 'left'
    // 5. 'width' is 'auto', 'left' and 'right' are not 'auto', then solve for 'width'
    // 6. 'right' is 'auto', 'left' and 'width' are not 'auto', then solve for 'right'

    auto& formattingContext = this->formattingContext();
    auto& style = layoutBox.style();
    auto& boxGeometry = formattingContext.geometryForBox(layoutBox);
    auto containingBlockWidth = usedHorizontalValues.constraints.width;
    auto isLeftToRightDirection = layoutBox.containingBlock()->style().isLeftToRightDirection();
    
    auto left = computedValueIfNotAuto(style.logicalLeft(), containingBlockWidth);
    auto right = computedValueIfNotAuto(style.logicalRight(), containingBlockWidth);
    auto width = usedHorizontalValues.width ? usedHorizontalValues.width : computedContentWidth(layoutBox, containingBlockWidth);
    auto computedHorizontalMargin = Geometry::computedHorizontalMargin(layoutBox, usedHorizontalValues);
    UsedHorizontalMargin usedHorizontalMargin;
    auto paddingLeft = boxGeometry.paddingLeft().valueOr(0);
    auto paddingRight = boxGeometry.paddingRight().valueOr(0);
    auto borderLeft = boxGeometry.borderLeft();
    auto borderRight = boxGeometry.borderRight();
    if (!left && !width && !right) {
        // If all three of 'left', 'width', and 'right' are 'auto': First set any 'auto' values for 'margin-left' and 'margin-right' to 0.
        // Then, if the 'direction' property of the element establishing the static-position containing block is 'ltr' set 'left' to the static
        // position and apply rule number three below; otherwise, set 'right' to the static position and apply rule number one below.
        usedHorizontalMargin = { computedHorizontalMargin.start.valueOr(0), computedHorizontalMargin.end.valueOr(0) };

        auto staticHorizontalPosition = staticHorizontalPositionForOutOfFlowPositioned(layoutBox, usedHorizontalValues);
        if (isLeftToRightDirection)
            left = staticHorizontalPosition;
        else
            right = staticHorizontalPosition;
    } else if (left && width && right) {
        // If none of the three is 'auto': If both 'margin-left' and 'margin-right' are 'auto', solve the equation under the extra constraint that the two margins get equal values,
        // unless this would make them negative, in which case when direction of the containing block is 'ltr' ('rtl'), set 'margin-left' ('margin-right') to zero and
        // solve for 'margin-right' ('margin-left'). If one of 'margin-left' or 'margin-right' is 'auto', solve the equation for that value.
        // If the values are over-constrained, ignore the value for 'left' (in case the 'direction' property of the containing block is 'rtl') or 'right'
        // (in case 'direction' is 'ltr') and solve for that value.
        if (!computedHorizontalMargin.start && !computedHorizontalMargin.end) {
            auto marginStartAndEnd = containingBlockWidth - (*left + borderLeft + paddingLeft + *width + paddingRight + borderRight + *right);
            if (marginStartAndEnd >= 0)
                usedHorizontalMargin = { marginStartAndEnd / 2, marginStartAndEnd / 2 };
            else {
                if (isLeftToRightDirection) {
                    usedHorizontalMargin.start = 0_lu;
                    usedHorizontalMargin.end = containingBlockWidth - (*left + usedHorizontalMargin.start + borderLeft + paddingLeft + *width + paddingRight + borderRight + *right);
                } else {
                    usedHorizontalMargin.end = 0_lu;
                    usedHorizontalMargin.start = containingBlockWidth - (*left + borderLeft + paddingLeft + *width + paddingRight + borderRight + usedHorizontalMargin.end + *right);
                }
            }
        } else if (!computedHorizontalMargin.start) {
            usedHorizontalMargin.end = *computedHorizontalMargin.end;
            usedHorizontalMargin.start = containingBlockWidth - (*left + borderLeft + paddingLeft + *width + paddingRight + borderRight + usedHorizontalMargin.end + *right);
        } else if (!computedHorizontalMargin.end) {
            usedHorizontalMargin.start = *computedHorizontalMargin.start;
            usedHorizontalMargin.end = containingBlockWidth - (*left + usedHorizontalMargin.start + borderLeft + paddingLeft + *width + paddingRight + borderRight + *right);
        } else {
            usedHorizontalMargin = { *computedHorizontalMargin.start, *computedHorizontalMargin.end };
            // Overconstrained? Ignore right (left).
            if (isLeftToRightDirection)
                right = containingBlockWidth - (usedHorizontalMargin.start + *left + borderLeft + paddingLeft + *width + paddingRight + borderRight + usedHorizontalMargin.end);
            else
                left = containingBlockWidth - (usedHorizontalMargin.start + borderLeft + paddingLeft + *width + paddingRight + borderRight + usedHorizontalMargin.end + *right);
        }
    } else {
        // Otherwise, set 'auto' values for 'margin-left' and 'margin-right' to 0, and pick the one of the following six rules that applies.
        usedHorizontalMargin = { computedHorizontalMargin.start.valueOr(0), computedHorizontalMargin.end.valueOr(0) };
    }

    if (!left && !width && right) {
        // #1
        // Calculate the available width by solving for 'width' after setting 'left' (in case 1) to 0
        left = LayoutUnit { 0 };
        auto availableWidth = containingBlockWidth - (*left + usedHorizontalMargin.start + borderLeft + paddingLeft + paddingRight + borderRight + usedHorizontalMargin.end + *right);
        width = shrinkToFitWidth(layoutBox, availableWidth);
        left = containingBlockWidth - (usedHorizontalMargin.start + borderLeft + paddingLeft + *width + paddingRight  + borderRight + usedHorizontalMargin.end + *right);
    } else if (!left && !right && width) {
        // #2
        auto staticHorizontalPosition = staticHorizontalPositionForOutOfFlowPositioned(layoutBox, usedHorizontalValues);
        if (isLeftToRightDirection) {
            left = staticHorizontalPosition;
            right = containingBlockWidth - (*left + usedHorizontalMargin.start + borderLeft + paddingLeft + *width + paddingRight + borderRight + usedHorizontalMargin.end);
        } else {
            right = staticHorizontalPosition;
            left = containingBlockWidth - (usedHorizontalMargin.start + borderLeft + paddingLeft + *width + paddingRight + borderRight + usedHorizontalMargin.end + *right);
        }
    } else if (!width && !right && left) {
        // #3
        // Calculate the available width by solving for 'width' after setting 'right' (in case 3) to 0
        right = LayoutUnit { 0 };
        auto availableWidth = containingBlockWidth - (*left + usedHorizontalMargin.start + borderLeft + paddingLeft + paddingRight + borderRight + usedHorizontalMargin.end + *right);
        width = shrinkToFitWidth(layoutBox, availableWidth);
        right = containingBlockWidth - (*left + usedHorizontalMargin.start + borderLeft + paddingLeft + *width + paddingRight + borderRight + usedHorizontalMargin.end);
    } else if (!left && width && right) {
        // #4
        left = containingBlockWidth - (usedHorizontalMargin.start + borderLeft + paddingLeft + *width + paddingRight + borderRight + usedHorizontalMargin.end + *right);
    } else if (!width && left && right) {
        // #5
        width = containingBlockWidth - (*left + usedHorizontalMargin.start + borderLeft + paddingLeft + paddingRight + borderRight + usedHorizontalMargin.end + *right);
    } else if (!right && left && width) {
        // #6
        right = containingBlockWidth - (*left + usedHorizontalMargin.start + borderLeft + paddingLeft + *width + paddingRight + borderRight + usedHorizontalMargin.end);
    }

    ASSERT(left);
    ASSERT(right);
    ASSERT(width);

    // For out-of-flow elements the containing block is formed by the padding edge of the ancestor.
    // At this point the positioned value is in the coordinate system of the padding box. Let's convert it to border box coordinate system.
    auto containingBlockPaddingVerticalEdge = usedHorizontalValues.constraints.contentBoxLeft;
    *left += containingBlockPaddingVerticalEdge;
    *right += containingBlockPaddingVerticalEdge;

    LOG_WITH_STREAM(FormattingContextLayout, stream << "[Position][Width][Margin] -> out-of-flow non-replaced -> left(" << *left << "px) right("  << *right << "px) width(" << *width << "px) margin(" << usedHorizontalMargin.start << "px, "  << usedHorizontalMargin.end << "px) layoutBox(" << &layoutBox << ")");
    return { *left, *right, { *width, usedHorizontalMargin, computedHorizontalMargin } };
}

VerticalGeometry FormattingContext::Geometry::outOfFlowReplacedVerticalGeometry(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues, const UsedVerticalValues& usedVerticalValues) const
{
    ASSERT(layoutBox.isOutOfFlowPositioned() && layoutBox.replaced());
    ASSERT(usedVerticalValues.constraints.height);

    // 10.6.5 Absolutely positioned, replaced elements
    //
    // The used value of 'height' is determined as for inline replaced elements.
    // If 'margin-top' or 'margin-bottom' is specified as 'auto' its used value is determined by the rules below.
    // 1. If both 'top' and 'bottom' have the value 'auto', replace 'top' with the element's static position.
    // 2. If 'bottom' is 'auto', replace any 'auto' on 'margin-top' or 'margin-bottom' with '0'.
    // 3. If at this point both 'margin-top' and 'margin-bottom' are still 'auto', solve the equation under the extra constraint that the two margins must get equal values.
    // 4. If at this point there is only one 'auto' left, solve the equation for that value.
    // 5. If at this point the values are over-constrained, ignore the value for 'bottom' and solve for that value.

    auto& formattingContext = this->formattingContext();
    auto& style = layoutBox.style();
    auto& boxGeometry = formattingContext.geometryForBox(layoutBox);
    auto containingBlockHeight = *usedVerticalValues.constraints.height;
    auto containingBlockWidth = usedHorizontalValues.constraints.width;

    auto top = computedValueIfNotAuto(style.logicalTop(), containingBlockWidth);
    auto bottom = computedValueIfNotAuto(style.logicalBottom(), containingBlockWidth);
    auto height = inlineReplacedHeightAndMargin(layoutBox, usedHorizontalValues, usedVerticalValues).contentHeight;
    auto computedVerticalMargin = Geometry::computedVerticalMargin(layoutBox, usedHorizontalValues);
    Optional<LayoutUnit> usedMarginBefore = computedVerticalMargin.before;
    Optional<LayoutUnit> usedMarginAfter = computedVerticalMargin.after;
    auto paddingTop = boxGeometry.paddingTop().valueOr(0);
    auto paddingBottom = boxGeometry.paddingBottom().valueOr(0);
    auto borderTop = boxGeometry.borderTop();
    auto borderBottom = boxGeometry.borderBottom();

    if (!top && !bottom) {
        // #1
        top = staticVerticalPositionForOutOfFlowPositioned(layoutBox, usedVerticalValues);
    }

    if (!bottom) {
        // #2
        usedMarginBefore = computedVerticalMargin.before.valueOr(0);
        usedMarginAfter = usedMarginBefore;
    }

    if (!usedMarginBefore && !usedMarginAfter) {
        // #3
        auto marginBeforeAndAfter = containingBlockHeight - (*top + borderTop + paddingTop + height + paddingBottom + borderBottom + *bottom);
        usedMarginBefore = marginBeforeAndAfter / 2;
        usedMarginAfter = usedMarginBefore;
    }

    // #4
    if (!top)
        top = containingBlockHeight - (*usedMarginBefore + borderTop + paddingTop + height + paddingBottom + borderBottom + *usedMarginAfter + *bottom);

    if (!bottom)
        bottom = containingBlockHeight - (*top + *usedMarginBefore + borderTop + paddingTop + height + paddingBottom + borderBottom + *usedMarginAfter);

    if (!usedMarginBefore)
        usedMarginBefore = containingBlockHeight - (*top + borderTop + paddingTop + height + paddingBottom + borderBottom + *usedMarginAfter + *bottom);

    if (!usedMarginAfter)
        usedMarginAfter = containingBlockHeight - (*top + *usedMarginBefore + borderTop + paddingTop + height + paddingBottom + borderBottom + *bottom);

    // #5
    auto boxHeight = *top + *usedMarginBefore + borderTop + paddingTop + height + paddingBottom + borderBottom + *usedMarginAfter + *bottom;
    if (boxHeight > containingBlockHeight)
        bottom = containingBlockHeight - (*top + *usedMarginBefore + borderTop + paddingTop + height + paddingBottom + borderBottom + *usedMarginAfter); 

    // For out-of-flow elements the containing block is formed by the padding edge of the ancestor.
    // At this point the positioned value is in the coordinate system of the padding box. Let's convert it to border box coordinate system.
    auto containingBlockPaddingVerticalEdge = usedVerticalValues.constraints.contentBoxTop;
    *top += containingBlockPaddingVerticalEdge;
    *bottom += containingBlockPaddingVerticalEdge;

    ASSERT(top);
    ASSERT(bottom);
    ASSERT(usedMarginBefore);
    ASSERT(usedMarginAfter);
    LOG_WITH_STREAM(FormattingContextLayout, stream << "[Position][Height][Margin] -> out-of-flow replaced -> top(" << *top << "px) bottom("  << *bottom << "px) height(" << height << "px) margin(" << *usedMarginBefore << "px, "  << *usedMarginAfter << "px) layoutBox(" << &layoutBox << ")");
    return { *top, *bottom, { height, { *usedMarginBefore, *usedMarginAfter } } };
}

HorizontalGeometry FormattingContext::Geometry::outOfFlowReplacedHorizontalGeometry(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues, const UsedVerticalValues& usedVerticalValues) const
{
    ASSERT(layoutBox.isOutOfFlowPositioned() && layoutBox.replaced());

    // 10.3.8 Absolutely positioned, replaced elements
    // In this case, section 10.3.7 applies up through and including the constraint equation, but the rest of section 10.3.7 is replaced by the following rules:
    //
    // The used value of 'width' is determined as for inline replaced elements. If 'margin-left' or 'margin-right' is specified as 'auto' its used value is determined by the rules below.
    // 1. If both 'left' and 'right' have the value 'auto', then if the 'direction' property of the element establishing the static-position containing block is 'ltr',
    //   set 'left' to the static position; else if 'direction' is 'rtl', set 'right' to the static position.
    // 2. If 'left' or 'right' are 'auto', replace any 'auto' on 'margin-left' or 'margin-right' with '0'.
    // 3. If at this point both 'margin-left' and 'margin-right' are still 'auto', solve the equation under the extra constraint that the two margins must get equal values,
    //   unless this would make them negative, in which case when the direction of the containing block is 'ltr' ('rtl'), set 'margin-left' ('margin-right') to zero and
    //   solve for 'margin-right' ('margin-left').
    // 4. If at this point there is an 'auto' left, solve the equation for that value.
    // 5. If at this point the values are over-constrained, ignore the value for either 'left' (in case the 'direction' property of the containing block is 'rtl') or
    //   'right' (in case 'direction' is 'ltr') and solve for that value.

    auto& formattingContext = this->formattingContext();
    auto& style = layoutBox.style();
    auto& boxGeometry = formattingContext.geometryForBox(layoutBox);
    auto containingBlockWidth = usedHorizontalValues.constraints.width;
    auto isLeftToRightDirection = layoutBox.containingBlock()->style().isLeftToRightDirection();

    auto left = computedValueIfNotAuto(style.logicalLeft(), containingBlockWidth);
    auto right = computedValueIfNotAuto(style.logicalRight(), containingBlockWidth);
    auto computedHorizontalMargin = Geometry::computedHorizontalMargin(layoutBox, usedHorizontalValues);
    Optional<LayoutUnit> usedMarginStart = computedHorizontalMargin.start;
    Optional<LayoutUnit> usedMarginEnd = computedHorizontalMargin.end;
    auto width = inlineReplacedWidthAndMargin(layoutBox, usedHorizontalValues, usedVerticalValues).contentWidth;
    auto paddingLeft = boxGeometry.paddingLeft().valueOr(0);
    auto paddingRight = boxGeometry.paddingRight().valueOr(0);
    auto borderLeft = boxGeometry.borderLeft();
    auto borderRight = boxGeometry.borderRight();

    if (!left && !right) {
        // #1
        auto staticHorizontalPosition = staticHorizontalPositionForOutOfFlowPositioned(layoutBox, usedHorizontalValues);
        if (isLeftToRightDirection)
            left = staticHorizontalPosition;
        else
            right = staticHorizontalPosition;
    }

    if (!left || !right) {
        // #2
        usedMarginStart = computedHorizontalMargin.start.valueOr(0);
        usedMarginEnd = computedHorizontalMargin.end.valueOr(0);
    }

    if (!usedMarginStart && !usedMarginEnd) {
        // #3
        auto marginStartAndEnd = containingBlockWidth - (*left + borderLeft + paddingLeft + width + paddingRight + borderRight + *right);
        if (marginStartAndEnd >= 0) {
            usedMarginStart = marginStartAndEnd / 2;
            usedMarginEnd = usedMarginStart;
        } else {
            if (isLeftToRightDirection) {
                usedMarginStart = 0_lu;
                usedMarginEnd = containingBlockWidth - (*left + *usedMarginStart + borderLeft + paddingLeft + width + paddingRight + borderRight + *right);
            } else {
                usedMarginEnd = 0_lu;
                usedMarginStart = containingBlockWidth - (*left + borderLeft + paddingLeft + width + paddingRight + borderRight + *usedMarginEnd + *right);
            }
        }
    }

    // #4
    if (!left)
        left = containingBlockWidth - (*usedMarginStart + borderLeft + paddingLeft + width + paddingRight + borderRight + *usedMarginEnd + *right);

    if (!right)
        right = containingBlockWidth - (*left + *usedMarginStart + borderLeft + paddingLeft + width + paddingRight + borderRight + *usedMarginEnd);

    if (!usedMarginStart)
        usedMarginStart = containingBlockWidth - (*left + borderLeft + paddingLeft + width + paddingRight + borderRight + *usedMarginEnd + *right);

    if (!usedMarginEnd)
        usedMarginEnd = containingBlockWidth - (*left + *usedMarginStart + borderLeft + paddingLeft + width + paddingRight + borderRight + *right);

    auto boxWidth = (*left + *usedMarginStart + borderLeft + paddingLeft + width + paddingRight + borderRight + *usedMarginEnd + *right);
    if (boxWidth > containingBlockWidth) {
        // #5 Over-constrained?
        if (isLeftToRightDirection)
            right = containingBlockWidth - (*left + *usedMarginStart + borderLeft + paddingLeft + width + paddingRight + borderRight + *usedMarginEnd);
        else
            left = containingBlockWidth - (*usedMarginStart + borderLeft + paddingLeft + width + paddingRight + borderRight + *usedMarginEnd + *right);
    }

    ASSERT(left);
    ASSERT(right);
    ASSERT(usedMarginStart);
    ASSERT(usedMarginEnd);

    // For out-of-flow elements the containing block is formed by the padding edge of the ancestor.
    // At this point the positioned value is in the coordinate system of the padding box. Let's convert it to border box coordinate system.
    auto containingBlockPaddingVerticalEdge = usedHorizontalValues.constraints.contentBoxLeft;
    *left += containingBlockPaddingVerticalEdge;
    *right += containingBlockPaddingVerticalEdge;

    LOG_WITH_STREAM(FormattingContextLayout, stream << "[Position][Width][Margin] -> out-of-flow replaced -> left(" << *left << "px) right("  << *right << "px) width(" << width << "px) margin(" << *usedMarginStart << "px, "  << *usedMarginEnd << "px) layoutBox(" << &layoutBox << ")");
    return { *left, *right, { width, { *usedMarginStart, *usedMarginEnd }, computedHorizontalMargin } };
}

ContentHeightAndMargin FormattingContext::Geometry::complicatedCases(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues, const UsedVerticalValues& usedVerticalValues) const
{
    ASSERT(!layoutBox.replaced());
    // TODO: Use complicated-case for document renderer for now (see BlockFormattingContext::Geometry::inFlowHeightAndMargin).
    ASSERT((layoutBox.isBlockLevelBox() && layoutBox.isInFlow() && !layoutBox.isOverflowVisible()) || layoutBox.isInlineBlockBox() || layoutBox.isFloatingPositioned() || layoutBox.isDocumentBox() || layoutBox.isTableBox());

    // 10.6.6 Complicated cases
    //
    // Block-level, non-replaced elements in normal flow when 'overflow' does not compute to 'visible' (except if the 'overflow' property's value has been propagated to the viewport).
    // 'Inline-block', non-replaced elements.
    // Floating, non-replaced elements.
    //
    // 1. If 'margin-top', or 'margin-bottom' are 'auto', their used value is 0.
    // 2. If 'height' is 'auto', the height depends on the element's descendants per 10.6.7.

    auto height = usedVerticalValues.height ? usedVerticalValues.height.value() : computedContentHeight(layoutBox);
    auto computedVerticalMargin = Geometry::computedVerticalMargin(layoutBox, usedHorizontalValues);
    // #1
    auto usedVerticalMargin = UsedVerticalMargin::NonCollapsedValues { computedVerticalMargin.before.valueOr(0), computedVerticalMargin.after.valueOr(0) }; 
    // #2
    if (!height) {
        ASSERT(isHeightAuto(layoutBox));
        height = contentHeightForFormattingContextRoot(layoutBox);
    }

    ASSERT(height);

    LOG_WITH_STREAM(FormattingContextLayout, stream << "[Height][Margin] -> floating non-replaced -> height(" << *height << "px) margin(" << usedVerticalMargin.before << "px, " << usedVerticalMargin.after << "px) -> layoutBox(" << &layoutBox << ")");
    return ContentHeightAndMargin { *height, usedVerticalMargin };
}

ContentWidthAndMargin FormattingContext::Geometry::floatingNonReplacedWidthAndMargin(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues)
{
    ASSERT(layoutBox.isFloatingPositioned() && !layoutBox.replaced());

    // 10.3.5 Floating, non-replaced elements
    //
    // 1. If 'margin-left', or 'margin-right' are computed as 'auto', their used value is '0'.
    // 2. If 'width' is computed as 'auto', the used value is the "shrink-to-fit" width.

    auto computedHorizontalMargin = Geometry::computedHorizontalMargin(layoutBox, usedHorizontalValues);

    // #1
    auto usedHorizontallMargin = UsedHorizontalMargin { computedHorizontalMargin.start.valueOr(0), computedHorizontalMargin.end.valueOr(0) };
    // #2
    auto width = usedHorizontalValues.width ? usedHorizontalValues.width : computedContentWidth(layoutBox, usedHorizontalValues.constraints.width);
    if (!width)
        width = shrinkToFitWidth(layoutBox, usedHorizontalValues.constraints.width);

    LOG_WITH_STREAM(FormattingContextLayout, stream << "[Width][Margin] -> floating non-replaced -> width(" << *width << "px) margin(" << usedHorizontallMargin.start << "px, " << usedHorizontallMargin.end << "px) -> layoutBox(" << &layoutBox << ")");
    return ContentWidthAndMargin { *width, usedHorizontallMargin, computedHorizontalMargin };
}

ContentHeightAndMargin FormattingContext::Geometry::floatingReplacedHeightAndMargin(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues, const UsedVerticalValues& usedVerticalValues) const
{
    ASSERT(layoutBox.isFloatingPositioned() && layoutBox.replaced());

    // 10.6.2 Inline replaced elements, block-level replaced elements in normal flow, 'inline-block'
    // replaced elements in normal flow and floating replaced elements
    LOG_WITH_STREAM(FormattingContextLayout, stream << "[Height][Margin] -> floating replaced -> redirected to inline replaced");
    return inlineReplacedHeightAndMargin(layoutBox, usedHorizontalValues, usedVerticalValues);
}

ContentWidthAndMargin FormattingContext::Geometry::floatingReplacedWidthAndMargin(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues) const
{
    ASSERT(layoutBox.isFloatingPositioned() && layoutBox.replaced());

    // 10.3.6 Floating, replaced elements
    //
    // 1. If 'margin-left' or 'margin-right' are computed as 'auto', their used value is '0'.
    // 2. The used value of 'width' is determined as for inline replaced elements.
    auto computedHorizontalMargin = Geometry::computedHorizontalMargin(layoutBox, usedHorizontalValues);

    LOG_WITH_STREAM(FormattingContextLayout, stream << "[Height][Margin] -> floating replaced -> redirected to inline replaced");
    auto usedMargin = UsedHorizontalMargin { computedHorizontalMargin.start.valueOr(0), computedHorizontalMargin.end.valueOr(0) };
    return inlineReplacedWidthAndMargin(layoutBox, UsedHorizontalValues { usedHorizontalValues.constraints, usedHorizontalValues.width, usedMargin });
}

VerticalGeometry FormattingContext::Geometry::outOfFlowVerticalGeometry(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues, const UsedVerticalValues& usedVerticalValues) const
{
    ASSERT(layoutBox.isOutOfFlowPositioned());

    if (!layoutBox.replaced())
        return outOfFlowNonReplacedVerticalGeometry(layoutBox, usedHorizontalValues, usedVerticalValues);
    return outOfFlowReplacedVerticalGeometry(layoutBox, usedHorizontalValues, usedVerticalValues);
}

HorizontalGeometry FormattingContext::Geometry::outOfFlowHorizontalGeometry(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues, const UsedVerticalValues& usedVerticalValues)
{
    ASSERT(layoutBox.isOutOfFlowPositioned());

    if (!layoutBox.replaced())
        return outOfFlowNonReplacedHorizontalGeometry(layoutBox, usedHorizontalValues);
    return outOfFlowReplacedHorizontalGeometry(layoutBox, usedHorizontalValues, usedVerticalValues);
}

ContentHeightAndMargin FormattingContext::Geometry::floatingHeightAndMargin(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues, const UsedVerticalValues& usedVerticalValues) const
{
    ASSERT(layoutBox.isFloatingPositioned());

    if (!layoutBox.replaced())
        return complicatedCases(layoutBox, usedHorizontalValues, usedVerticalValues);
    return floatingReplacedHeightAndMargin(layoutBox, usedHorizontalValues, usedVerticalValues);
}

ContentWidthAndMargin FormattingContext::Geometry::floatingWidthAndMargin(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues)
{
    ASSERT(layoutBox.isFloatingPositioned());

    if (!layoutBox.replaced())
        return floatingNonReplacedWidthAndMargin(layoutBox, usedHorizontalValues);
    return floatingReplacedWidthAndMargin(layoutBox, usedHorizontalValues);
}

ContentHeightAndMargin FormattingContext::Geometry::inlineReplacedHeightAndMargin(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues, const UsedVerticalValues& usedVerticalValues) const
{
    ASSERT((layoutBox.isOutOfFlowPositioned() || layoutBox.isFloatingPositioned() || layoutBox.isInFlow()) && layoutBox.replaced());

    // 10.6.2 Inline replaced elements, block-level replaced elements in normal flow, 'inline-block' replaced elements in normal flow and floating replaced elements
    //
    // 1. If 'margin-top', or 'margin-bottom' are 'auto', their used value is 0.
    // 2. If 'height' and 'width' both have computed values of 'auto' and the element also has an intrinsic height, then that intrinsic height is the used value of 'height'.
    // 3. Otherwise, if 'height' has a computed value of 'auto', and the element has an intrinsic ratio then the used value of 'height' is:
    //    (used width) / (intrinsic ratio)
    // 4. Otherwise, if 'height' has a computed value of 'auto', and the element has an intrinsic height, then that intrinsic height is the used value of 'height'.
    // 5. Otherwise, if 'height' has a computed value of 'auto', but none of the conditions above are met, then the used value of 'height' must be set to
    //    the height of the largest rectangle that has a 2:1 ratio, has a height not greater than 150px, and has a width not greater than the device width.

    // #1
    auto& formattingContext = this->formattingContext();
    auto computedVerticalMargin = Geometry::computedVerticalMargin(layoutBox, usedHorizontalValues);
    auto usedVerticalMargin = UsedVerticalMargin::NonCollapsedValues { computedVerticalMargin.before.valueOr(0), computedVerticalMargin.after.valueOr(0) };
    auto& style = layoutBox.style();
    auto replaced = layoutBox.replaced();

    auto height = usedVerticalValues.height ? usedVerticalValues.height.value() : computedContentHeight(layoutBox, usedVerticalValues.constraints.height);
    auto heightIsAuto = !usedVerticalValues.height && isHeightAuto(layoutBox);
    auto widthIsAuto = style.logicalWidth().isAuto();

    if (heightIsAuto && widthIsAuto && replaced->hasIntrinsicHeight()) {
        // #2
        height = replaced->intrinsicHeight();
    } else if (heightIsAuto && replaced->hasIntrinsicRatio()) {
        // #3
        auto usedWidth = formattingContext.geometryForBox(layoutBox).width();
        height = usedWidth / replaced->intrinsicRatio();
    } else if (heightIsAuto && replaced->hasIntrinsicHeight()) {
        // #4
        height = replaced->intrinsicHeight();
    } else if (heightIsAuto) {
        // #5
        height = { 150 };
    }

    ASSERT(height);

    LOG_WITH_STREAM(FormattingContextLayout, stream << "[Height][Margin] -> inflow replaced -> height(" << *height << "px) margin(" << usedVerticalMargin.before << "px, " << usedVerticalMargin.after << "px) -> layoutBox(" << &layoutBox << ")");
    return { *height, usedVerticalMargin };
}

ContentWidthAndMargin FormattingContext::Geometry::inlineReplacedWidthAndMargin(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues, Optional<UsedVerticalValues> usedVerticalValues) const
{
    ASSERT((layoutBox.isOutOfFlowPositioned() || layoutBox.isFloatingPositioned() || layoutBox.isInFlow()) && layoutBox.replaced());

    // 10.3.2 Inline, replaced elements
    //
    // A computed value of 'auto' for 'margin-left' or 'margin-right' becomes a used value of '0'.
    //
    // 1. If 'height' and 'width' both have computed values of 'auto' and the element also has an intrinsic width, then that intrinsic width is the used value of 'width'.
    //
    // 2. If 'height' and 'width' both have computed values of 'auto' and the element has no intrinsic width, but does have an intrinsic height and intrinsic ratio;
    //    or if 'width' has a computed value of 'auto', 'height' has some other computed value, and the element does have an intrinsic ratio;
    //    then the used value of 'width' is: (used height) * (intrinsic ratio)
    //
    // 3. If 'height' and 'width' both have computed values of 'auto' and the element has an intrinsic ratio but no intrinsic height or width,
    //    then the used value of 'width' is undefined in CSS 2.2. However, it is suggested that, if the containing block's width does not itself depend on the replaced
    //    element's width, then the used value of 'width' is calculated from the constraint equation used for block-level, non-replaced elements in normal flow.
    //
    // 4. Otherwise, if 'width' has a computed value of 'auto', and the element has an intrinsic width, then that intrinsic width is the used value of 'width'.
    //
    // 5. Otherwise, if 'width' has a computed value of 'auto', but none of the conditions above are met, then the used value of 'width' becomes 300px.
    //    If 300px is too wide to fit the device, UAs should use the width of the largest rectangle that has a 2:1 ratio and fits the device instead.

    auto computedHorizontalMargin = Geometry::computedHorizontalMargin(layoutBox, usedHorizontalValues);

    auto usedMarginStart = [&] {
        if (usedHorizontalValues.margin)
            return usedHorizontalValues.margin->start;
        return computedHorizontalMargin.start.valueOr(0_lu);
    };

    auto usedMarginEnd = [&] {
        if (usedHorizontalValues.margin)
            return usedHorizontalValues.margin->end;
        return computedHorizontalMargin.end.valueOr(0_lu);
    };

    auto replaced = layoutBox.replaced();
    ASSERT(replaced);

    auto width = usedHorizontalValues.width ? usedHorizontalValues.width : computedContentWidth(layoutBox, usedHorizontalValues.constraints.width);
    auto heightIsAuto = isHeightAuto(layoutBox);
    auto height = computedContentHeight(layoutBox, usedVerticalValues ? usedVerticalValues->constraints.height : WTF::nullopt);

    if (!width && heightIsAuto && replaced->hasIntrinsicWidth()) {
        // #1
        width = replaced->intrinsicWidth();
    } else if ((!width && heightIsAuto && !replaced->hasIntrinsicWidth() && replaced->hasIntrinsicHeight() && replaced->hasIntrinsicRatio())
        || (!width && height && replaced->hasIntrinsicRatio())) {
        // #2
        width = height.valueOr(replaced->hasIntrinsicHeight()) * replaced->intrinsicRatio();
    } else if (!width && heightIsAuto && replaced->hasIntrinsicRatio() && !replaced->hasIntrinsicWidth() && !replaced->hasIntrinsicHeight()) {
        // #3
        // FIXME: undefined but surely doable.
        ASSERT_NOT_IMPLEMENTED_YET();
    } else if (!width && replaced->hasIntrinsicWidth()) {
        // #4
        width = replaced->intrinsicWidth();
    } else if (!width) {
        // #5
        width = { 300 };
    }

    ASSERT(width);

    LOG_WITH_STREAM(FormattingContextLayout, stream << "[Width][Margin] -> inflow replaced -> width(" << *width << "px) margin(" << usedMarginStart() << "px, " << usedMarginEnd() << "px) -> layoutBox(" << &layoutBox << ")");
    return { *width, { usedMarginStart(), usedMarginEnd() }, computedHorizontalMargin };
}

LayoutSize FormattingContext::Geometry::inFlowPositionedPositionOffset(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues) const
{
    ASSERT(layoutBox.isInFlowPositioned());

    // 9.4.3 Relative positioning
    //
    // The 'top' and 'bottom' properties move relatively positioned element(s) up or down without changing their size.
    // Top' moves the boxes down, and 'bottom' moves them up. Since boxes are not split or stretched as a result of 'top' or 'bottom', the used values are always: top = -bottom.
    //
    // 1. If both are 'auto', their used values are both '0'.
    // 2. If one of them is 'auto', it becomes the negative of the other.
    // 3. If neither is 'auto', 'bottom' is ignored (i.e., the used value of 'bottom' will be minus the value of 'top').

    auto& style = layoutBox.style();
    auto containingBlockWidth = usedHorizontalValues.constraints.width;

    auto top = computedValueIfNotAuto(style.logicalTop(), containingBlockWidth);
    auto bottom = computedValueIfNotAuto(style.logicalBottom(), containingBlockWidth);

    if (!top && !bottom) {
        // #1
        top = bottom = { 0 };
    } else if (!top) {
        // #2
        top = -*bottom;
    } else if (!bottom) {
        // #3
        bottom = -*top;
    } else {
        // #4
        bottom = WTF::nullopt;
    }

    // For relatively positioned elements, 'left' and 'right' move the box(es) horizontally, without changing their size.
    // 'Left' moves the boxes to the right, and 'right' moves them to the left.
    // Since boxes are not split or stretched as a result of 'left' or 'right', the used values are always: left = -right.
    //
    // 1. If both 'left' and 'right' are 'auto' (their initial values), the used values are '0' (i.e., the boxes stay in their original position).
    // 2. If 'left' is 'auto', its used value is minus the value of 'right' (i.e., the boxes move to the left by the value of 'right').
    // 3. If 'right' is specified as 'auto', its used value is minus the value of 'left'.
    // 4. If neither 'left' nor 'right' is 'auto', the position is over-constrained, and one of them has to be ignored.
    //    If the 'direction' property of the containing block is 'ltr', the value of 'left' wins and 'right' becomes -'left'.
    //    If 'direction' of the containing block is 'rtl', 'right' wins and 'left' is ignored.

    auto left = computedValueIfNotAuto(style.logicalLeft(), containingBlockWidth);
    auto right = computedValueIfNotAuto(style.logicalRight(), containingBlockWidth);

    if (!left && !right) {
        // #1
        left = right = { 0 };
    } else if (!left) {
        // #2
        left = -*right;
    } else if (!right) {
        // #3
        right = -*left;
    } else {
        // #4
        auto isLeftToRightDirection = layoutBox.containingBlock()->style().isLeftToRightDirection();
        if (isLeftToRightDirection)
            right = -*left;
        else
            left = WTF::nullopt;
    }

    ASSERT(!bottom || *top == -*bottom);
    ASSERT(!left || *left == -*right);

    auto topPositionOffset = *top;
    auto leftPositionOffset = left.valueOr(-*right);

    LOG_WITH_STREAM(FormattingContextLayout, stream << "[Position] -> positioned inflow -> top offset(" << topPositionOffset << "px) left offset(" << leftPositionOffset << "px) layoutBox(" << &layoutBox << ")");
    return { leftPositionOffset, topPositionOffset };
}

Edges FormattingContext::Geometry::computedBorder(const Box& layoutBox) const
{
    auto& style = layoutBox.style();
    LOG_WITH_STREAM(FormattingContextLayout, stream << "[Border] -> layoutBox: " << &layoutBox);
    return {
        { LayoutUnit(style.borderLeft().boxModelWidth()), LayoutUnit(style.borderRight().boxModelWidth()) },
        { LayoutUnit(style.borderTop().boxModelWidth()), LayoutUnit(style.borderBottom().boxModelWidth()) }
    };
}

Optional<Edges> FormattingContext::Geometry::computedPadding(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues) const
{
    if (!layoutBox.isPaddingApplicable())
        return WTF::nullopt;

    auto& style = layoutBox.style();
    auto containingBlockWidth = usedHorizontalValues.constraints.width;
    LOG_WITH_STREAM(FormattingContextLayout, stream << "[Padding] -> layoutBox: " << &layoutBox);
    return Edges {
        { valueForLength(style.paddingLeft(), containingBlockWidth), valueForLength(style.paddingRight(), containingBlockWidth) },
        { valueForLength(style.paddingTop(), containingBlockWidth), valueForLength(style.paddingBottom(), containingBlockWidth) }
    };
}

ComputedHorizontalMargin FormattingContext::Geometry::computedHorizontalMargin(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues) const
{
    auto& style = layoutBox.style();
    auto containingBlockWidth = usedHorizontalValues.constraints.width;
    return { computedValueIfNotAuto(style.marginStart(), containingBlockWidth), computedValueIfNotAuto(style.marginEnd(), containingBlockWidth) };
}

ComputedVerticalMargin FormattingContext::Geometry::computedVerticalMargin(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues) const
{
    auto& style = layoutBox.style();
    auto containingBlockWidth = usedHorizontalValues.constraints.width;
    return { computedValueIfNotAuto(style.marginBefore(), containingBlockWidth), computedValueIfNotAuto(style.marginAfter(), containingBlockWidth) };
}

FormattingContext::IntrinsicWidthConstraints FormattingContext::Geometry::constrainByMinMaxWidth(const Box& layoutBox, IntrinsicWidthConstraints intrinsicWidth) const
{
    auto& style = layoutBox.style();
    auto minWidth = fixedValue(style.logicalMinWidth());
    auto maxWidth = fixedValue(style.logicalMaxWidth());
    if (!minWidth && !maxWidth)
        return intrinsicWidth;

    if (maxWidth) {
        intrinsicWidth.minimum = std::min(*maxWidth, intrinsicWidth.minimum);
        intrinsicWidth.maximum = std::min(*maxWidth, intrinsicWidth.maximum);
    }

    if (minWidth) {
        intrinsicWidth.minimum = std::max(*minWidth, intrinsicWidth.minimum);
        intrinsicWidth.maximum = std::max(*minWidth, intrinsicWidth.maximum);
    }

    ASSERT(intrinsicWidth.minimum <= intrinsicWidth.maximum);
    return intrinsicWidth;
}

}
}
#endif
