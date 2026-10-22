// Get Back My Zoom: restores a much further camera zoom-out and a few other
// visual changes (no Atlas fog, full map reveal, larger light radius) by
// changing a few bytes of the running game, and undoes them again.
//
// Originally created by Omer Faruk ARPA; maintained by POEFixer since
// October 2026. Licensed under the GNU General Public License v3.0.
#include "../../../POEFixer/plugin_sdk/PluginSDK.h"
#include "../../../POEFixer/imgui/IconsFontAwesome6.h"
#include "../../../POEFixer/plugin_sdk/PluginUi.h"

#include "Settings.h"
#include "patch/Patcher.h"

#include <imgui.h>
#include <imgui_internal.h>   // ImTextInitClassifiers (see BindImGui)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

// Plugin info: the host checks it before CreatePlugin; the publish tool reads it.
POEFIXER_PLUGIN_INFO(
    "GetBackMyZoom",
    "Get Back My Zoom",
    "1.1.3",
    "Omer Faruk ARPA, POEFixer",
    "Restores a much further camera zoom-out, removes Atlas fog, reveals the whole map and enlarges the light radius, safely undone when switched off.")

namespace {

using Clock = std::chrono::steady_clock;
using GetBackMyZoomConfig::kLightMax;
using GetBackMyZoomConfig::kLightMin;
using Gbmz::PatchState;
using Gbmz::SessionState;

ImVec4 PatchColor(PatchState s) {
    const PluginUi::Palette p = PluginUi::Colors();
    switch (s) {
        case PatchState::On:             return p.Success;
        case PatchState::NotFound:
        case PatchState::Ambiguous:
        case PatchState::AlreadyChanged:
        case PatchState::Reverted:       return p.Warning;
        case PatchState::Invalid:
        case PatchState::Failed:         return p.Error;
        default:                         return p.TextDisabled;
    }
}

ImVec4 SessionColor(SessionState s) {
    const PluginUi::Palette p = PluginUi::Colors();
    switch (s) {
        case SessionState::Ready:      return p.Success;
        case SessionState::OpenFailed:
        case SessionState::ScanFailed: return p.Warning;
        case SessionState::Crashed:    return p.Error;
        default:                       return p.TextDisabled;
    }
}

// Width PluginUi::StatusPill takes for this text (the kit's own padding).
float StatusPillWidth(const char* text) {
    return ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 1.5f;
}

// Puts the next item of width `w` at the right end of the current line, or on
// a new line when the row is too narrow (never over the item before). The kit's
// AlignRight only aligns within the current line.
void SameLineRightOrWrap(float w) {
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x >= w) PluginUi::AlignRight(w);
    else ImGui::NewLine();
}

// A status pill in a semantic colour with its explanation on hover.
void Pill(const char* text, const ImVec4& color, const char* tip = nullptr) {
    PluginUi::StatusPill(text, ImGui::ColorConvertFloat4ToU32(color));
    PluginUi::Tip(tip);
}

const char* PatchTip(PatchState s) {
    switch (s) {
        case PatchState::Unknown:  return "Waiting for the game to be scanned.";
        case PatchState::Off:      return "Found in this game version and currently off.";
        case PatchState::On:       return "Active in the running game.";
        case PatchState::NotFound: return "The code this feature changes was not found.\nThe game was probably updated; the feature\nstays off until the plugin is updated.";
        default:                   return nullptr;
    }
}

} // namespace

class GetBackMyZoomPlugin : public PluginSDK::Plugin {
public:
    ~GetBackMyZoomPlugin() override { m_patcher.Stop(); }

    const char* GetName() const override { return "Get Back My Zoom"; }

    void OnEnable(bool /*isGameAttached*/) override {
        if (!HostCompatible()) return;
        BindImGui();
        m_settings = {};
        m_settings.Load(DirectoryPath());
        m_loaded = true;
        m_dirty = false;
        m_patcher.Start(DirectoryPath() / "config" / "restore_journal.json");
        PostRequest();
        ctx()->Log.Info("[GetBackMyZoom] enabled");
    }

