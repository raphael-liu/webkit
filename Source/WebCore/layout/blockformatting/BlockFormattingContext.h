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

#pragma once

#if ENABLE(LAYOUT_FORMATTING_CONTEXT)

#include "FormattingContext.h"
#include <wtf/HashMap.h>
#include <wtf/IsoMalloc.h>

namespace WebCore {

class LayoutUnit;

namespace Layout {

class BlockFormattingState;
class Box;
class FloatingContext;

// This class implements the layout logic for block formatting contexts.
// https://www.w3.org/TR/CSS22/visuren.html#block-formatting
class BlockFormattingContext : public FormattingContext {
    WTF_MAKE_ISO_ALLOCATED(BlockFormattingContext);
public:
    BlockFormattingContext(const Container& formattingContextRoot, BlockFormattingState&);

    void layoutInFlowContent(InvalidationState&) override;

private:
    void layoutFormattingContextRoot(FloatingContext&, const Box&, InvalidationState&);
    void placeInFlowPositionedChildren(const Box&);

    void computeWidthAndMargin(const Box&, Optional<LayoutUnit> usedAvailableWidth = { });
    void computeHeightAndMargin(const Box&);

    void computeStaticHorizontalPosition(const Box&);
    void computeStaticVerticalPosition(const FloatingContext&, const Box&);
    void computeStaticPosition(const FloatingContext&, const Box&);
    void computeFloatingPosition(const FloatingContext&, const Box&);
    void computePositionToAvoidFloats(const FloatingContext&, const Box&);

    void computeEstimatedVerticalPosition(const Box&);
    void computeEstimatedVerticalPositionForAncestors(const Box&);
    void computeEstimatedVerticalPositionForFormattingRoot(const Box&);
    void computeEstimatedVerticalPositionForFloatClear(const FloatingContext&, const Box&);

    IntrinsicWidthConstraints computedIntrinsicWidthConstraints() override;
    LayoutUnit verticalPositionWithMargin(const Box&, const UsedVerticalMargin&) const;

    // This class implements positioning and sizing for boxes participating in a block formatting context.
    class Geometry : public FormattingContext::Geometry {
    public:
        ContentHeightAndMargin inFlowHeightAndMargin(const Box&, const UsedHorizontalValues&, const UsedVerticalValues&);
        ContentWidthAndMargin inFlowWidthAndMargin(const Box&, const UsedHorizontalValues&);

        Point staticPosition(const Box&, const UsedHorizontalValues&, const UsedVerticalValues&) const;
        LayoutUnit staticVerticalPosition(const Box&, const UsedVerticalValues&) const;
        LayoutUnit staticHorizontalPosition(const Box&, const UsedHorizontalValues&) const;

        IntrinsicWidthConstraints intrinsicWidthConstraints(const Box&);

    private:
        friend class BlockFormattingContext;
        Geometry(const BlockFormattingContext&);

        ContentHeightAndMargin inFlowNonReplacedHeightAndMargin(const Box&, const UsedHorizontalValues&, const UsedVerticalValues&);
        ContentWidthAndMargin inFlowNonReplacedWidthAndMargin(const Box&, const UsedHorizontalValues&) const;
        ContentWidthAndMargin inFlowReplacedWidthAndMargin(const Box&, const UsedHorizontalValues&) const;
        Point staticPositionForOutOfFlowPositioned(const Box&) const;

        const BlockFormattingContext& formattingContext() const { return downcast<BlockFormattingContext>(FormattingContext::Geometry::formattingContext()); }
    };
    BlockFormattingContext::Geometry geometry() const { return Geometry(*this); }

    // This class implements margin collapsing for block formatting context.
    class MarginCollapse {
    public:
        UsedVerticalMargin::CollapsedValues collapsedVerticalValues(const Box&, UsedVerticalMargin::NonCollapsedValues);

        EstimatedMarginBefore estimatedMarginBefore(const Box&, UsedVerticalMargin::NonCollapsedValues);
        LayoutUnit marginBeforeIgnoringCollapsingThrough(const Box&, UsedVerticalMargin::NonCollapsedValues);
        static void updateMarginAfterForPreviousSibling(BlockFormattingContext&, const MarginCollapse&, const Box&);
        static void updatePositiveNegativeMarginValues(BlockFormattingContext&, const MarginCollapse&, const Box&);

