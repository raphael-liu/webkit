/*
 * Copyright (C) 2019 Apple Inc. All rights reserved.
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
#include "InlineLineBuilder.h"

#if ENABLE(LAYOUT_FORMATTING_CONTEXT)

#include "InlineFormattingContext.h"
#include "TextUtil.h"
#include <wtf/IsoMallocInlines.h>

namespace WebCore {
namespace Layout {

class InlineItemRun {
WTF_MAKE_ISO_ALLOCATED_INLINE(InlineItemRun);
public:
    InlineItemRun(const InlineItem&, const Display::Rect&, WTF::Optional<Display::Run::TextContext> = WTF::nullopt);

    const Box& layoutBox() const { return m_inlineItem.layoutBox(); }
    const Display::Rect& logicalRect() const { return m_logicalRect; }
    Optional<Display::Run::TextContext> textContext() const { return m_textContext; }

    bool isText() const { return m_inlineItem.isText(); }
    bool isBox() const { return m_inlineItem.isBox(); }
    bool isContainerStart() const { return m_inlineItem.isContainerStart(); }
    bool isContainerEnd() const { return m_inlineItem.isContainerEnd(); }
    bool isForcedLineBreak() const { return m_inlineItem.isForcedLineBreak(); }
    InlineItem::Type type() const { return m_inlineItem.type(); }

    void setIsCollapsed() { m_isCollapsed = true; }
    bool isCollapsed() const { return m_isCollapsed; }

    void setCollapsesToZeroAdvanceWidth();
    bool isCollapsedToZeroAdvanceWidth() const { return m_collapsedToZeroAdvanceWidth; }

    bool isCollapsible() const { return is<InlineTextItem>(m_inlineItem) && downcast<InlineTextItem>(m_inlineItem).isCollapsible(); }
    bool isWhitespace() const { return is<InlineTextItem>(m_inlineItem) && downcast<InlineTextItem>(m_inlineItem).isWhitespace(); }

    bool hasExpansionOpportunity() const { return isWhitespace() && !isCollapsedToZeroAdvanceWidth(); }

private:
    const InlineItem& m_inlineItem;
    Display::Rect m_logicalRect;
    const Optional<Display::Run::TextContext> m_textContext;
    bool m_isCollapsed { false };
    bool m_collapsedToZeroAdvanceWidth { false };
};

InlineItemRun::InlineItemRun(const InlineItem& inlineItem, const Display::Rect& logicalRect, WTF::Optional<Display::Run::TextContext> textContext)
    : m_inlineItem(inlineItem)
    , m_logicalRect(logicalRect)
    , m_textContext(textContext)
{
}

void InlineItemRun::setCollapsesToZeroAdvanceWidth()
{
    m_collapsedToZeroAdvanceWidth = true;
    m_logicalRect.setWidth({ });
}

struct ContinousContent {
public:
    ContinousContent(const InlineItemRun&, bool textIsAlignJustify);

    bool append(const InlineItemRun&);
    LineBuilder::Run close();

private:
    static bool canBeExpanded(const InlineItemRun& run) { return run.isText() && !run.isCollapsed() && !run.isCollapsedToZeroAdvanceWidth(); }
    bool canBeMerged(const InlineItemRun& run) const { return run.isText() && !run.isCollapsedToZeroAdvanceWidth() && &m_initialInlineRun.layoutBox() == &run.layoutBox(); }

    const InlineItemRun& m_initialInlineRun;
    const bool m_textIsAlignJustify { false };
    unsigned m_expandedLength { 0 };
    LayoutUnit m_expandedWidth;
    bool m_trailingRunCanBeExpanded { false };
    bool m_hasTrailingExpansionOpportunity { false };
    unsigned m_expansionOpportunityCount { 0 };
};

ContinousContent::ContinousContent(const InlineItemRun& initialInlineRun, bool textIsAlignJustify)
    : m_initialInlineRun(initialInlineRun)
    , m_textIsAlignJustify(textIsAlignJustify)
    , m_trailingRunCanBeExpanded(canBeExpanded(initialInlineRun))
{
}

bool ContinousContent::append(const InlineItemRun& inlineItemRun)
{
    // Merged content needs to be continuous.
    if (!m_trailingRunCanBeExpanded)
        return false;
    if (!canBeMerged(inlineItemRun))
        return false;

    m_trailingRunCanBeExpanded = canBeExpanded(inlineItemRun);

    ASSERT(inlineItemRun.isText());
    m_expandedLength += inlineItemRun.textContext()->length();
    m_expandedWidth += inlineItemRun.logicalRect().width();

    if (m_textIsAlignJustify) {
        m_hasTrailingExpansionOpportunity = inlineItemRun.hasExpansionOpportunity();
        if (m_hasTrailingExpansionOpportunity)
            ++m_expansionOpportunityCount;
    }
    return true;
}

LineBuilder::Run ContinousContent::close()
{
    if (!m_expandedLength)
        return { m_initialInlineRun };
    // Expand the text content and set the expansion opportunities.
    ASSERT(m_initialInlineRun.isText());
    auto logicalRect = m_initialInlineRun.logicalRect();
    logicalRect.expandHorizontally(m_expandedWidth);

    auto textContext = *m_initialInlineRun.textContext();
    auto length = textContext.length() + m_expandedLength;
    textContext.expand(m_initialInlineRun.layoutBox().textContext()->content.substring(textContext.start(), length), length);

    if (m_textIsAlignJustify) {
        // FIXME: This is a very simple expansion merge. We should eventually switch over to FontCascade::expansionOpportunityCount.
        ExpansionBehavior expansionBehavior = m_hasTrailingExpansionOpportunity ? (ForbidLeadingExpansion | AllowTrailingExpansion) : (AllowLeadingExpansion | AllowTrailingExpansion);
        if (m_initialInlineRun.hasExpansionOpportunity())
            ++m_expansionOpportunityCount;
        textContext.setExpansion({ expansionBehavior, { } });
    }
    return { m_initialInlineRun, logicalRect, textContext, m_expansionOpportunityCount };
}

LineBuilder::Run::Run(const InlineItemRun& inlineItemRun)
    : m_layoutBox(&inlineItemRun.layoutBox())
    , m_type(inlineItemRun.type())
    , m_logicalRect(inlineItemRun.logicalRect())
    , m_textContext(inlineItemRun.textContext())
    , m_isCollapsedToVisuallyEmpty(inlineItemRun.isCollapsedToZeroAdvanceWidth())
{
    if (inlineItemRun.hasExpansionOpportunity()) {
        m_expansionOpportunityCount = 1;
        ASSERT(m_textContext);
        m_textContext->setExpansion({ DefaultExpansion, { } });
    }
}

LineBuilder::Run::Run(const InlineItemRun& inlineItemRun, const Display::Rect& logicalRect, const Display::Run::TextContext& textContext, unsigned expansionOpportunityCount)
    : m_layoutBox(&inlineItemRun.layoutBox())
    , m_type(inlineItemRun.type())
    , m_logicalRect(logicalRect)
    , m_textContext(textContext)
    , m_expansionOpportunityCount(expansionOpportunityCount)
    , m_isCollapsedToVisuallyEmpty(inlineItemRun.isCollapsedToZeroAdvanceWidth())
{
}

void LineBuilder::Run::adjustExpansionBehavior(ExpansionBehavior expansionBehavior)
{
    ASSERT(isText());
    ASSERT(hasExpansionOpportunity());
    m_textContext->setExpansion({ expansionBehavior, m_textContext->expansion()->horizontalExpansion });
}

inline Optional<ExpansionBehavior> LineBuilder::Run::expansionBehavior() const
{
    ASSERT(isText());
    if (auto expansionContext = m_textContext->expansion())
        return expansionContext->behavior;
    return { };
}

void LineBuilder::Run::setComputedHorizontalExpansion(LayoutUnit logicalExpansion)
{
    ASSERT(isText());
    ASSERT(hasExpansionOpportunity());
    m_logicalRect.expandHorizontally(logicalExpansion);
    m_textContext->setExpansion({ m_textContext->expansion()->behavior, logicalExpansion });
}

LineBuilder::LineBuilder(const InlineFormattingContext& inlineFormattingContext, Optional<TextAlignMode> horizontalAlignment, SkipAlignment skipAlignment)
    : m_inlineFormattingContext(inlineFormattingContext)
    , m_horizontalAlignment(horizontalAlignment)
    , m_skipAlignment(skipAlignment == SkipAlignment::Yes)
{
}

LineBuilder::~LineBuilder()
{
}

void LineBuilder::initialize(const Constraints& constraints)
{
    ASSERT(m_skipAlignment || constraints.heightAndBaseline);

    LayoutUnit initialLineHeight;
    LayoutUnit initialBaselineOffset;
    if (constraints.heightAndBaseline) {
        m_initialStrut = constraints.heightAndBaseline->strut;
        initialLineHeight = constraints.heightAndBaseline->height;
        initialBaselineOffset = constraints.heightAndBaseline->baselineOffset;
    } else
        m_initialStrut = { };

    auto lineRect = Display::Rect { constraints.logicalTopLeft, { }, initialLineHeight };
    auto baseline = LineBox::Baseline { initialBaselineOffset, initialLineHeight - initialBaselineOffset };
    m_lineBox = LineBox { lineRect, baseline, initialBaselineOffset };
    m_lineLogicalWidth = constraints.availableLogicalWidth;
    m_hasIntrusiveFloat = constraints.lineIsConstrainedByFloat;

    m_inlineItemRuns.clear();
    m_trimmableContent.clear();
}

static bool shouldPreserveTrailingContent(const InlineTextItem& inlineTextItem)
{
    if (!inlineTextItem.isWhitespace())
        return true;
    auto whitespace = inlineTextItem.style().whiteSpace();
    return whitespace == WhiteSpace::Pre || whitespace == WhiteSpace::PreWrap;
}

static bool shouldPreserveLeadingContent(const InlineTextItem& inlineTextItem)
{
    if (!inlineTextItem.isWhitespace())
        return true;
    auto whitespace = inlineTextItem.style().whiteSpace();
    return whitespace == WhiteSpace::Pre || whitespace == WhiteSpace::PreWrap || whitespace == WhiteSpace::BreakSpaces;
}

LineBuilder::RunList LineBuilder::close(IsLastLineWithInlineContent isLastLineWithInlineContent)
{
    // 1. Remove trimmable trailing content.
    // 2. Join text runs together when possible [foo][ ][bar] -> [foo bar].
    // 3. Align merged runs both vertically and horizontally.
    removeTrailingTrimmableContent();
    RunList runList;
    unsigned runIndex = 0;
    while (runIndex < m_inlineItemRuns.size()) {
        // Merge eligible runs.
        auto continousContent = ContinousContent { *m_inlineItemRuns[runIndex], isTextAlignJustify() };
        while (++runIndex < m_inlineItemRuns.size()) {
            if (!continousContent.append(*m_inlineItemRuns[runIndex]))
                break;
        }
        runList.append(continousContent.close());
    }

    if (!m_skipAlignment) {
        if (isVisuallyEmpty()) {
            m_lineBox.resetBaseline();
            m_lineBox.setLogicalHeight({ });
        }
        // Remove descent when all content is baseline aligned but none of them have descent.
        if (formattingContext().quirks().lineDescentNeedsCollapsing(runList)) {
            m_lineBox.shrinkVertically(m_lineBox.baseline().descent());
            m_lineBox.resetDescent();
        }
        alignContentVertically(runList);
        alignContentHorizontally(runList, isLastLineWithInlineContent);
    }
    return runList;
}

void LineBuilder::alignContentVertically(RunList& runList)
{
    ASSERT(!m_skipAlignment);
    for (auto& run : runList) {
        adjustBaselineAndLineHeight(run);
        run.setLogicalHeight(runContentHeight(run));
    }

    for (auto& run : runList) {
        LayoutUnit logicalTop;
        auto& layoutBox = run.layoutBox();
        auto verticalAlign = layoutBox.style().verticalAlign();
        auto ascent = layoutBox.style().fontMetrics().ascent();

        switch (verticalAlign) {
        case VerticalAlign::Baseline:
            if (run.isForcedLineBreak() || run.isText())
                logicalTop = baselineOffset() - ascent;
            else if (run.isContainerStart()) {
                auto& boxGeometry = formattingContext().geometryForBox(layoutBox);
                logicalTop = baselineOffset() - ascent - boxGeometry.borderTop() - boxGeometry.paddingTop().valueOr(0);
            } else if (layoutBox.isInlineBlockBox() && layoutBox.establishesInlineFormattingContext()) {
                auto& formattingState = downcast<InlineFormattingState>(layoutState().establishedFormattingState(downcast<Container>(layoutBox)));
                // Spec makes us generate at least one line -even if it is empty.
                ASSERT(!formattingState.lineBoxes().isEmpty());
                auto inlineBlockBaselineOffset = formattingState.lineBoxes().last()->baselineOffset();
                // The inline-block's baseline offset is relative to its content box. Let's convert it relative to the margin box.
                //   inline-block
                //              \
                //           _______________ <- margin box
                //          |
                //          |  ____________  <- border box
                //          | |
                //          | |  _________  <- content box
                //          | | |   ^
                //          | | |   |  <- baseline offset
                //          | | |   |
                //     text | | |   v text
                //     -----|-|-|---------- <- baseline
                //
                auto& boxGeometry = formattingContext().geometryForBox(layoutBox);
                auto baselineOffsetFromMarginBox = boxGeometry.marginBefore() + boxGeometry.borderTop() + boxGeometry.paddingTop().valueOr(0) + inlineBlockBaselineOffset;
                logicalTop = baselineOffset() - baselineOffsetFromMarginBox;
            } else
                logicalTop = baselineOffset() - run.logicalRect().height();
            break;
        case VerticalAlign::Top:
            logicalTop = { };
            break;
        case VerticalAlign::Bottom:
            logicalTop = logicalBottom() - run.logicalRect().height();
            break;
        default:
            ASSERT_NOT_IMPLEMENTED_YET();
            break;
        }
        run.adjustLogicalTop(logicalTop);
        // Convert runs from relative to the line top/left to the formatting root's border box top/left.
        run.moveVertically(this->logicalTop());
        run.moveHorizontally(this->logicalLeft());
    }
}

void LineBuilder::justifyRuns(RunList& runList) const
{
    ASSERT(!runList.isEmpty());
    ASSERT(availableWidth() > 0);
    // Need to fix up the last run first.
    auto& lastRun = runList.last();
    if (lastRun.hasExpansionOpportunity())
        lastRun.adjustExpansionBehavior(*lastRun.expansionBehavior() | ForbidTrailingExpansion);
    // Collect the expansion opportunity numbers.
    auto expansionOpportunityCount = 0;
    for (auto& run : runList)
        expansionOpportunityCount += run.expansionOpportunityCount();
    // Nothing to distribute?
    if (!expansionOpportunityCount)
        return;
    // Distribute the extra space.
    auto expansionToDistribute = availableWidth() / expansionOpportunityCount;
    LayoutUnit accumulatedExpansion;
    for (auto& run : runList) {
        // Expand and moves runs by the accumulated expansion.
        if (!run.hasExpansionOpportunity()) {
            run.moveHorizontally(accumulatedExpansion);
            continue;
        }
        ASSERT(run.expansionOpportunityCount());
        auto computedExpansion = expansionToDistribute * run.expansionOpportunityCount();
        run.setComputedHorizontalExpansion(computedExpansion);
        run.moveHorizontally(accumulatedExpansion);
        accumulatedExpansion += computedExpansion;
    }
}

void LineBuilder::alignContentHorizontally(RunList& runList, IsLastLineWithInlineContent lastLine) const
{
    ASSERT(!m_skipAlignment);
    if (runList.isEmpty() || availableWidth() <= 0)
        return;

    if (isTextAlignJustify()) {
        // Do not justify align the last line.
        if (lastLine == IsLastLineWithInlineContent::No)
            justifyRuns(runList);
        return;
    }

    auto adjustmentForAlignment = [&]() -> Optional<LayoutUnit> {
        switch (*m_horizontalAlignment) {
        case TextAlignMode::Left:
        case TextAlignMode::WebKitLeft:
        case TextAlignMode::Start:
            return { };
        case TextAlignMode::Right:
        case TextAlignMode::WebKitRight:
        case TextAlignMode::End:
            return std::max(availableWidth(), 0_lu);
        case TextAlignMode::Center:
        case TextAlignMode::WebKitCenter:
            return std::max(availableWidth() / 2, 0_lu);
        case TextAlignMode::Justify:
            ASSERT_NOT_REACHED();
            break;
        }
        ASSERT_NOT_REACHED();
        return { };
    };

    auto adjustment = adjustmentForAlignment();
    if (!adjustment)
        return;

    for (auto& run : runList)
        run.moveHorizontally(*adjustment);
}

void LineBuilder::removeTrailingTrimmableContent()
{
    if (m_trimmableContent.isEmpty() || m_inlineItemRuns.isEmpty())
        return;

    // Collapse trimmable trailing content
    for (auto* trimmableRun : m_trimmableContent.runs()) {
        ASSERT(trimmableRun->isText());
        // FIXME: We might need to be able to differentiate between trimmed and collapsed runs.
        trimmableRun->setCollapsesToZeroAdvanceWidth();
    }
    m_lineBox.shrinkHorizontally(m_trimmableContent.width());
    m_trimmableContent.clear();
}

void LineBuilder::moveLogicalLeft(LayoutUnit delta)
{
    if (!delta)
        return;
    ASSERT(delta > 0);
    m_lineBox.moveHorizontally(delta);
    m_lineLogicalWidth -= delta;
}

void LineBuilder::moveLogicalRight(LayoutUnit delta)
{
    ASSERT(delta > 0);
    m_lineLogicalWidth -= delta;
}

void LineBuilder::append(const InlineItem& inlineItem, LayoutUnit logicalWidth)
{
    if (inlineItem.isText())
        return appendTextContent(downcast<InlineTextItem>(inlineItem), logicalWidth);
    if (inlineItem.isForcedLineBreak())
        return appendLineBreak(inlineItem);
    if (inlineItem.isContainerStart())
        return appendInlineContainerStart(inlineItem, logicalWidth);
    if (inlineItem.isContainerEnd())
        return appendInlineContainerEnd(inlineItem, logicalWidth);
    if (inlineItem.layoutBox().replaced())
        return appendReplacedInlineBox(inlineItem, logicalWidth);
    appendNonReplacedInlineBox(inlineItem, logicalWidth);
}

void LineBuilder::appendNonBreakableSpace(const InlineItem& inlineItem, const Display::Rect& logicalRect)
{
    m_inlineItemRuns.append(makeUnique<InlineItemRun>(inlineItem, logicalRect));
    m_lineBox.expandHorizontally(logicalRect.width());
    if (logicalRect.width())
        m_lineBox.setIsConsideredNonEmpty();
}

void LineBuilder::appendInlineContainerStart(const InlineItem& inlineItem, LayoutUnit logicalWidth)
{
    // This is really just a placeholder to mark the start of the inline level container <span>.
    appendNonBreakableSpace(inlineItem, Display::Rect { 0, contentLogicalWidth(), logicalWidth, { } });
}

void LineBuilder::appendInlineContainerEnd(const InlineItem& inlineItem, LayoutUnit logicalWidth)
{
    // This is really just a placeholder to mark the end of the inline level container </span>.
    appendNonBreakableSpace(inlineItem, Display::Rect { 0, contentLogicalRight(), logicalWidth, { } });
}

void LineBuilder::appendTextContent(const InlineTextItem& inlineItem, LayoutUnit logicalWidth)
{
    auto isTrimmable = !shouldPreserveTrailingContent(inlineItem);
    if (!isTrimmable)
        m_trimmableContent.clear();

    auto willCollapseCompletely = [&] {
        // Empty run.
        if (!inlineItem.length()) {
            ASSERT(!logicalWidth);
            return true;
        }
        // Leading whitespace.
        if (m_inlineItemRuns.isEmpty())
            return !shouldPreserveLeadingContent(inlineItem);

        if (!inlineItem.isCollapsible())
            return false;
        // Check if the last item is collapsed as well.
        for (auto i = m_inlineItemRuns.size(); i--;) {
            auto& run = m_inlineItemRuns[i];
            if (run->isBox())
                return false;
            // https://drafts.csswg.org/css-text-3/#white-space-phase-1
            // Any collapsible space immediately following another collapsible space—even one outside the boundary of the inline containing that space,
            // provided both spaces are within the same inline formatting context—is collapsed to have zero advance width.
            // : "<span>  </span> " <- the trailing whitespace collapses completely.
            // Not that when the inline container has preserve whitespace style, "<span style="white-space: pre">  </span> " <- this whitespace stays around.
            if (run->isText())
                return run->isCollapsible();
            ASSERT(run->isContainerStart() || run->isContainerEnd());
        }
        return true;
    };

    auto collapsedRun = inlineItem.isCollapsible() && inlineItem.length() > 1;
    auto contentStart = inlineItem.start();
    auto contentLength =  collapsedRun ? 1 : inlineItem.length();
    auto lineRun = makeUnique<InlineItemRun>(inlineItem, Display::Rect { 0, contentLogicalWidth(), logicalWidth, { } },
        Display::Run::TextContext { contentStart, contentLength, inlineItem.layoutBox().textContext()->content.substring(contentStart, contentLength) });

    auto collapsesToZeroAdvanceWidth = willCollapseCompletely();
    if (collapsesToZeroAdvanceWidth)
        lineRun->setCollapsesToZeroAdvanceWidth();
    else
        m_lineBox.setIsConsideredNonEmpty();

    if (collapsedRun)
        lineRun->setIsCollapsed();
    if (isTrimmable)
        m_trimmableContent.append(*lineRun);

    m_lineBox.expandHorizontally(lineRun->logicalRect().width());
    m_inlineItemRuns.append(WTFMove(lineRun));
}

void LineBuilder::appendNonReplacedInlineBox(const InlineItem& inlineItem, LayoutUnit logicalWidth)
{
    auto& layoutBox = inlineItem.layoutBox();
    auto& boxGeometry = formattingContext().geometryForBox(layoutBox);
    auto horizontalMargin = boxGeometry.horizontalMargin();
    m_inlineItemRuns.append(makeUnique<InlineItemRun>(inlineItem, Display::Rect { 0, contentLogicalWidth() + horizontalMargin.start, logicalWidth, { } }));
    m_lineBox.expandHorizontally(logicalWidth + horizontalMargin.start + horizontalMargin.end);
    m_lineBox.setIsConsideredNonEmpty();
    m_trimmableContent.clear();
    if (!layoutBox.establishesFormattingContext() || !boxGeometry.isEmpty())
        m_lineBox.setIsConsideredNonEmpty();
}

void LineBuilder::appendReplacedInlineBox(const InlineItem& inlineItem, LayoutUnit logicalWidth)
{
    ASSERT(inlineItem.layoutBox().isReplaced());
    // FIXME: Surely replaced boxes behave differently.
    appendNonReplacedInlineBox(inlineItem, logicalWidth);
    m_lineBox.setIsConsideredNonEmpty();
}

void LineBuilder::appendLineBreak(const InlineItem& inlineItem)
{
    m_lineBox.setIsConsideredNonEmpty();
    m_inlineItemRuns.append(makeUnique<InlineItemRun>(inlineItem, Display::Rect { 0, contentLogicalWidth(), { }, { } }));
}

void LineBuilder::adjustBaselineAndLineHeight(const Run& run)
{
    auto& baseline = m_lineBox.baseline();
    if (run.isText() || run.isForcedLineBreak()) {
        // For text content we set the baseline either through the initial strut (set by the formatting context root) or
        // through the inline container (start) -see above. Normally the text content itself does not stretch the line.
        if (!m_initialStrut)
            return;
        m_lineBox.setAscentIfGreater(m_initialStrut->ascent());
        m_lineBox.setDescentIfGreater(m_initialStrut->descent());
        m_lineBox.setLogicalHeightIfGreater(baseline.height());
        m_initialStrut = { };
        return;
    }

    auto& layoutBox = run.layoutBox();
    auto& style = layoutBox.style();
    if (run.isContainerStart()) {
        // Inline containers stretch the line by their font size.
        // Vertical margins, paddings and borders don't contribute to the line height.
        auto& fontMetrics = style.fontMetrics();
        if (style.verticalAlign() == VerticalAlign::Baseline) {
            auto halfLeading = halfLeadingMetrics(fontMetrics, style.computedLineHeight());
            // Both halfleading ascent and descent could be negative (tall font vs. small line-height value)
            if (halfLeading.descent() > 0)
                m_lineBox.setDescentIfGreater(halfLeading.descent());
            if (halfLeading.ascent() > 0)
                m_lineBox.setAscentIfGreater(halfLeading.ascent());
            m_lineBox.setLogicalHeightIfGreater(baseline.height());
        } else
            m_lineBox.setLogicalHeightIfGreater(fontMetrics.height());
        return;
    }

    if (run.isContainerEnd()) {
        // The line's baseline and height have already been adjusted at ContainerStart.
        return;
    }

    if (run.isBox()) {
        auto& boxGeometry = formattingContext().geometryForBox(layoutBox);
        auto marginBoxHeight = boxGeometry.marginBoxHeight();

        switch (style.verticalAlign()) {
        case VerticalAlign::Baseline: {
            if (layoutBox.isInlineBlockBox() && layoutBox.establishesInlineFormattingContext()) {
                // Inline-blocks with inline content always have baselines.
                auto& formattingState = downcast<InlineFormattingState>(layoutState().establishedFormattingState(downcast<Container>(layoutBox)));
                // Spec makes us generate at least one line -even if it is empty.
                ASSERT(!formattingState.lineBoxes().isEmpty());
                auto& lastLineBox = *formattingState.lineBoxes().last();
                auto inlineBlockBaseline = lastLineBox.baseline();
                auto beforeHeight = boxGeometry.marginBefore() + boxGeometry.borderTop() + boxGeometry.paddingTop().valueOr(0);

                m_lineBox.setAscentIfGreater(inlineBlockBaseline.ascent());
                m_lineBox.setDescentIfGreater(inlineBlockBaseline.descent());
                m_lineBox.setBaselineOffsetIfGreater(beforeHeight + lastLineBox.baselineOffset());
                m_lineBox.setLogicalHeightIfGreater(marginBoxHeight);
            } else {
                // Non inline-block boxes sit on the baseline (including their bottom margin).
                m_lineBox.setAscentIfGreater(marginBoxHeight);
                // Ignore negative descent (yes, negative descent is a thing).
                m_lineBox.setLogicalHeightIfGreater(marginBoxHeight + std::max(LayoutUnit(), baseline.descent()));
            }
            break;
        }
        case VerticalAlign::Top:
            // Top align content never changes the baseline, it only pushes the bottom of the line further down.
            m_lineBox.setLogicalHeightIfGreater(marginBoxHeight);
            break;
        case VerticalAlign::Bottom: {
            // Bottom aligned, tall content pushes the baseline further down from the line top.
            auto lineLogicalHeight = m_lineBox.logicalHeight();
            if (marginBoxHeight > lineLogicalHeight) {
                m_lineBox.setLogicalHeightIfGreater(marginBoxHeight);
                m_lineBox.setBaselineOffsetIfGreater(m_lineBox.baselineOffset() + (marginBoxHeight - lineLogicalHeight));
            }
            break;
        }
        default:
            ASSERT_NOT_IMPLEMENTED_YET();
            break;
        }
        return;
    }
    ASSERT_NOT_REACHED();
}

LayoutUnit LineBuilder::runContentHeight(const Run& run) const
{
    ASSERT(!m_skipAlignment);
    auto& fontMetrics = run.layoutBox().style().fontMetrics();
    if (run.isText() || run.isForcedLineBreak())
        return fontMetrics.height();

    if (run.isContainerStart() || run.isContainerEnd())
        return fontMetrics.height();

    auto& layoutBox = run.layoutBox();
    auto& boxGeometry = formattingContext().geometryForBox(layoutBox);
    if (layoutBox.replaced() || layoutBox.isFloatingPositioned())
        return boxGeometry.contentBoxHeight();

    // Non-replaced inline box (e.g. inline-block). It looks a bit misleading but their margin box is considered the content height here.
    return boxGeometry.marginBoxHeight();
}

void LineBuilder::TrimmableContent::append(InlineItemRun& inlineItemRun)
{
    ASSERT(inlineItemRun.logicalRect().width() >= 0);
    m_width += inlineItemRun.logicalRect().width();
    m_inlineItemRuns.append(&inlineItemRun);
}

LineBox::Baseline LineBuilder::halfLeadingMetrics(const FontMetrics& fontMetrics, LayoutUnit lineLogicalHeight)
{
    auto ascent = fontMetrics.ascent();
    auto descent = fontMetrics.descent();
    // 10.8.1 Leading and half-leading
    auto leading = lineLogicalHeight - (ascent + descent);
    // Inline tree is all integer based.
    auto adjustedAscent = std::max((ascent + leading / 2).floor(), 0);
    auto adjustedDescent = std::max((descent + leading / 2).ceil(), 0);
    return { adjustedAscent, adjustedDescent };
}

LayoutState& LineBuilder::layoutState() const
{ 
    return formattingContext().layoutState();
}

const InlineFormattingContext& LineBuilder::formattingContext() const
{
    return m_inlineFormattingContext;
} 

}
}

#endif
