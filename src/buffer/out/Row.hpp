/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- Row.hpp

Abstract:
- data structure for information associated with one row of screen buffer

Author(s):
- Michael Niksa (miniksa) 10-Apr-2014
- Paul Campbell (paulcam) 10-Apr-2014

Revision History:
- From components of output.h/.c
  by Therese Stowell (ThereseS) 1990-1991
- Pulled into its own file from textBuffer.hpp/cpp (AustDi, 2017)
--*/

#pragma once

#include <til/rle.h>

#include "LineRendition.hpp"
#include "OutputCell.hpp"
#include "OutputCellIterator.hpp"

class TextBuffer;

enum class DelimiterClass
{
    ControlChar,
    DelimiterChar,
    RegularChar
};

struct RowWriteState
{
    // The text you want to write into the given ROW. When ReplaceText() returns,
    // this is updated to remove all text from the beginning that was successfully written.
    std::wstring_view text; // IN/OUT
    // The column at which to start writing.
    til::CoordType columnBegin = 0; // IN
    // The first column which should not be written to anymore.
    til::CoordType columnLimit = 0; // IN

    // The column 1 past the last glyph that was successfully written into the row. If you need to call
    // ReplaceAttributes() to colorize the written range, etc., this is the columnEnd parameter you want.
    // If you want to continue writing where you left off, this is also the next columnBegin parameter.
    til::CoordType columnEnd = 0; // OUT
    // The first column that got modified by this write operation. In case that the first glyph we write overwrites
    // the trailing half of a wide glyph, leadingSpaces will be 1 and this value will be 1 less than colBeg.
    til::CoordType columnBeginDirty = 0; // OUT
    // This is 1 past the last column that was modified and will be 1 past columnEnd if we overwrote
    // the leading half of a wide glyph and had to fill the trailing half with whitespace.
    til::CoordType columnEndDirty = 0; // OUT
};

class ROW final
{
public:
    ROW() = default;
    ROW(wchar_t* charsBuffer, uint16_t* charOffsetsBuffer, uint16_t rowWidth, const TextAttribute& fillAttribute);

    ROW(const ROW& other) = delete;
    ROW& operator=(const ROW& other) = delete;

    explicit ROW(ROW&& other) = default;
    ROW& operator=(ROW&& other) = default;

    friend void swap(ROW& lhs, ROW& rhs) noexcept;

    void SetWrapForced(const bool wrap) noexcept;
    bool WasWrapForced() const noexcept;
    void SetDoubleBytePadded(const bool doubleBytePadded) noexcept;
    bool WasDoubleBytePadded() const noexcept;
    void SetLineRendition(const LineRendition lineRendition) noexcept;
    LineRendition GetLineRendition() const noexcept;

    void Reset(const TextAttribute& attr);
    void Resize(wchar_t* charsBuffer, uint16_t* charOffsetsBuffer, uint16_t rowWidth, const TextAttribute& fillAttribute);
    void TransferAttributes(const til::small_rle<TextAttribute, uint16_t, 1>& attr, til::CoordType newWidth);

    til::CoordType NavigateToPrevious(til::CoordType column) const noexcept;
    til::CoordType NavigateToNext(til::CoordType column) const noexcept;

    void ClearCell(til::CoordType column);
    OutputCellIterator WriteCells(OutputCellIterator it, til::CoordType columnBegin, std::optional<bool> wrap = std::nullopt, std::optional<til::CoordType> limitRight = std::nullopt);
    bool SetAttrToEnd(til::CoordType columnBegin, TextAttribute attr);
    void ReplaceAttributes(til::CoordType beginIndex, til::CoordType endIndex, const TextAttribute& newAttr);
    void ReplaceCharacters(til::CoordType columnBegin, til::CoordType width, const std::wstring_view& chars);
    void ReplaceText(RowWriteState& state);
    til::CoordType CopyRangeFrom(til::CoordType columnBegin, til::CoordType columnLimit, const ROW& other, til::CoordType& otherBegin, til::CoordType otherLimit);

