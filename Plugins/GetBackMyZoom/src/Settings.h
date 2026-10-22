// Plugin settings: which changes the player wants and the light radius value.
// File: <plugin folder>/config/settings.json (same file and keys as the original
// plugin, so existing users keep their choices).
#pragma once

#include "patch/PatchDefs.h"

#include <array>
#include <filesystem>
#include <string>

namespace GetBackMyZoomConfig {

inline constexpr float kLightMin = 100.0f;
inline constexpr float kLightMax = 50000.0f;
inline constexpr float kLightDefault = 2000.0f;

struct Settings {
    std::array<bool, Gbmz::kPatchCount> want{};
    float lightRadius = kLightDefault;

    // Reads the file; a missing or broken file leaves the defaults. The original
    // plugin's master "enabled": false (all changes off) is honoured once by
    // clearing every wish, since the host's Enabled box now does that job.
    void Load(const std::filesystem::path& pluginDir);
    // Atomic write (tmp + replace). Returns false on failure.
    bool Save(const std::filesystem::path& pluginDir) const;

    static std::filesystem::path FilePath(const std::filesystem::path& pluginDir) {
        return pluginDir / "config" / "settings.json";
    }
    std::string ToJson() const;
    void FromJson(const std::string& text);
};

} // namespace GetBackMyZoomConfig