        bool marginBeforeCollapsesWithParentMarginBefore(const Box&) const;
        bool marginBeforeCollapsesWithFirstInFlowChildMarginBefore(const Box&) const;
        bool marginBeforeCollapsesWithParentMarginAfter(const Box&) const;
        bool marginBeforeCollapsesWithPreviousSiblingMarginAfter(const Box&) const;

        bool marginAfterCollapsesWithParentMarginAfter(const Box&) const;
        bool marginAfterCollapsesWithLastInFlowChildMarginAfter(const Box&) const;
        bool marginAfterCollapsesWithParentMarginBefore(const Box&) const;
        bool marginAfterCollapsesWithNextSiblingMarginBefore(const Box&) const;
        bool marginAfterCollapsesWithSiblingMarginBeforeWithClearance(const Box&) const;

        bool marginsCollapseThrough(const Box&) const;

    private:
        friend class BlockFormattingContext;
        MarginCollapse(const BlockFormattingContext&);

        enum class MarginType { Before, After };
        PositiveAndNegativeVerticalMargin::Values positiveNegativeValues(const Box&, MarginType) const;
        PositiveAndNegativeVerticalMargin::Values positiveNegativeMarginBefore(const Box&, UsedVerticalMargin::NonCollapsedValues) const;
        PositiveAndNegativeVerticalMargin::Values positiveNegativeMarginAfter(const Box&, UsedVerticalMargin::NonCollapsedValues) const;
        bool hasClearance(const Box&) const;

        LayoutState& layoutState() { return m_blockFormattingContext.layoutState(); }
        const LayoutState& layoutState() const { return m_blockFormattingContext.layoutState(); }
        const BlockFormattingContext& formattingContext() const { return m_blockFormattingContext; }

        const BlockFormattingContext& m_blockFormattingContext;
    };
    MarginCollapse marginCollapse() const { return MarginCollapse(*this); }

    class Quirks : public FormattingContext::Quirks {
    public:
        bool needsStretching(const Box&) const;
        ContentHeightAndMargin stretchedInFlowHeight(const Box&, ContentHeightAndMargin);

        bool shouldIgnoreCollapsedQuirkMargin(const Box&) const;
        bool shouldIgnoreMarginBefore(const Box&) const;
        bool shouldIgnoreMarginAfter(const Box&) const;

    private:
        friend class BlockFormattingContext;
        Quirks(const BlockFormattingContext&);

        const BlockFormattingContext& formattingContext() const { return downcast<BlockFormattingContext>(FormattingContext::Quirks::formattingContext()); }

    };
    BlockFormattingContext::Quirks quirks() const { return Quirks(*this); }

    void setEstimatedMarginBefore(const Box&, const EstimatedMarginBefore&);
    void removeEstimatedMarginBefore(const Box& layoutBox) { m_estimatedMarginBeforeList.remove(&layoutBox); }
    bool hasEstimatedMarginBefore(const Box&) const;
    Optional<LayoutUnit> usedAvailableWidthForFloatAvoider(const FloatingContext&, const Box&) const;
#ifndef NDEBUG
    EstimatedMarginBefore estimatedMarginBefore(const Box& layoutBox) const { return m_estimatedMarginBeforeList.get(&layoutBox); }
    bool hasPrecomputedMarginBefore(const Box&) const;
#endif

    const BlockFormattingState& formattingState() const { return downcast<BlockFormattingState>(FormattingContext::formattingState()); }
    BlockFormattingState& formattingState() { return downcast<BlockFormattingState>(FormattingContext::formattingState()); }

private:
    HashMap<const Box*, EstimatedMarginBefore> m_estimatedMarginBeforeList;
};

inline BlockFormattingContext::Geometry::Geometry(const BlockFormattingContext& blockFormattingContext)
    : FormattingContext::Geometry(blockFormattingContext)
{
}

inline BlockFormattingContext::Quirks::Quirks(const BlockFormattingContext& blockFormattingContext)
    : FormattingContext::Quirks(blockFormattingContext)
{
}

inline BlockFormattingContext::MarginCollapse::MarginCollapse(const BlockFormattingContext& blockFormattingContext)
    : m_blockFormattingContext(blockFormattingContext)
{
}

}
}

SPECIALIZE_TYPE_TRAITS_LAYOUT_FORMATTING_CONTEXT(BlockFormattingContext, isBlockFormattingContext())

#endif