    til::small_rle<TextAttribute, uint16_t, 1>& Attributes() noexcept;
    const til::small_rle<TextAttribute, uint16_t, 1>& Attributes() const noexcept;
    TextAttribute GetAttrByColumn(til::CoordType column) const;
    std::vector<uint16_t> GetHyperlinks() const;
    uint16_t size() const noexcept;
    til::CoordType LineRenditionColumns() const noexcept;
    til::CoordType MeasureLeft() const noexcept;
    til::CoordType MeasureRight() const noexcept;
    bool ContainsText() const noexcept;
    std::wstring_view GlyphAt(til::CoordType column) const noexcept;
    DbcsAttribute DbcsAttrAt(til::CoordType column) const noexcept;
    std::wstring_view GetText() const noexcept;
    DelimiterClass DelimiterClassAt(til::CoordType column, const std::wstring_view& wordDelimiters) const noexcept;

    auto AttrBegin() const noexcept { return _attr.begin(); }
    auto AttrEnd() const noexcept { return _attr.end(); }

#ifdef UNIT_TESTING
    friend constexpr bool operator==(const ROW& a, const ROW& b) noexcept;
    friend class RowTests;
#endif

private:
    // WriteHelper exists because other forms of abstracting this functionality away (like templates with lambdas)
    // where only very poorly optimized by MSVC as it failed to inline the templates.
    struct WriteHelper
    {
        explicit WriteHelper(ROW& row, til::CoordType columnBegin, til::CoordType columnLimit, const std::wstring_view& chars) noexcept;
        bool IsValid() const noexcept;
        void ReplaceCharacters(til::CoordType width) noexcept;
        void ReplaceText() noexcept;
        void CopyRangeFrom(const std::span<const uint16_t>& charOffsets) noexcept;
        void Finish();

        // Parent pointer.
        ROW& row;
        // The text given by the caller.
        const std::wstring_view& chars;

        // This is the same as the columnBegin parameter for ReplaceText(), etc.,
        // but clamped to a valid range via _clampedColumnInclusive.
        uint16_t colBeg;
        // This is the same as the columnLimit parameter for ReplaceText(), etc.,
        // but clamped to a valid range via _clampedColumnInclusive.
        uint16_t colLimit;

        // The column 1 past the last glyph that was successfully written into the row. If you need to call
        // ReplaceAttributes() to colorize the written range, etc., this is the columnEnd parameter you want.
        // If you want to continue writing where you left off, this is also the next columnBegin parameter.
        uint16_t colEnd;
        // The first column that got modified by this write operation. In case that the first glyph we write overwrites
        // the trailing half of a wide glyph, leadingSpaces will be 1 and this value will be 1 less than colBeg.
        uint16_t colBegDirty;
        // Similar to dirtyBeg, this is 1 past the last column that was modified and will be 1 past colEnd if
        // we overwrote the leading half of a wide glyph and had to fill the trailing half with whitespace.
        uint16_t colEndDirty;
        // The offset in ROW::chars at which we start writing the contents of WriteHelper::chars.
        uint16_t chBeg;
        // The offset at which we start writing leadingSpaces-many whitespaces.
        uint16_t chBegDirty;
        // The same as `colBeg - colBegDirty`. This is the amount of whitespace
        // we write at chBegDirty, before the actual WriteHelper::chars content.
        uint16_t leadingSpaces;
        // The amount of characters copied from WriteHelper::chars.
        size_t charsConsumed;
    };

    // To simplify the detection of wide glyphs, we don't just store the simple character offset as described
    // for _charOffsets. Instead we use the most significant bit to indicate whether any column is the
    // trailing half of a wide glyph. This simplifies many implementation details via _uncheckedIsTrailer.
    static constexpr uint16_t CharOffsetsTrailer = 0x8000;
    static constexpr uint16_t CharOffsetsMask = 0x7fff;

    template<typename T>
    static constexpr uint16_t _clampedUint16(T v) noexcept;
    template<typename T>
    constexpr uint16_t _clampedColumn(T v) const noexcept;
    template<typename T>
    constexpr uint16_t _clampedColumnInclusive(T v) const noexcept;

