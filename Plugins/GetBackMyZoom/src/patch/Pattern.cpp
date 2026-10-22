#include "Pattern.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>

namespace Gbmz {

namespace {

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

bool Pattern::MatchesAt(const uint8_t* p) const {
    for (size_t i = 0; i < bytes.size(); ++i)
        if (fixed[i] && p[i] != bytes[i]) return false;
    return true;
}

std::optional<Pattern> Pattern::Parse(std::string_view text) {
    Pattern out;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == ' ' || c == '\t') { ++i; continue; }
        if (c == '?') {
            out.bytes.push_back(0);
            out.fixed.push_back(0);
            ++i;
            if (i < text.size() && text[i] == '?') ++i;
            continue;
        }
        if (i + 1 >= text.size()) return std::nullopt;
        const int hi = HexValue(c), lo = HexValue(text[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.bytes.push_back(static_cast<uint8_t>(hi * 16 + lo));
        out.fixed.push_back(1);
        i += 2;
        if (i < text.size() && text[i] != ' ' && text[i] != '\t') return std::nullopt;
    }
    if (out.bytes.empty()) return std::nullopt;

    // Longest run of fixed bytes becomes the search anchor.
    size_t runStart = 0, runLen = 0;
    for (size_t k = 0; k <= out.fixed.size(); ++k) {
        if (k < out.fixed.size() && out.fixed[k]) {
            if (runLen == 0) runStart = k;
            ++runLen;
            if (runLen > out.anchorLen) { out.anchorLen = runLen; out.anchorPos = runStart; }
        } else {
            runLen = 0;
        }
    }
    if (out.anchorLen == 0) return std::nullopt;
    return out;
}

bool ScanRegion::RangeReadable(size_t offset, size_t length) const {
    if (length == 0) return true;
    if (offset > data.size() || length > data.size() - offset) return false;
    const size_t first = offset / kPageSize;
    const size_t last = (offset + length - 1) / kPageSize;
    for (size_t p = first; p <= last; ++p)
        if (p >= pageReadable.size() || !pageReadable[p]) return false;
    return true;
}

MatchResult FindMatches(const Pattern& pattern, const std::vector<ScanRegion>& regions, int maxCount) {
    MatchResult result;
    if (pattern.size() == 0 || pattern.anchorLen == 0 || maxCount <= 0) return result;

    const uint8_t* anchor = pattern.bytes.data() + pattern.anchorPos;
    const std::boyer_moore_horspool_searcher searcher(anchor, anchor + pattern.anchorLen);

    for (size_t r = 0; r < regions.size(); ++r) {
        const ScanRegion& region = regions[r];
        if (region.data.size() < pattern.size()) continue;
        const uint8_t* begin = region.data.data();
        const uint8_t* end = begin + region.data.size();
        const uint8_t* it = begin;
        while (true) {
            const auto [found, foundEnd] = searcher(it, end);
            if (found == end) break;
            it = found + 1;
            const size_t anchorOffset = static_cast<size_t>(found - begin);
            if (anchorOffset < pattern.anchorPos) continue;
            const size_t start = anchorOffset - pattern.anchorPos;
            if (start + pattern.size() > region.data.size()) continue;
            if (!pattern.MatchesAt(begin + start)) continue;
            if (!region.RangeReadable(start, pattern.size())) continue;
            if (result.count == 0) {
                result.address = region.base + start;
                result.region = r;
                result.offset = start;
            }
            if (++result.count >= maxCount) return result;
        }
    }
    return result;
}

std::string ToHex(const uint8_t* data, size_t size) {
    std::string out;
    out.reserve(size * 3);
    char buf[4];
    for (size_t i = 0; i < size; ++i) {
        std::snprintf(buf, sizeof(buf), i + 1 < size ? "%02X " : "%02X", data[i]);
        out += buf;
    }
    return out;
}

} // namespace Gbmz
