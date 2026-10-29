#include "Patcher.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>

namespace Gbmz {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kIdleWait = std::chrono::milliseconds(200);
constexpr auto kOpenRetry = std::chrono::seconds(5);
constexpr auto kVerifyEvery = std::chrono::seconds(2);
constexpr size_t kRecentLines = 40;

bool Writable(PatchState s) {
    return s == PatchState::Off || s == PatchState::On || s == PatchState::Failed || s == PatchState::Reverted;
}

} // namespace

const char* PatchStateLabel(PatchState state) {
    switch (state) {
        case PatchState::Unknown:        return Key("patch_state_waiting");
        case PatchState::NotFound:       return Key("patch_state_not_found");
        case PatchState::Ambiguous:      return Key("patch_state_not_unique");
        case PatchState::AlreadyChanged: return Key("patch_state_already_changed");
        case PatchState::Invalid:        return Key("patch_state_unsafe");
        case PatchState::Off:            return Key("patch_state_off");
        case PatchState::On:             return Key("patch_state_active");
        case PatchState::Failed:         return Key("patch_state_failed");
        case PatchState::Reverted:       return Key("patch_state_undone_by_game");
    }
    return "?";
}

const char* SessionStateLabel(SessionState state) {
    switch (state) {
        case SessionState::Stopped:     return Key("session_stopped");
        case SessionState::NotAttached: return Key("session_no_game");
        case SessionState::Opening:     return Key("session_connecting");
        case SessionState::Scanning:    return Key("session_scanning");
        case SessionState::Ready:       return Key("session_ready");
        case SessionState::OpenFailed:  return Key("session_no_access");
        case SessionState::ScanFailed:  return Key("session_scan_failed");
        case SessionState::Crashed:     return Key("session_stopped_error");
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

void Patcher::Start(const std::filesystem::path& journalFile) {
    if (m_thread.joinable()) return;
    m_journalFile = journalFile;
    m_stop.store(false);
    m_failed.store(false);
    m_rt = {};
    m_scanned = false;
    m_nextOpen = {};
    m_lastOpenError.Clear();
    m_lastOpenErrorPid = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_dirty = true;
        m_rescan = false;
    }
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_status = {};
    }
    SetSession(SessionState::NotAttached, Key("message_waiting"));
    m_thread = std::thread(&Patcher::ThreadEntry, this);
}

void Patcher::Stop() {
    if (!m_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop.store(true);
    }
    m_cv.notify_all();
    m_thread.join();
    // The worker undoes its changes on the way out; if it died first, do it here
    // (the thread is gone, so the session is ours alone).
    if (m_proc.IsOpen()) {
        SehRun(&Patcher::FinishEntry, this);
        m_proc.Close();
    }
    SetSession(SessionState::Stopped, Key("message_disabled"));
}

void Patcher::Request(const PatcherRequest& request) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_request == request) return;
        m_request = request;
        m_dirty = true;
    }
    m_cv.notify_one();
}

void Patcher::Rescan() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_rescan = true;
        m_dirty = true;
    }
    m_cv.notify_one();
}

PatcherStatus Patcher::Status() const {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_status;
}

std::vector<std::string> Patcher::DrainLog() {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    std::vector<std::string> out;
    out.swap(m_logOut);
    return out;
}

// ---------------------------------------------------------------------------
// Thread entry: SEH outside, C++ exceptions inside, restore on every way out
// ---------------------------------------------------------------------------

bool Patcher::SehRun(void (*fn)(Patcher*), Patcher* self) {
    __try {
        fn(self);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Patcher::LoopEntry(Patcher* self) {
    try {
        self->Loop();
    } catch (const std::exception& e) {
        self->Log(std::string("Internal error: ") + e.what());
        self->m_failed.store(true);
    } catch (...) {
        self->Log("Internal error.");
        self->m_failed.store(true);
    }
}

void Patcher::FinishEntry(Patcher* self) {
    try {
        self->RestoreAll();
    } catch (...) {
    }
}

void Patcher::ThreadEntry(Patcher* self) {
    const bool clean = SehRun(&Patcher::LoopEntry, self) && !self->m_failed.load();
    SehRun(&Patcher::FinishEntry, self);
    self->m_proc.Close();
    if (!clean) {
        self->SetSession(SessionState::Crashed, Key("message_crashed"));
        self->Log("Patcher stopped after an internal error");
    }
}

void Patcher::Loop() {
    while (!m_stop.load()) {
        PatcherRequest req;
        bool rescan = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, kIdleWait, [&] { return m_stop.load() || m_dirty; });
            if (m_stop.load()) break;
            m_dirty = false;
            req = m_request;
            rescan = m_rescan;
            m_rescan = false;
        }
        Tick(req, rescan);
    }
}