    uint16_t _adjustBackward(uint16_t column) const noexcept;
    uint16_t _adjustForward(uint16_t column) const noexcept;

    wchar_t _uncheckedChar(size_t off) const noexcept;
    uint16_t _charSize() const noexcept;
    uint16_t _uncheckedCharOffset(size_t col) const noexcept;
    bool _uncheckedIsTrailer(size_t col) const noexcept;

    void _init() noexcept;
    void _resizeChars(uint16_t colEndDirty, uint16_t chBegDirty, size_t chEndDirty, uint16_t chEndDirtyOld);

    // These fields are a bit "wasteful", but it makes all this a bit more robust against
    // programming errors during initial development (which is when this comment was written).
    // * _chars and _charsHeap are redundant
    //   If _charsHeap is stored in _chars, we can still infer that
    //   _chars was allocated on the heap if _chars != _charsBuffer.
    // * _chars doesn't need a size_t size()
    //   The size may never exceed an uint16_t anyways.
    // * _charOffsets doesn't need a size() at all
    //   The length is already stored in _columns.

    // Most text uses only a single wchar_t per codepoint / grapheme cluster.
    // That's why TextBuffer allocates a large blob of virtual memory which we can use as
    // a simplified chars buffer, without having to allocate any additional heap memory.
    // _charsBuffer fits _columnCount characters at most.
    wchar_t* _charsBuffer = nullptr;
    // ...but if this ROW needs to store more than _columnCount characters
    // then it will allocate a larger string on the heap and store it here.
    // The capacity of this string on the heap is stored in _chars.size().
    std::unique_ptr<wchar_t[]> _charsHeap;
    // _chars either refers to our _charsBuffer or _charsHeap, defaulting to the former.
    // _chars.size() is NOT the length of the string, but rather its capacity.
    // _charOffsets[_columnCount] stores the length.
    std::span<wchar_t> _chars;
    // _charOffsets accelerates indexing into the above _chars string given a terminal column,
    // by storing the character index/offset at which a column's text in _chars starts.
    // It stores 1 more item than this row is wide, allowing it to store the
    // past-the-end offset, which is thus equal to the length of the string.
    //
    // For instance given a 4 column ROW containing "abcd" it would store 01234,
    // because each of "abcd" are 1 column wide and 1 wchar_t per column.
    // Given "a\u732Bd" it would store 01123, because "\u732B" is a wide glyph
    // and "11" indicates that both column 1 and 2 start at &_chars[1] (= wide glyph).
    // The fact that the next offset is 2 tells us that the glyph is 1 wchar_t long.
    // Given "a\uD83D\uDE00d" ("\uD83D\uDE00" is an Emoji) it would store 01134,
    // because while it's 2 cells wide as indicated by 2 offsets that are identical (11),
    // the next offset is 3, which indicates that the glyph is 3-1 = 2 wchar_t long.
    //
    // In other words, _charOffsets tells us both the width in chars and width in columns.
    // See CharOffsetsTrailer for more information.
    std::span<uint16_t> _charOffsets;
    // _attr is a run-length-encoded vector of TextAttribute with a decompressed
    // length equal to _columnCount (= 1 TextAttribute per column).
    til::small_rle<TextAttribute, uint16_t, 1> _attr;
    // The width of the row in visual columns.
    uint16_t _columnCount = 0;
    // Stores double-width/height (DECSWL/DECDWL/DECDHL) attributes.
    LineRendition _lineRendition = LineRendition::SingleWidth;
    // Occurs when the user runs out of text in a given row and we're forced to wrap the cursor to the next line
    bool _wrapForced = false;
    // Occurs when the user runs out of text to support a double byte character and we're forced to the next line
    bool _doubleBytePadded = false;
};

#ifdef UNIT_TESTING
constexpr bool operator==(const ROW& a, const ROW& b) noexcept
{
    // comparison is only used in the tests; this should suffice.
    return a._charsBuffer == b._charsBuffer;
}
#endif
