// The patcher: one worker thread that owns the game-process handle, scans the
// game code once per game process, and keeps the game in the state the player
// asked for. The render thread only posts requests and reads a status copy, so
// nothing slow (process open, a ~50 MB code scan, writes) runs on a frame.
//
// Safety rules it enforces:
//   * a change is written only through a pattern that matched exactly once;
//   * the bytes are compared with the scanned original right before writing and
//     read back after; anything unexpected stops that change (no fighting);
//   * the original bytes are saved to disk before the first write, so a crash
//     is undone by the next session (RestoreJournal);
//   * everything is undone when the plugin is disabled or unloaded, when the
//     game is detached, or before the game process changes; a game that exited
//     needs nothing undone.
#pragma once

#include "GameProcess.h"
#include "PatchDefs.h"
#include "RestoreJournal.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Gbmz {

enum class PatchState : uint8_t {
    Unknown,        // not scanned yet
    NotFound,       // pattern absent (game updated?)
    Ambiguous,      // pattern found in several places: never written
    AlreadyChanged, // the code is already changed (by an older copy or another tool)
    Invalid,        // found, but the target failed a safety check
    Off,            // found, original bytes in place
    On,             // our bytes are in place
    Failed,         // a read or write failed or the bytes were unexpected
    Reverted,       // the game put the original back on its own
};

enum class SessionState : uint8_t {
    Stopped,      // worker not running (plugin disabled)
    NotAttached,  // POEFixer has no game attached
    Opening,
    Scanning,
    Ready,
    OpenFailed,   // OpenProcess / image read refused (retried every few seconds)
    ScanFailed,
    Crashed,      // the worker stopped after an internal error
};

struct PatchStatus {
    PatchState  state = PatchState::Unknown;
    int         matches = 0;
    uint32_t    rva = 0;            // target relative to the image base
    std::string originalHex;
    bool        hasGameValue = false;
    float       gameValue = 0.f;    // Value patches: the game's own constant
    float       appliedValue = 0.f;
    LocText     detail;             // last error / explanation
};

struct PatcherStatus {
    SessionState session = SessionState::Stopped;
    DWORD        pid = 0;
    uint32_t     imageStamp = 0;
    LocText      message;
    std::array<PatchStatus, kPatchCount> patches{};
    std::deque<std::string> recent;     // last activity lines (the log's, English), newest last
};

struct PatcherRequest {
    DWORD     pid = 0;
    uintptr_t imageBase = 0;
    std::array<bool, kPatchCount> want{};
    float     lightValue = 2000.f;

    bool operator==(const PatcherRequest&) const = default;
};

class Patcher {
public:
    Patcher() = default;
    Patcher(const Patcher&) = delete;
    Patcher& operator=(const Patcher&) = delete;
    ~Patcher() { Stop(); }

    void Start(const std::filesystem::path& journalFile);
    // Undoes every change still in place, then joins the worker.
    void Stop();
    bool Running() const { return m_thread.joinable(); }

    void Request(const PatcherRequest& request);   // cheap; wakes the worker on change
    void Rescan();                                  // undo everything, scan again, re-apply

    uint64_t      Revision() const { return m_revision.load(std::memory_order_acquire); }
    PatcherStatus Status() const;
    std::vector<std::string> DrainLog();             // lines for the host log (render thread)

private:
    struct Runtime {
        PatchState state = PatchState::Unknown;
        int        matches = 0;
        uintptr_t  target = 0;
        std::vector<uint8_t> original;
        std::vector<uint8_t> written;
        bool       active = false;
        bool       latched = false;    // a failure stops retries until the wish changes
        bool       lastWant = false;
        float      gameValue = 0.f;
        float      applied = 0.f;
        LocText    detail;
    };

    static void ThreadEntry(Patcher* self);
    static bool SehRun(void (*fn)(Patcher*), Patcher* self);
    static void LoopEntry(Patcher* self);
    static void FinishEntry(Patcher* self);

    void Loop();
    void Tick(const PatcherRequest& req, bool rescan);
    bool OpenSession(const PatcherRequest& req);
    void CloseSession(bool processGone);
    void RecoverFromJournal();
    void Scan();
    void ScanOne(size_t index, const std::vector<ScanRegion>& regions);
    void Reconcile(const PatcherRequest& req);
    void Verify();
    void RestoreAll();
    bool Apply(size_t index, const std::vector<uint8_t>& bytes);
    bool Undo(size_t index, bool force);
    void Fail(size_t index, LocText why);
    bool SaveJournal();
    void JournalPut(size_t index, const std::vector<uint8_t>& patched);
    void JournalDrop(size_t index);

    void SetSession(SessionState state, LocText message = {});
    void Publish();
    void Log(std::string line);

    // Worker-only state.
    GameProcess m_proc;
    std::array<Runtime, kPatchCount> m_rt{};
    Journal     m_journal;
    bool        m_scanned = false;
    std::chrono::steady_clock::time_point m_nextOpen{};
    LocText      m_lastOpenError;       // last logged open refusal (logged once per pid + text)
    DWORD        m_lastOpenErrorPid = 0;
    std::chrono::steady_clock::time_point m_nextVerify{};
    SessionState m_session = SessionState::Stopped;
    LocText      m_sessionMessage;

    // Shared with the render thread.
    std::filesystem::path   m_journalFile;
    std::thread             m_thread;
    mutable std::mutex      m_mutex;          // m_request, m_dirty, m_rescan, m_stop
    std::condition_variable m_cv;
    PatcherRequest          m_request;
    bool                    m_dirty = false;
    bool                    m_rescan = false;
    std::atomic<bool>       m_stop{false};
    std::atomic<bool>       m_failed{false};

    mutable std::mutex      m_statusMutex;    // m_status, m_logOut
    PatcherStatus           m_status;
    std::vector<std::string> m_logOut;
    std::atomic<uint64_t>   m_revision{0};
};

// The state's label: a key of lang/en.json (the drawing thread looks it up, the
// log and the report take its English()).
const char* PatchStateLabel(PatchState state);
const char* SessionStateLabel(SessionState state);

} // namespace Gbmz