    void OnDisable() override {
        if (!m_loaded) return;
        m_patcher.Stop();   // undoes every change before returning
        FlushLog();
        SaveSettings();
        m_statusRevision = ~uint64_t{0};
        ctx()->Log.Info("[GetBackMyZoom] disabled; every change was undone");
    }

    void SaveSettings() override {
        if (!m_loaded) return;
        if (m_settings.Save(DirectoryPath())) m_dirty = false;
    }

    // Runs every frame while enabled, menu shown or not: hands the current game
    // process to the patcher (three cheap host calls) and forwards its log.
    void DrawUI() override {
        if (!m_loaded) return;
        PostRequest();
        FlushLog();
        if (m_dirty && Clock::now() - m_dirtySince > std::chrono::seconds(1)) SaveSettings();
    }

    void DrawSettings() override {
        BindImGui();
        if (!HostCompatible()) {
            PluginUi::Hint("This POEFixer version is too old for Get Back My Zoom. Update POEFixer.");
            return;
        }
        RefreshStatus();
        ImGui::PushID("GetBackMyZoom");
        ImGui::Spacing();
        DrawSectionConnection();
        ImGui::Spacing();
        DrawSectionFeatures();
        ImGui::Spacing();
        DrawSectionDiagnostics();
        ImGui::Spacing();
        PluginUi::Hint("Originally created by Omer Faruk ARPA. Maintained by POEFixer. GPL-3.0.");
        ImGui::Spacing();
        ImGui::PopID();
    }

private:
    GetBackMyZoomConfig::Settings m_settings;
    Gbmz::Patcher       m_patcher;
    Gbmz::PatcherStatus m_status;
    uint64_t            m_statusRevision = ~uint64_t{0};
    bool                m_loaded = false;
    bool                m_dirty = false;
    Clock::time_point   m_dirtySince{};

