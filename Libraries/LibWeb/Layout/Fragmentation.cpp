/*
 * Copyright (c) 2026-present, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/Fragmentation.h>

namespace Web::Layout {

FragmentationContext::FragmentationContext(GC::Ref<Box const> root_box)
    : m_root_box(root_box)
{
}

FragmentationContext::~FragmentationContext() = default;

ColumnFragmentationContext::ColumnFragmentationContext(GC::Ref<Box const> root_box, int column_count, CSSPixels column_width, CSSPixels column_gap, CSSPixels preferred_column_height, Optional<CSSPixels> max_column_height)
    : FragmentationContext(root_box)
    , m_column_count(column_count)
    , m_column_width(column_width)
    , m_column_gap(column_gap)
    , m_preferred_column_height(preferred_column_height)
    , m_max_column_height(max_column_height)
{
}

ColumnFragmentationContext::~ColumnFragmentationContext() = default;

CSSPixels ColumnFragmentationContext::fragmentainer_x_offset_at(CSSPixels progress) const
{
    auto current_fragmentainer = floor(progress / m_preferred_column_height);
    return (m_column_width + m_column_gap) * current_fragmentainer;
}

CSSPixels ColumnFragmentationContext::fragmentainer_y_offset_at(CSSPixels progress) const
{
    auto current_fragmentainer = floor(progress / m_preferred_column_height);
    return -m_preferred_column_height * current_fragmentainer;
}

// https://drafts.csswg.org/css-break/#remaining-fragmentainer-extent
CSSPixels ColumnFragmentationContext::remaining_fragmentainer_extent_at(CSSPixels progress) const
{
    return m_preferred_column_height - (progress % m_preferred_column_height);
}

FragmentationDecision ColumnFragmentationContext::make_fragmentation_decision_at(CSSPixels progress, CSSPixels item_height)
{
    // This function decides whether a piece of monolithic content is placed as-is, shunted down into the next
    // fragmentainer, or placed as-is and then sliced into fragments.
    auto remaining_fragmentainer_extent = remaining_fragmentainer_extent_at(progress);

    // If there is enough preferable space in this column, place the item as-is.
    if (remaining_fragmentainer_extent >= item_height)
        return FragmentationDecision::Place;

    // If there is not enough preferable space and this item would exceed our height limit, shunt the content into the
    // next fragmentainer if it can fit there.
    if (m_max_column_height.has_value() && item_height > m_max_column_height.value() - m_preferred_column_height + remaining_fragmentainer_extent) {
        if (item_height <= m_max_column_height.value()) {
            // If the item is larger than our preferred height, but still fits within the height limit, placing it at
            // the start of the next column will still incur a content deficit as it exceeds the preferred height of
            // that column.
            if (item_height > m_preferred_column_height)
                m_content_deficit += item_height - m_preferred_column_height;
            return FragmentationDecision::Shunt;
        }

        // If the content cannot fit in a full column, it must be fragmented across multiple.
        return FragmentationDecision::Fragment;
    }

    // If we are not height-limited and are in the last column, we cannot shunt or fragment. We must place.
    if (floor(progress / m_preferred_column_height) >= m_column_count) {
        return FragmentationDecision::Place;
    }

    // If we are not yet in the last column, calculate the deficit we would incur by placing the item as-is.
    auto deficit_if_placed = item_height - remaining_fragmentainer_extent;

    // If the potential deficit outweighs the potential surplus, shunt the item and take the surplus.
    if (abs(m_content_deficit + deficit_if_placed) > abs(m_content_deficit - remaining_fragmentainer_extent)) {
        m_content_deficit -= remaining_fragmentainer_extent;
        return FragmentationDecision::Shunt;
    }

    // Otherwise take the deficit.
    m_content_deficit += deficit_if_placed;
    return FragmentationDecision::Place;
}

}