// ---------------------------------------------------------------------------
// One reconciliation step
// ---------------------------------------------------------------------------

void Patcher::Tick(const PatcherRequest& req, bool rescan) {
    const auto now = Clock::now();

    if (m_proc.IsOpen()) {
        if (!m_proc.IsAlive()) {
            Log("The game process exited; nothing to undo.");
            CloseSession(true);
        } else if (req.pid != m_proc.Pid() || req.imageBase != m_proc.Layout().base) {
            Log(req.pid ? "The game process changed; undoing changes in the old one."
                        : "The game was detached; undoing every change.");
            RestoreAll();
            CloseSession(false);
        }
    }

    if (!m_proc.IsOpen()) {
        if (req.pid == 0 || req.imageBase == 0) {
            if (m_session != SessionState::NotAttached) SetSession(SessionState::NotAttached, Key("message_waiting"));
            return;
        }
        if (now < m_nextOpen && !rescan) return;
        if (!OpenSession(req)) {
            m_nextOpen = now + kOpenRetry;
            return;
        }
        rescan = false;   // OpenSession scanned already
    } else if (rescan) {
        Log("Scanning again on request.");
        RestoreAll();
        Scan();
    }

    if (m_stop.load() || !m_scanned) return;
    Reconcile(req);
    if (now >= m_nextVerify) {
        Verify();
        m_nextVerify = now + kVerifyEvery;
    }
}

bool Patcher::OpenSession(const PatcherRequest& req) {
    SetSession(SessionState::Opening, Key("message_connecting"));
    LocText error;
    if (!m_proc.Open(req.pid, req.imageBase, error)) {
        SetSession(SessionState::OpenFailed, error);
        // Retried every few seconds: log a refusal once per game process and
        // text, not on every attempt (the status line keeps showing it).
        if (req.pid != m_lastOpenErrorPid || error != m_lastOpenError) {
            Log(English(error));
            m_lastOpenError = error;
            m_lastOpenErrorPid = req.pid;
        }
        return false;
    }
    m_lastOpenError.Clear();
    m_lastOpenErrorPid = 0;
    const ImageLayout& L = m_proc.Layout();
    m_journal = {};
    m_journal.pid = m_proc.Pid();
    m_journal.imageBase = L.base;
    m_journal.timeDateStamp = L.timeDateStamp;
    m_journal.sizeOfImage = L.sizeOfImage;
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_status.pid = m_proc.Pid();
        m_status.imageStamp = L.timeDateStamp;
    }
    Log("Connected to the game (process " + std::to_string(m_proc.Pid()) + ").");
    RecoverFromJournal();
    Scan();
    return true;
}

void Patcher::CloseSession(bool processGone) {
    if (processGone) Journal::Remove(m_journalFile);
    m_proc.Close();
    m_scanned = false;
    m_rt = {};
    m_nextOpen = {};
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_status.pid = 0;
        m_status.imageStamp = 0;
    }
    SetSession(SessionState::NotAttached, Key("message_waiting"));
}

