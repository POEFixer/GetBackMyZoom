#include "PatchDefs.h"

#include <string>

namespace Gbmz {

const std::array<PatchDef, kPatchCount>& Definitions() {
    // Signatures and patch bytes as shipped by the original plugin (1.1.0);
    // RevealMap's field displacements are wildcards since 1.1.5.
    static const std::array<PatchDef, kPatchCount> defs = {{
        {
            "InfiniteZoom", Key("feature_infinite_zoom"), Key("feature_infinite_zoom_tip"),
            // maxss xmm1,xmm0 | minss xmm1,[rip+limit] | movss [rsi+..],xmm1
            "F3 0F 5F C8 F3 0F 5D 0D ?? ?? ?? ?? F3 0F 11 8E",
            4, {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90}, PatchKind::Code,
        },
        {
            "NoAtlasFog", Key("feature_no_atlas_fog"), Key("feature_no_atlas_fog_tip"),
            // mulss xmm2,[rcx+disp8] | addss xmm0,xmm1
            "F3 0F 59 51 ?? F3 0F 58 C1",
            0, {0x90, 0x90, 0x90, 0x90, 0x90}, PatchKind::Code,
        },
        {
            "RevealMap", Key("feature_reveal_map"), Key("feature_reveal_map_tip"),
            // call [rax+28] | mov word [r15+d],0000 | mov byte [r15+d+2],..
            // The field displacements are wildcards: the game moved both
            // fields by 8 (d 0x58 -> 0x60), which the original exact
            // pattern never found again.
            "FF 50 28 66 41 C7 47 ?? 00 00 41 C6 47 ??",
            8, {0x90, 0x90}, PatchKind::Code,
        },
        {
            "PlayerLightRadius", Key("feature_light_radius"), Key("feature_light_radius_tip"),
            // addss xmm8,xmm6 | mulss xmm15,[rip+constant]
            "F3 44 0F 58 C6 F3 44 0F 59 3D",
            5, {}, PatchKind::Value, 9, 5,
            100.f, 50000.f, 2000.f,
        },
    }};
    return defs;
}

const char* ValidateDefinition(const PatchDef& def) {
    const auto pattern = Pattern::Parse(def.pattern);
    if (!pattern) return "pattern does not parse";
    if (def.targetOffset < 0) return "negative target offset";
    const size_t target = static_cast<size_t>(def.targetOffset);
    if (def.kind == PatchKind::Code) {
        if (def.patch.empty()) return "code patch without bytes";
        if (target + def.patch.size() > pattern->size()) return "code patch runs past the pattern";
    } else {
        if (!def.patch.empty()) return "value patch with code bytes";
        if (def.ripInstrLen < 5 || def.ripDispOffset < 1 || def.ripDispOffset + 4 > def.ripInstrLen)
            return "bad RIP-relative instruction shape";
        if (target >= pattern->size()) return "instruction starts past the pattern";
        if (!(def.valueMin < def.valueMax) || def.valueDefault < def.valueMin || def.valueDefault > def.valueMax)
            return "bad value range";
    }
    return "";
}

std::optional<Pattern> PatchedPattern(const PatchDef& def) {
    if (def.kind != PatchKind::Code) return std::nullopt;
    auto pattern = Pattern::Parse(def.pattern);
    if (!pattern) return std::nullopt;
    const size_t target = static_cast<size_t>(def.targetOffset);
    if (target + def.patch.size() > pattern->size()) return std::nullopt;
    for (size_t i = 0; i < def.patch.size(); ++i) {
        pattern->bytes[target + i] = def.patch[i];
        pattern->fixed[target + i] = 1;
    }
    // Recompute the anchor over the new fixed mask.
    return Pattern::Parse([&] {
        std::string text;
        for (size_t i = 0; i < pattern->size(); ++i) {
            if (!text.empty()) text += ' ';
            text += pattern->fixed[i] ? ToHex(&pattern->bytes[i], 1) : std::string("??");
        }
        return text;
    }());
}

} // namespace Gbmz
