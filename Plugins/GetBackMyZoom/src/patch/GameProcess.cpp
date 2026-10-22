#include "GameProcess.h"

#include <algorithm>
#include <cstring>

namespace Gbmz {

namespace {

// Everything patching needs and nothing more: read/write the image, change page
// protection for the write, tell whether the process is still running.
constexpr DWORD kAccess = PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                          PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;

constexpr size_t   kChunk = 1u << 20;                // 1 MiB reads, page-wise retry on failure
constexpr uint32_t kMaxSectionBytes = 512u << 20;    // refuse absurd headers
constexpr size_t   kMaxTotalBytes = size_t{1} << 30;
constexpr WORD     kMaxSections = 96;

std::string Win32Message(const char* what, DWORD code) {
    std::string out = what;
    out += " (Windows error ";
    out += std::to_string(code);
    out += ")";
    return out;
}

} // namespace

const ImageSection* ImageLayout::SectionOf(uintptr_t addr, size_t size) const {
    if (addr < base || size == 0) return nullptr;
    const uintptr_t rva = addr - base;
    for (const auto& s : sections) {
        if (rva >= s.rva && rva - s.rva <= s.size && size <= s.size - (rva - s.rva)) return &s;
    }
    return nullptr;
}

bool GameProcess::Open(DWORD pid, uintptr_t imageBase, std::string& error) {
    Close();
    if (pid == 0 || imageBase == 0) {
        error = "The game is not attached yet.";
        return false;
    }
    m_handle = ::OpenProcess(kAccess, FALSE, pid);
    if (!m_handle) {
        const DWORD code = ::GetLastError();
        error = code == ERROR_ACCESS_DENIED
            ? Win32Message("Windows refused access to the game. Start POEFixer as administrator", code)
            : Win32Message("Could not open the game process", code);
        return false;
    }
    m_pid = pid;
    m_layout = {};
    m_layout.base = imageBase;
    if (!ReadLayout(error)) {
        Close();
        return false;
    }
    return true;
}

void GameProcess::Close() {
    if (m_handle) ::CloseHandle(m_handle);
    m_handle = nullptr;
    m_pid = 0;
    m_layout = {};
}

bool GameProcess::IsAlive() const {
    return m_handle && ::WaitForSingleObject(m_handle, 0) == WAIT_TIMEOUT;
}

bool GameProcess::Read(uintptr_t addr, void* buffer, size_t size) const {
    if (!m_handle || !addr || !buffer || size == 0) return false;
    SIZE_T got = 0;
    return ::ReadProcessMemory(m_handle, reinterpret_cast<LPCVOID>(addr), buffer, size, &got) && got == size;
}

bool GameProcess::ReadLayout(std::string& error) {
    IMAGE_DOS_HEADER dos{};
    if (!Read(m_layout.base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0 || dos.e_lfanew > 0x1000) {
        error = "The game image could not be read (bad DOS header).";
        return false;
    }
    IMAGE_NT_HEADERS64 nt{};
    const uintptr_t ntAddr = m_layout.base + static_cast<uintptr_t>(dos.e_lfanew);
    if (!Read(ntAddr, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        error = "The game image could not be read (not a 64-bit PE image).";
        return false;
    }
    const WORD count = nt.FileHeader.NumberOfSections;
    if (count == 0 || count > kMaxSections) {
        error = "The game image has an unexpected section table.";
        return false;
    }
    m_layout.sizeOfImage = nt.OptionalHeader.SizeOfImage;
    m_layout.timeDateStamp = nt.FileHeader.TimeDateStamp;

    std::vector<IMAGE_SECTION_HEADER> headers(count);
    const uintptr_t secAddr = ntAddr + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                              nt.FileHeader.SizeOfOptionalHeader;
    if (!Read(secAddr, headers.data(), headers.size() * sizeof(IMAGE_SECTION_HEADER))) {
        error = "The game image could not be read (section table).";
        return false;
    }
    for (const auto& h : headers) {
        ImageSection s;
        std::memcpy(s.name, h.Name, 8);
        s.rva = h.VirtualAddress;
        s.size = h.Misc.VirtualSize ? h.Misc.VirtualSize : h.SizeOfRawData;
        s.characteristics = h.Characteristics;
        if (s.size == 0 || s.size > kMaxSectionBytes) continue;
        if (s.rva >= m_layout.sizeOfImage || s.size > m_layout.sizeOfImage - s.rva) continue;
        m_layout.sections.push_back(s);
    }
    if (m_layout.sections.empty()) {
        error = "The game image has no usable sections.";
        return false;
    }
    return true;
}

bool GameProcess::ReadExecutableSections(std::vector<ScanRegion>& out, const std::atomic<bool>& stop) const {
    out.clear();
    size_t total = 0;
    for (const auto& s : m_layout.sections) {
        if (!s.Executable() || !s.Readable()) continue;
        if (total + s.size > kMaxTotalBytes) break;
        total += s.size;

        ScanRegion region;
        region.base = m_layout.base + s.rva;
        region.data.assign(s.size, 0);
        region.pageReadable.assign((s.size + ScanRegion::kPageSize - 1) / ScanRegion::kPageSize, 0);

        for (size_t off = 0; off < s.size; off += kChunk) {
            if (stop.load(std::memory_order_relaxed)) return false;
            const size_t len = std::min(kChunk, static_cast<size_t>(s.size) - off);
            if (Read(region.base + off, region.data.data() + off, len)) {
                std::fill(region.pageReadable.begin() + static_cast<ptrdiff_t>(off / ScanRegion::kPageSize),
                          region.pageReadable.begin() +
                              static_cast<ptrdiff_t>((off + len + ScanRegion::kPageSize - 1) / ScanRegion::kPageSize),
                          uint8_t{1});
                continue;
            }
            // A chunk with an unreadable page: retry page by page, keep zeros where it fails.
            for (size_t p = off; p < off + len; p += ScanRegion::kPageSize) {
                const size_t plen = std::min(ScanRegion::kPageSize, off + len - p);
                if (Read(region.base + p, region.data.data() + p, plen))
                    region.pageReadable[p / ScanRegion::kPageSize] = 1;
                else
                    std::memset(region.data.data() + p, 0, plen);
            }
        }
        out.push_back(std::move(region));
    }
    return !out.empty();
}

bool GameProcess::Write(uintptr_t addr, const void* data, size_t size, bool code, std::string& error) const {
    if (!m_handle || !addr || !data || size == 0) {
        error = "Nothing to write.";
        return false;
    }
    // Only inside the game image, and only in a section of the expected kind.
    const ImageSection* section = m_layout.SectionOf(addr, size);
    if (!section || section->Executable() != code) {
        error = "Refused: the address is not where this change belongs.";
        return false;
    }
    DWORD oldProtect = 0;
    if (!::VirtualProtectEx(m_handle, reinterpret_cast<LPVOID>(addr), size,
                            code ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &oldProtect)) {
        error = Win32Message("Windows refused to unlock the game memory", ::GetLastError());
        return false;
    }
    SIZE_T written = 0;
    const BOOL ok = ::WriteProcessMemory(m_handle, reinterpret_cast<LPVOID>(addr), data, size, &written);
    const DWORD writeError = ok ? 0 : ::GetLastError();
    DWORD ignored = 0;
    ::VirtualProtectEx(m_handle, reinterpret_cast<LPVOID>(addr), size, oldProtect, &ignored);
    if (code) ::FlushInstructionCache(m_handle, reinterpret_cast<LPCVOID>(addr), size);
    if (!ok || written != size) {
        error = Win32Message("Writing to the game failed", writeError);
        return false;
    }
    std::vector<uint8_t> check(size);
    if (!Read(addr, check.data(), size) || std::memcmp(check.data(), data, size) != 0) {
        error = "The game memory did not keep the new bytes.";
        return false;
    }
    return true;
}

} // namespace Gbmz