// A record left by a session that could not undo its changes.
void Patcher::RecoverFromJournal() {
    const auto old = Journal::Load(m_journalFile);
    if (!old) return;
    const ImageLayout& L = m_proc.Layout();
    if (!old->SameProcess(m_proc.Pid(), L.base, L.timeDateStamp, L.sizeOfImage)) {
        // Another (now gone or no longer patched) game process: nothing to do here.
        Journal::Remove(m_journalFile);
        return;
    }
    for (const auto& e : old->entries) {
        const uintptr_t addr = L.base + e.rva;
        std::vector<uint8_t> current(e.original.size());
        if (!m_proc.Read(addr, current.data(), current.size())) continue;
        if (current == e.original) continue;
        if (!e.patched.empty() && current != e.patched) {
            Log("Left " + e.id + " alone: the bytes are not the ones this plugin wrote.");
            continue;
        }
        const ImageSection* sec = L.SectionOf(addr, current.size());
        LocText error;
        if (sec && m_proc.Write(addr, e.original.data(), e.original.size(), sec->Executable(), error))
            Log("Undid " + e.id + " left over from an earlier session.");
        else
            Log("Could not undo " + e.id + " from an earlier session: " + English(error));
    }
    Journal::Remove(m_journalFile);
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

void Patcher::Scan() {
    m_scanned = false;
    for (auto& rt : m_rt) {
        const bool want = rt.lastWant;
        rt = {};
        rt.lastWant = want;
    }
    SetSession(SessionState::Scanning, Key("message_reading"));
    std::vector<ScanRegion> regions;
    if (!m_proc.ReadExecutableSections(regions, m_stop)) {
        if (m_stop.load()) return;
        SetSession(SessionState::ScanFailed, Key("message_read_failed"));
        Log("Scan failed: no readable code.");
        return;
    }
    for (size_t i = 0; i < kPatchCount && !m_stop.load(); ++i) ScanOne(i, regions);
    m_scanned = true;
    m_nextVerify = Clock::now() + kVerifyEvery;

    int found = 0;
    for (const auto& rt : m_rt) found += rt.state == PatchState::Off ? 1 : 0;
    SetSession(SessionState::Ready, found == static_cast<int>(kPatchCount) ? Key("message_all_found")
                                                                           : Key("message_some_missing"));
    Log("Scan done: " + std::to_string(found) + " of " + std::to_string(kPatchCount) + " features found.");
}

void Patcher::ScanOne(size_t i, const std::vector<ScanRegion>& regions) {
    const PatchDef& def = Definitions()[i];
    Runtime& rt = m_rt[i];
    const auto pattern = Pattern::Parse(def.pattern);
    const char* invalid = ValidateDefinition(def);
    if (!pattern || invalid[0] != '\0') {
        rt.state = PatchState::Invalid;
        // The reason is the definition check's own words (a developer's, never shown unless a
        // definition is broken; the tests check every one).
        rt.detail = LocText(Key("detail_definition_invalid"), {{"reason", invalid}});
        return;
    }
    const MatchResult m = FindMatches(*pattern, regions, 2);
    rt.matches = m.count;
    if (m.count == 0) {
        const auto patched = PatchedPattern(def);
        if (patched && FindMatches(*patched, regions, 2).count == 1) {
            rt.state = PatchState::AlreadyChanged;
            rt.detail = Key("detail_already_changed");
        } else {
            rt.state = PatchState::NotFound;
            rt.detail = Key("detail_not_found");
        }
        Log(std::string(def.id) + ": " + English(PatchStateLabel(rt.state)));
        return;
    }
    if (m.count > 1) {
        rt.state = PatchState::Ambiguous;
        rt.detail = Key("detail_not_unique");
        Log(std::string(def.id) + ": pattern is not unique; left alone.");
        return;
    }

    const ScanRegion& region = regions[m.region];
    const ImageLayout& L = m_proc.Layout();
    const size_t at = m.offset + static_cast<size_t>(def.targetOffset);

    if (def.kind == PatchKind::Code) {
        const size_t n = def.patch.size();
        const uintptr_t target = m.address + static_cast<uintptr_t>(def.targetOffset);
        const ImageSection* sec = L.SectionOf(target, n);
        if (!region.RangeReadable(at, n) || !sec || !sec->Executable()) {
            rt.state = PatchState::Invalid;
            rt.detail = Key("detail_safety_check");
            return;
        }
        rt.target = target;
        rt.original.assign(region.data.begin() + static_cast<ptrdiff_t>(at),
                           region.data.begin() + static_cast<ptrdiff_t>(at + n));
        rt.state = PatchState::Off;
        return;
    }

    // Value: follow the RIP-relative operand to the constant.
    const size_t dispAt = at + static_cast<size_t>(def.ripDispOffset);
    if (!region.RangeReadable(dispAt, 4)) {
        rt.state = PatchState::Invalid;
        rt.detail = Key("detail_instruction_unreadable");
        return;
    }
    int32_t disp = 0;
    std::memcpy(&disp, region.data.data() + dispAt, 4);
    const uintptr_t instr = m.address + static_cast<uintptr_t>(def.targetOffset);
    const uintptr_t constant = ResolveRipTarget(instr, def.ripInstrLen, disp);
    const ImageSection* sec = L.SectionOf(constant, 4);
    float value = 0.f;
    std::vector<uint8_t> bytes(4);
    if ((constant & 3) != 0 || !sec || sec->Executable() || !sec->Readable() ||
        !m_proc.Read(constant, bytes.data(), 4)) {
        rt.state = PatchState::Invalid;
        rt.detail = Key("detail_constant_misplaced");
        return;
    }
    std::memcpy(&value, bytes.data(), 4);
    if (!std::isfinite(value) || std::fabs(value) > 1e9f) {
        rt.state = PatchState::Invalid;
        rt.detail = Key("detail_constant_not_sensible");
        return;
    }
    rt.target = constant;
    rt.original = std::move(bytes);
    rt.gameValue = value;
    rt.state = PatchState::Off;
}

// ---------------------------------------------------------------------------
// Apply / undo
// ---------------------------------------------------------------------------

void Patcher::Reconcile(const PatcherRequest& req) {
    bool changed = false;
    for (size_t i = 0; i < kPatchCount; ++i) {
        Runtime& rt = m_rt[i];
        const PatchDef& def = Definitions()[i];
        const bool want = req.want[i];
        if (want != rt.lastWant) {
            rt.lastWant = want;
            rt.latched = false;
            if (rt.state == PatchState::Failed || rt.state == PatchState::Reverted) {
                rt.state = rt.active ? PatchState::On : PatchState::Off;
                rt.detail.Clear();
                changed = true;
            }
        }
        if (!Writable(rt.state) || rt.latched) continue;

        if (def.kind == PatchKind::Code) {
            if (want && !rt.active) {
                Apply(i, def.patch);
                changed = true;
            } else if (!want && rt.active) {
                Undo(i, false);
                changed = true;
            }
        } else {
            const float v = std::clamp(req.lightValue, def.valueMin, def.valueMax);
            if (want && (!rt.active || v != rt.applied)) {
                std::vector<uint8_t> bytes(4);
                std::memcpy(bytes.data(), &v, 4);
                if (Apply(i, bytes)) rt.applied = v;
                changed = true;
            } else if (!want && rt.active) {
                Undo(i, false);
                changed = true;
            }
        }
    }
    if (changed) Publish();
}

bool Patcher::Apply(size_t i, const std::vector<uint8_t>& bytes) {
    Runtime& rt = m_rt[i];
    const PatchDef& def = Definitions()[i];
    std::vector<uint8_t> current(rt.original.size());
    if (bytes.size() != rt.original.size() || !m_proc.Read(rt.target, current.data(), current.size())) {
        Fail(i, Key("detail_memory_unreadable"));
        return false;
    }
    const std::vector<uint8_t>& expected = rt.active ? rt.written : rt.original;
    if (current != expected) {
        Fail(i, Key("detail_changed_since_scan"));
        return false;
    }
    if (!rt.active) {
        JournalPut(i, def.kind == PatchKind::Code ? bytes : std::vector<uint8_t>{});
        if (!SaveJournal()) {
            JournalDrop(i);
            Fail(i, Key("detail_journal_unsaved"));
            return false;
        }
    }
    LocText error;
    if (!m_proc.Write(rt.target, bytes.data(), bytes.size(), def.kind == PatchKind::Code, error)) {
        if (!rt.active) {
            JournalDrop(i);
            SaveJournal();
        }
        Fail(i, error);
        return false;
    }
    const bool first = !rt.active;
    rt.active = true;
    rt.written = bytes;
    rt.state = PatchState::On;
    rt.detail.Clear();
    if (first) Log(std::string(def.id) + " switched on.");
    return true;
}

bool Patcher::Undo(size_t i, bool force) {
    Runtime& rt = m_rt[i];
    const PatchDef& def = Definitions()[i];
    if (!rt.active) return true;
    if (rt.latched && !force) return false;
    std::vector<uint8_t> current(rt.original.size());
    if (!m_proc.Read(rt.target, current.data(), current.size())) {
        Fail(i, Key("detail_memory_unreadable"));
        return false;
    }
    if (current != rt.original) {
        if (current != rt.written) {
            Fail(i, Key("detail_changed_by_other"));
            return false;
        }
        LocText error;
        if (!m_proc.Write(rt.target, rt.original.data(), rt.original.size(), def.kind == PatchKind::Code, error)) {
            Fail(i, error);
            return false;
        }
    }
    rt.active = false;
    rt.written.clear();
    rt.applied = 0.f;
    if (rt.state == PatchState::On) rt.state = PatchState::Off;
    JournalDrop(i);
    SaveJournal();
    Log(std::string(def.id) + " switched off.");
    return true;
}

void Patcher::Verify() {
    bool changed = false;
    for (size_t i = 0; i < kPatchCount; ++i) {
        Runtime& rt = m_rt[i];
        if (!rt.active || rt.latched) continue;
        std::vector<uint8_t> current(rt.written.size());
        if (!m_proc.Read(rt.target, current.data(), current.size()) || current == rt.written) continue;
        if (current == rt.original) {
            rt.active = false;
            rt.written.clear();
            rt.state = PatchState::Reverted;
            rt.latched = true;   // never fight the game over it
            rt.detail = Key("detail_undone_by_game");
            JournalDrop(i);
            SaveJournal();
            Log(std::string(Definitions()[i].id) + " was undone by the game.");
        } else {
            Fail(i, Key("detail_changed_by_other"));
        }
        changed = true;
    }
    if (changed) Publish();
}

void Patcher::RestoreAll() {
    if (!m_proc.IsOpen() || !m_proc.IsAlive()) return;
    for (size_t i = 0; i < kPatchCount; ++i)
        if (m_rt[i].active) Undo(i, true);
    if (m_journal.entries.empty()) Journal::Remove(m_journalFile);
    Publish();
}

void Patcher::Fail(size_t i, LocText why) {
    Runtime& rt = m_rt[i];
    rt.state = PatchState::Failed;
    rt.latched = true;
    rt.detail = std::move(why);
    Log(std::string(Definitions()[i].id) + ": " + English(rt.detail));
}

bool Patcher::SaveJournal() { return m_journal.Save(m_journalFile); }

void Patcher::JournalPut(size_t i, const std::vector<uint8_t>& patched) {
    JournalDrop(i);
    JournalEntry e;
    e.id = Definitions()[i].id;
    e.rva = static_cast<uint32_t>(m_rt[i].target - m_proc.Layout().base);
    e.original = m_rt[i].original;
    e.patched = patched;
    m_journal.entries.push_back(std::move(e));
}

void Patcher::JournalDrop(size_t i) {
    const std::string id = Definitions()[i].id;
    auto& v = m_journal.entries;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const JournalEntry& e) { return e.id == id; }), v.end());
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