    void BindImGui() const {
        if (ctx()->ImGuiContext) ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));
        // This DLL has its own copy of ImGui's word-wrap character table, which only
        // a font atlas build in this module would fill: unfilled, every wrapped text
        // (hints, tooltips) breaks inside words. Cheap after the first call.
        ImTextInitClassifiers();
    }

    void MarkDirty() {
        if (!m_dirty) m_dirtySince = Clock::now();
        m_dirty = true;
    }

    void PostRequest() {
        Gbmz::PatcherRequest req;
        if (ctx()->Game.IsAttached()) {
            req.pid = ctx()->Game.GetProcessId();
            req.imageBase = ctx()->Memory.GetBaseAddress();
        }
        req.want = m_settings.want;
        req.lightValue = m_settings.lightRadius;
        m_patcher.Request(req);
    }

    void FlushLog() {
        for (const auto& line : m_patcher.DrainLog()) ctx()->Log.Info(line.c_str());
    }

    void RefreshStatus() {
        const uint64_t rev = m_patcher.Revision();
        if (rev == m_statusRevision) return;
        m_status = m_patcher.Status();
        m_statusRevision = rev;
    }

    // --- Game connection ----------------------------------------------------
    void DrawSectionConnection() {
        PluginUi::BeginCard("###connection");
        PluginUi::Heading(ICON_FA_PLUG, "Game connection",
            "The plugin changes a few bytes of the running game's code. It finds\n"
            "them by searching for known byte patterns, writes only where a pattern\n"
            "matches exactly once, and keeps the original bytes so every change\n"
            "can be undone.\n\n"
            "If Windows refuses access, start POEFixer as administrator.");

        Pill(Gbmz::SessionStateLabel(m_status.session), SessionColor(m_status.session));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(m_status.message.empty() ? "Waiting for the game." : m_status.message.c_str());
        ImGui::PopTextWrapPos();

        if (m_status.pid) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Game process %lu", static_cast<unsigned long>(m_status.pid));
        } else {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("No game process");
        }
        const char* rescanLabel = ICON_FA_ARROWS_ROTATE "  Scan again###rescan";
        SameLineRightOrWrap(PluginUi::ButtonWidth(rescanLabel));
        const bool canRescan = m_status.session == SessionState::Ready || m_status.session == SessionState::ScanFailed;
        ImGui::BeginDisabled(!canRescan);
        if (PluginUi::SubtleButton(rescanLabel)) m_patcher.Rescan();
        ImGui::EndDisabled();
        PluginUi::Tip("Undoes every change, reads the game code again and re-applies\n"
                   "what is switched on. Use it after a game update or when a\n"
                   "feature reports a problem.");
        PluginUi::EndCard();
    }

    // --- Features ---------------------------------------------------------------
    void DrawFeatureRow(size_t i) {
        const Gbmz::PatchDef& def = Gbmz::Definitions()[i];
        const Gbmz::PatchStatus& st = m_status.patches[i];

        char id[24];
        std::snprintf(id, sizeof(id), "want%zu", i);
        bool want = m_settings.want[i];
        if (PluginUi::Check(def.label, id, &want, def.tip)) {
            m_settings.want[i] = want;
            MarkDirty();
            PostRequest();
        }

        const bool sessionUp = m_status.session == SessionState::Ready;
        const char* text = sessionUp ? Gbmz::PatchStateLabel(st.state) : "Waiting";
        const ImVec4 color = sessionUp ? PatchColor(st.state) : PluginUi::Colors().TextDisabled;
        const char* tip = !st.detail.empty() ? st.detail.c_str() : PatchTip(sessionUp ? st.state : PatchState::Unknown);
        SameLineRightOrWrap(StatusPillWidth(text));
        Pill(text, color, tip);
    }

    void DrawSectionFeatures() {
        PluginUi::BeginCard("###features");
        PluginUi::Heading(ICON_FA_MAGNIFYING_GLASS_MINUS, "Camera and map",
                          "Tick a feature to apply it to the running game at once; untick it\n"
                          "to put the original back. Your choices are re-applied whenever\n"
                          "the game is attached again.");
        DrawFeatureRow(Gbmz::kInfiniteZoom);
        DrawFeatureRow(Gbmz::kNoAtlasFog);
        DrawFeatureRow(Gbmz::kRevealMap);

        ImGui::Spacing();
        PluginUi::Heading(ICON_FA_LIGHTBULB, "Light");
        DrawFeatureRow(Gbmz::kLightRadius);
        {
            const Gbmz::PatchStatus& st = m_status.patches[Gbmz::kLightRadius];
            const float rowX = ImGui::GetCursorPosX();
            const PluginUi::LabelColumn col({"Radius"});
            ImGui::BeginDisabled(!m_settings.want[Gbmz::kLightRadius]);
            col.Row("Radius");
            float v = m_settings.lightRadius;
            if (ImGui::SliderFloat("###radius", &v, kLightMin, kLightMax, "%.0f",
                                   ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) {
                m_settings.lightRadius = v;
                MarkDirty();
                PostRequest();
            }
            char tip[200];
            if (st.hasGameValue)
                std::snprintf(tip, sizeof(tip), "The value written into the game.\nThe game's own value is %.0f.\nCtrl+click to type a number.", st.gameValue);
            else
                std::snprintf(tip, sizeof(tip), "The value written into the game.\nCtrl+click to type a number.");
            PluginUi::Tip(tip);

            // Beside the slider; on a narrow tab under it, in the control column.
            const char* resetLabel = ICON_FA_ROTATE_LEFT "  Game value###gameValue";
            ImGui::SameLine();
            if (ImGui::GetContentRegionAvail().x < PluginUi::ButtonWidth(resetLabel)) {
                ImGui::NewLine();
                ImGui::SetCursorPosX(rowX + col.Width());
            }
            ImGui::BeginDisabled(!st.hasGameValue);
            if (PluginUi::SubtleButton(resetLabel)) {
                m_settings.lightRadius = std::clamp(st.gameValue, kLightMin, kLightMax);
                MarkDirty();
                PostRequest();
            }
            ImGui::EndDisabled();
            PluginUi::Tip("Sets the radius back to the game's own value\n(known once the game has been scanned).");
            ImGui::EndDisabled();
        }

        ImGui::Spacing();
        PluginUi::Hint("Changes are undone when you switch them off, disable this plugin, "
                    "detach the game or close POEFixer.");
        PluginUi::EndCard();
    }

    // --- Diagnostics (collapsed) ---------------------------------------------
    std::string BuildReport() const {
        std::string r = "Get Back My Zoom ";
        r += PluginSDK_GetInfo()->version;
        r += "\nSession: ";
        r += Gbmz::SessionStateLabel(m_status.session);
        r += " - " + m_status.message + "\n";
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Process: %lu, image stamp 0x%08X\n",
                      static_cast<unsigned long>(m_status.pid), m_status.imageStamp);
        r += buf;
        for (size_t i = 0; i < Gbmz::kPatchCount; ++i) {
            const auto& st = m_status.patches[i];
            std::snprintf(buf, sizeof(buf), "%s: %s, matches %d, rva 0x%X, original [%s]%s%s\n",
                          Gbmz::Definitions()[i].id, Gbmz::PatchStateLabel(st.state), st.matches, st.rva,
                          st.originalHex.c_str(), st.detail.empty() ? "" : " - ", st.detail.c_str());
            r += buf;
        }
        r += "Recent:\n";
        for (const auto& line : m_status.recent) r += "  " + line + "\n";
        return r;
    }

    void DrawSectionDiagnostics() {
        if (PluginUi::BeginDiagnostics("Each change's state, location and original bytes, recent activity and a "
                                       "report to copy.")) {
            // Short columns fit their content; the long byte string takes the rest
            // and wraps, so no id is cut on a narrow tab.
            const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
            if (ImGui::BeginTable("###patches", 4, flags)) {
                ImGui::TableSetupColumn("Feature", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < Gbmz::kPatchCount; ++i) {
                    const auto& st = m_status.patches[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(Gbmz::Definitions()[i].id);
                    ImGui::TableNextColumn();
                    ImGui::TextColored(PatchColor(st.state), "%s", Gbmz::PatchStateLabel(st.state));
                    if (!st.detail.empty()) PluginUi::Tip(st.detail.c_str());
                    ImGui::TableNextColumn();
                    if (st.rva) ImGui::Text("game+0x%X", st.rva);
                    else ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    ImGui::PushTextWrapPos(0.0f);
                    if (st.originalHex.empty()) ImGui::TextDisabled("-");
                    else ImGui::TextUnformatted(st.originalHex.c_str());
                    ImGui::PopTextWrapPos();
                }
                ImGui::EndTable();
            }
            ImGui::Spacing();
            ImGui::TextDisabled("Recent activity");
            if (m_status.recent.empty()) {
                ImGui::TextDisabled("(nothing yet)");
            } else {
                ImGui::PushTextWrapPos(0.0f);
                for (const auto& line : m_status.recent) ImGui::TextUnformatted(line.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::Spacing();
            if (PluginUi::SubtleButton(ICON_FA_CLIPBOARD "  Copy report###copy")) {
                const std::string report = BuildReport();
                ImGui::SetClipboardText(report.c_str());
            }
            PluginUi::Tip("Copies the state above as text, for a bug report.");
            PluginUi::EndDiagnostics();
        }
    }
};

extern "C" PLUGIN_API PluginSDK::Plugin* CreatePlugin() { return new GetBackMyZoomPlugin(); }

extern "C" PLUGIN_API void DestroyPlugin(PluginSDK::Plugin* p) { delete p; }
