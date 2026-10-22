// The original bytes of every change currently applied, written to disk BEFORE
// the change is made. If POEFixer stops without undoing its changes (crash,
// killed process), the next session finds the record for the same game process
// and puts the original bytes back. Records for another process are dropped
// once that process is gone.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Gbmz {

struct JournalEntry {
    std::string          id;        // PatchDef::id
    uint32_t             rva = 0;   // relative to the image base
    std::vector<uint8_t> original;
    std::vector<uint8_t> patched;   // empty: a value change (any current value is ours)
};

struct Journal {
    uint32_t  pid = 0;
    uint64_t  imageBase = 0;
    uint32_t  timeDateStamp = 0;
    uint32_t  sizeOfImage = 0;
    std::vector<JournalEntry> entries;

    bool SameProcess(uint32_t p, uint64_t base, uint32_t stamp, uint32_t size) const {
        return pid == p && imageBase == base && timeDateStamp == stamp && sizeOfImage == size;
    }

    std::string ToJson() const;
    static std::optional<Journal> FromJson(const std::string& text);

    // Atomic save (tmp + replace); an empty journal deletes the file.
    bool Save(const std::filesystem::path& file) const;
    static std::optional<Journal> Load(const std::filesystem::path& file);
    static void Remove(const std::filesystem::path& file);
};

std::vector<uint8_t> HexToBytes(const std::string& hex);
std::string BytesToHex(const std::vector<uint8_t>& bytes);

} // namespace Gbmz