void Patcher::SetSession(SessionState state, LocText message) {
    m_session = state;
    m_sessionMessage = std::move(message);
    Publish();
}

void Patcher::Publish() {
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_status.session = m_session;
        m_status.message = m_sessionMessage;
        const uintptr_t base = m_proc.IsOpen() ? m_proc.Layout().base : 0;
        for (size_t i = 0; i < kPatchCount; ++i) {
            const Runtime& rt = m_rt[i];
            PatchStatus& s = m_status.patches[i];
            s.state = rt.state;
            s.matches = rt.matches;
            s.rva = (rt.target && base && rt.target >= base) ? static_cast<uint32_t>(rt.target - base) : 0;
            s.originalHex = rt.original.empty() ? std::string() : ToHex(rt.original.data(), rt.original.size());
            s.hasGameValue = Definitions()[i].kind == PatchKind::Value && rt.original.size() == 4;
            s.gameValue = rt.gameValue;
            s.appliedValue = rt.applied;
            s.detail = rt.detail;
        }
    }
    m_revision.fetch_add(1, std::memory_order_release);
}

void Patcher::Log(std::string line) {
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_status.recent.push_back(line);
        while (m_status.recent.size() > kRecentLines) m_status.recent.pop_front();
        m_logOut.push_back("[GetBackMyZoom] " + line);
        if (m_logOut.size() > 200) m_logOut.erase(m_logOut.begin());
    }
    m_revision.fetch_add(1, std::memory_order_release);
}

} // namespace Gbmz
