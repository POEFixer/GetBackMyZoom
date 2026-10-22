// The game changes this plugin can make, described as data: where to find the
// code (a byte pattern), what to write, and how to check the bytes before and
// after. Pure code: covered by tests/.
#pragma once

#include "Pattern.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Gbmz {

enum class PatchKind {
    Code,   // overwrite instruction bytes inside an executable section
    Value,  // overwrite a 4-byte float constant a RIP-relative instruction reads
};

struct PatchDef {
    const char* id;          // stable settings key (upstream names, never change)
    const char* label;       // checkbox text
    const char* tip;         // multi-line tooltip
    const char* pattern;     // byte pattern, see Pattern::Parse
    int         targetOffset = 0;    // Code: first patched byte; Value: the instruction
    std::vector<uint8_t> patch;      // Code only: bytes written at the target
    PatchKind   kind = PatchKind::Code;
    int         ripInstrLen = 0;     // Value: length of the RIP-relative instruction
    int         ripDispOffset = 0;   // Value: offset of its disp32 inside the instruction
    float       valueMin = 0.f, valueMax = 0.f, valueDefault = 0.f;
};

inline constexpr size_t kPatchCount = 4;
enum PatchIndex : size_t { kInfiniteZoom = 0, kNoAtlasFog = 1, kRevealMap = 2, kLightRadius = 3 };

// The definitions, in display order. Parsed patterns are validated by
// ValidateDefinition (tests run it over every entry).
const std::array<PatchDef, kPatchCount>& Definitions();

// Static consistency of one definition: the pattern parses, a Code patch stays
// inside the pattern and has as many bytes as it overwrites, a Value patch's
// instruction fits. Returns an empty string when fine, else the reason.
const char* ValidateDefinition(const PatchDef& def);

// The pattern with the patch bytes put in place: how the code looks once this
// plugin (or a copy of it) has already changed it. Code patches only.
std::optional<Pattern> PatchedPattern(const PatchDef& def);

// RIP-relative resolution: the absolute address the instruction at `instrAddr`
// reads, given its disp32. No memory access.
inline uintptr_t ResolveRipTarget(uintptr_t instrAddr, int instrLen, int32_t disp) {
    return instrAddr + static_cast<uintptr_t>(instrLen) + static_cast<uintptr_t>(static_cast<intptr_t>(disp));
}

} // namespace Gbmz
