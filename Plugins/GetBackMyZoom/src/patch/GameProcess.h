// A handle on the game process with only the rights patching needs, the layout
// of its main image (parsed from the PE headers in the game's memory), and
// checked reads and writes. Used only from the patcher's worker thread.
#pragma once

#include "LocText.h"
#include "Pattern.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <Windows.h>

namespace Gbmz {

struct ImageSection {
    char     name[9] = {};
    uint32_t rva = 0;
    uint32_t size = 0;            // virtual size
    uint32_t characteristics = 0;
    bool Executable() const { return (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0; }
    bool Readable() const { return (characteristics & IMAGE_SCN_MEM_READ) != 0; }
};

struct ImageLayout {
    uintptr_t base = 0;
    uint32_t  sizeOfImage = 0;
    uint32_t  timeDateStamp = 0;
    std::vector<ImageSection> sections;

    // The section holding [addr, addr + size), or nullptr.
    const ImageSection* SectionOf(uintptr_t addr, size_t size) const;
};

class GameProcess {
public:
    GameProcess() = default;
    GameProcess(const GameProcess&) = delete;
    GameProcess& operator=(const GameProcess&) = delete;
    ~GameProcess() { Close(); }

    // Opens `pid` and reads the PE layout of the module at `imageBase`.
    // On failure returns false and fills `error` with a sentence for the UI.
    bool Open(DWORD pid, uintptr_t imageBase, LocText& error);
    void Close();

    bool  IsOpen() const { return m_handle != nullptr; }
    bool  IsAlive() const;
    DWORD Pid() const { return m_pid; }
    const ImageLayout& Layout() const { return m_layout; }

    bool Read(uintptr_t addr, void* buffer, size_t size) const;

    // Writes `size` bytes, lifting the page protection only for the write
    // (code pages: execute-read-write, data pages: read-write), flushes the
    // instruction cache for code and reads the bytes back to confirm.
    bool Write(uintptr_t addr, const void* data, size_t size, bool code, LocText& error) const;

    // Copies every executable section of the image. Unreadable pages are
    // zero-filled and flagged. Returns false if stopped or nothing was read.
    bool ReadExecutableSections(std::vector<ScanRegion>& out, const std::atomic<bool>& stop) const;

private:
    bool ReadLayout(LocText& error);

    HANDLE      m_handle = nullptr;
    DWORD       m_pid = 0;
    ImageLayout m_layout;
};

} // namespace Gbmz
