#include "RestoreJournal.h"

#include "../../../../POEFixer/core/FileIO.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>
#include <system_error>

namespace Gbmz {

std::vector<uint8_t> HexToBytes(const std::string& hex) {
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> out;
    if (hex.size() % 2 != 0) return out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = value(hex[i]), lo = value(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<uint8_t>(hi * 16 + lo));
    }
    return out;
}

std::string BytesToHex(const std::vector<uint8_t>& bytes) {
    static const char* kDigits = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out += kDigits[b >> 4];
        out += kDigits[b & 15];
    }
    return out;
}

std::string Journal::ToJson() const {
    nlohmann::json j;
    j["version"] = 1;
    j["pid"] = pid;
    j["image_base"] = imageBase;
    j["time_date_stamp"] = timeDateStamp;
    j["size_of_image"] = sizeOfImage;
    nlohmann::json list = nlohmann::json::array();
    for (const auto& e : entries) {
        list.push_back({{"id", e.id}, {"rva", e.rva},
                        {"original", BytesToHex(e.original)}, {"patched", BytesToHex(e.patched)}});
    }
    j["entries"] = std::move(list);
    return j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::optional<Journal> Journal::FromJson(const std::string& text) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("version", 0) != 1) return std::nullopt;
    Journal out;
    const auto num = [&](const char* key, auto& dst) {
        const auto it = j.find(key);
        if (it == j.end() || !it->is_number_unsigned()) return false;
        dst = it->get<std::remove_reference_t<decltype(dst)>>();
        return true;
    };
    if (!num("pid", out.pid) || !num("image_base", out.imageBase) ||
        !num("time_date_stamp", out.timeDateStamp) || !num("size_of_image", out.sizeOfImage))
        return std::nullopt;
    const auto list = j.find("entries");
    if (list == j.end() || !list->is_array()) return std::nullopt;
    for (const auto& item : *list) {
        if (!item.is_object()) return std::nullopt;
        JournalEntry e;
        const auto id = item.find("id");
        const auto rva = item.find("rva");
        const auto orig = item.find("original");
        const auto patched = item.find("patched");
        if (id == item.end() || !id->is_string() || rva == item.end() || !rva->is_number_unsigned() ||
            orig == item.end() || !orig->is_string() || patched == item.end() || !patched->is_string())
            return std::nullopt;
        e.id = id->get<std::string>();
        e.rva = rva->get<uint32_t>();
        e.original = HexToBytes(orig->get<std::string>());
        e.patched = HexToBytes(patched->get<std::string>());
        if (e.original.empty() || e.original.size() > 64) return std::nullopt;
        if (!e.patched.empty() && e.patched.size() != e.original.size()) return std::nullopt;
        out.entries.push_back(std::move(e));
    }
    return out;
}

bool Journal::Save(const std::filesystem::path& file) const {
    if (entries.empty()) {
        Remove(file);
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    return FileIO::AtomicWrite(file, ToJson());
}

std::optional<Journal> Journal::Load(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return FromJson(text);
}

void Journal::Remove(const std::filesystem::path& file) {
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

} // namespace Gbmz
