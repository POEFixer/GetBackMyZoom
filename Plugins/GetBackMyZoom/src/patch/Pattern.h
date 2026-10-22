// Byte patterns with wildcards and a match counter that reports how many places
// a pattern occurs, so a caller can refuse to touch anything that is not unique.
// Pure code (no Win32, no game access): covered by tests/.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Gbmz {

// "F3 0F 5D 0D ?? ?? ?? ??" (also "?" for one wildcard byte).
struct Pattern {
    std::vector<uint8_t> bytes;   // value per position (0 where wildcard)
    std::vector<uint8_t> fixed;   // 1 = must match, 0 = wildcard
    size_t anchorPos = 0;         // longest run of fixed bytes: searched first
    size_t anchorLen = 0;

    size_t size() const { return bytes.size(); }
    bool MatchesAt(const uint8_t* p) const;

    // nullopt on a malformed text, an empty pattern or one without fixed bytes.
    static std::optional<Pattern> Parse(std::string_view text);
};

// One contiguous block of the target image copied into this process. Pages that
// could not be read are zero-filled and flagged, and a match that touches one is
// ignored (zeros could otherwise fake a match).
struct ScanRegion {
    uintptr_t                   base = 0;      // address in the target process
    std::vector<uint8_t>        data;
    std::vector<uint8_t>        pageReadable;  // one flag per kPageSize bytes
    static constexpr size_t     kPageSize = 0x1000;

    bool RangeReadable(size_t offset, size_t length) const;
};

struct MatchResult {
    int       count = 0;        // capped at the maxCount passed to FindMatches
    uintptr_t address = 0;      // first match (target-process address)
    size_t    region = 0;       // index of the region holding the first match
    size_t    offset = 0;       // offset of the first match inside that region
};

// Counts matches of `pattern` across all regions, stopping at maxCount.
// A result with count == 1 is the only one a caller may write through.
MatchResult FindMatches(const Pattern& pattern, const std::vector<ScanRegion>& regions,
                        int maxCount = 2);

// Formats bytes as "F3 0F 5D 0D".
std::string ToHex(const uint8_t* data, size_t size);

} // namespace Gbmz
