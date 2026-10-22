#include "Settings.h"

#include "../../../POEFixer/core/FileIO.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>

namespace GetBackMyZoomConfig {

void Settings::FromJson(const std::string& text) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;

    if (const auto it = j.find("light_radius"); it != j.end() && it->is_number()) {
        const float v = it->get<float>();
        if (std::isfinite(v)) lightRadius = std::clamp(v, kLightMin, kLightMax);
    }
    if (const auto it = j.find("active"); it != j.end() && it->is_object()) {
        const auto& defs = Gbmz::Definitions();
        for (size_t i = 0; i < defs.size(); ++i) {
            const auto e = it->find(defs[i].id);
            if (e != it->end() && e->is_boolean()) want[i] = e->get<bool>();
        }
    }
    // Original plugin: "enabled": false meant every change reverted.
    if (const auto it = j.find("enabled"); it != j.end() && it->is_boolean() && !it->get<bool>())
        want.fill(false);
}

std::string Settings::ToJson() const {
    nlohmann::json j;
    j["version"] = 2;
    j["light_radius"] = lightRadius;
    nlohmann::json a = nlohmann::json::object();
    const auto& defs = Gbmz::Definitions();
    for (size_t i = 0; i < defs.size(); ++i) a[defs[i].id] = want[i];
    j["active"] = std::move(a);
    return j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

void Settings::Load(const std::filesystem::path& pluginDir) {
    try {
        const auto path = FilePath(pluginDir);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) return;
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return;
        FromJson(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
    } catch (...) {
    }
}

bool Settings::Save(const std::filesystem::path& pluginDir) const {
    try {
        if (pluginDir.empty()) return false;
        std::error_code ec;
        std::filesystem::create_directories(pluginDir / "config", ec);
        return FileIO::AtomicWrite(FilePath(pluginDir), ToJson());
    } catch (...) {
        return false;
    }
}

} // namespace GetBackMyZoomConfig
