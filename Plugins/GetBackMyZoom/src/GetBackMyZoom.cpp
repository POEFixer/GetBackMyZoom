// Get Back My Zoom: restores a much further camera zoom-out and a few other
// visual changes (no Atlas fog, full map reveal, larger light radius) by
// changing a few bytes of the running game, and undoes them again.
//
// Originally created by Omer Faruk ARPA; maintained by POEFixer since
// October 2026. Licensed under the GNU General Public License v3.0.
//
// Texts follow the program language (PluginI18n.h): lang/<code>.json next to the
// DLL, English compiled in through lang_en.generated.h (python
// tools/plugin_i18n/gen.py Plugins/GetBackMyZoom after editing lang/en.json). The
// patcher's worker hands its texts on as keys (patch/LocText.h): they are worded
// here, as drawn; its log lines and the copied report stay English.
#include "../../../POEFixer/plugin_sdk/PluginSDK.h"
#include "lang_en.generated.h"   // once per DLL, after PluginSDK.h
#include "../../../POEFixer/imgui/IconsFontAwesome6.h"
#include "../../../POEFixer/plugin_sdk/PluginUi.h"

#include "Settings.h"
#include "patch/Patcher.h"

#include <imgui.h>
#include <imgui_internal.h>   // ImTextInitClassifiers (see BindImGui)

#include <sal.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <string>

// Plugin info: the host checks it before CreatePlugin; the publish tool reads it.
POEFIXER_PLUGIN_INFO(
    "GetBackMyZoom",
    "Get Back My Zoom",
    "1.1.7",
    "Omer Faruk ARPA, POEFixer",
    "Restores a much further camera zoom-out, removes Atlas fog, reveals the whole map and enlarges the light radius, safely undone when switched off.")

// The English of a worker text: the compiled-in lang/en.json (constant data, any thread).
std::string Gbmz::English(const LocText& t) {
    if (t.Empty()) return {};
    for (const auto& e : kPluginLangEn)
        if (t.key == e.key) return FillPlaceholders(e.text, t.args);
    return t.key;
}

namespace {

namespace I18n = PluginSDK::I18n;
using Clock = std::chrono::steady_clock;
using GetBackMyZoomConfig::kLightMax;
using GetBackMyZoomConfig::kLightMin;
using Gbmz::PatchState;
using Gbmz::SessionState;

// A worker text in the program language (drawing thread only).
std::string Text(const Gbmz::LocText& t) {
    if (t.Empty()) return {};
    return Gbmz::FillPlaceholders(I18n::Tr(t.key.c_str()), t.args);
}

// printf for a TrF argument: Num("%.0f", value). The format is code, never a translation.
std::string Num(_In_z_ _Printf_format_string_ const char* format, ...) {
    char buf[128];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return buf;
}

// "<icon>  <text>###<id>": a button label whose ImGui id never changes with the language.
std::string IconLabel(const char* icon, const char* key, const char* id) {
    return std::string(icon) + "  " + I18n::Tr(key) + "###" + id;
}

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
        case PatchState::Unknown:  return I18n::Tr("tip_waiting_scan");
        case PatchState::Off:      return I18n::Tr("tip_off");
        case PatchState::On:       return I18n::Tr("tip_active");
        case PatchState::NotFound: return I18n::Tr("tip_not_found");
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
            PluginUi::Hint(I18n::Tr("host_too_old"));
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
        PluginUi::Heading(ICON_FA_PLUG, I18n::Tr("connection_title"), I18n::Tr("connection_help"));

        Pill(I18n::Tr(Gbmz::SessionStateLabel(m_status.session)), SessionColor(m_status.session));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::PushTextWrapPos(0.0f);
        const std::string message = Text(m_status.message);
        ImGui::TextUnformatted(message.empty() ? I18n::Tr("message_waiting") : message.c_str());
        ImGui::PopTextWrapPos();

        ImGui::AlignTextToFramePadding();
        if (m_status.pid)
            ImGui::TextDisabled("%s", I18n::TrF("connection_process", {{"pid", Num("%lu", static_cast<unsigned long>(m_status.pid))}}).c_str());
        else
            ImGui::TextDisabled("%s", I18n::Tr("connection_no_process"));
        const std::string rescanLabel = IconLabel(ICON_FA_ARROWS_ROTATE, I18n::Key("connection_rescan"), "rescan");
        SameLineRightOrWrap(PluginUi::ButtonWidth(rescanLabel.c_str()));
        const bool canRescan = m_status.session == SessionState::Ready || m_status.session == SessionState::ScanFailed;
        ImGui::BeginDisabled(!canRescan);
        if (PluginUi::SubtleButton(rescanLabel.c_str())) m_patcher.Rescan();
        ImGui::EndDisabled();
        PluginUi::Tip(I18n::Tr("connection_rescan_tip"));
        PluginUi::EndCard();
    }

    // --- Features ---------------------------------------------------------------
    void DrawFeatureRow(size_t i) {
        const Gbmz::PatchDef& def = Gbmz::Definitions()[i];
        const Gbmz::PatchStatus& st = m_status.patches[i];

        char id[24];
        std::snprintf(id, sizeof(id), "want%zu", i);
        bool want = m_settings.want[i];
        if (PluginUi::Check(I18n::Tr(def.label), id, &want, I18n::Tr(def.tip))) {
            m_settings.want[i] = want;
            MarkDirty();
            PostRequest();
        }

        const bool sessionUp = m_status.session == SessionState::Ready;
        const char* text = I18n::Tr(Gbmz::PatchStateLabel(sessionUp ? st.state : PatchState::Unknown));
        const ImVec4 color = sessionUp ? PatchColor(st.state) : PluginUi::Colors().TextDisabled;
        const std::string detail = Text(st.detail);
        const char* tip = !detail.empty() ? detail.c_str() : PatchTip(sessionUp ? st.state : PatchState::Unknown);
        SameLineRightOrWrap(StatusPillWidth(text));
        Pill(text, color, tip);
    }

    void DrawSectionFeatures() {
        PluginUi::BeginCard("###features");
        PluginUi::Heading(ICON_FA_MAGNIFYING_GLASS_MINUS, I18n::Tr("features_title"), I18n::Tr("features_help"));
        DrawFeatureRow(Gbmz::kInfiniteZoom);
        DrawFeatureRow(Gbmz::kNoAtlasFog);
        DrawFeatureRow(Gbmz::kRevealMap);

        ImGui::Spacing();
        PluginUi::Heading(ICON_FA_LIGHTBULB, I18n::Tr("light_title"));
        DrawFeatureRow(Gbmz::kLightRadius);
        {
            const Gbmz::PatchStatus& st = m_status.patches[Gbmz::kLightRadius];
            const float rowX = ImGui::GetCursorPosX();
            const char* radiusLabel = I18n::Tr("light_radius");
            const PluginUi::LabelColumn col({radiusLabel});
            ImGui::BeginDisabled(!m_settings.want[Gbmz::kLightRadius]);
            col.Row(radiusLabel);
            float v = m_settings.lightRadius;
            if (ImGui::SliderFloat("###radius", &v, kLightMin, kLightMax, "%.0f",
                                   ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) {
                m_settings.lightRadius = v;
                MarkDirty();
                PostRequest();
            }
            if (st.hasGameValue)
                PluginUi::Tip(I18n::TrF("light_radius_tip_game", {{"value", Num("%.0f", st.gameValue)}}).c_str());
            else
                PluginUi::Tip(I18n::Tr("light_radius_tip"));

            // Beside the slider; on a narrow tab under it, in the control column.
            const std::string resetLabel = IconLabel(ICON_FA_ROTATE_LEFT, I18n::Key("light_game_value"), "gameValue");
            ImGui::SameLine();
            if (ImGui::GetContentRegionAvail().x < PluginUi::ButtonWidth(resetLabel.c_str())) {
                ImGui::NewLine();
                ImGui::SetCursorPosX(rowX + col.Width());
            }
            ImGui::BeginDisabled(!st.hasGameValue);
            if (PluginUi::SubtleButton(resetLabel.c_str())) {
                m_settings.lightRadius = std::clamp(st.gameValue, kLightMin, kLightMax);
                MarkDirty();
                PostRequest();
            }
            ImGui::EndDisabled();
            PluginUi::Tip(I18n::Tr("light_game_value_tip"));
            ImGui::EndDisabled();
        }

        ImGui::Spacing();
        PluginUi::Hint(I18n::Tr("features_hint"));
        PluginUi::EndCard();
    }

    // --- Diagnostics (collapsed) ---------------------------------------------
    // The copied report is a debug dump: English (worker texts through Gbmz::English).
    std::string BuildReport() const {
        std::string r = "Get Back My Zoom ";
        r += PluginSDK_GetInfo()->version;
        r += "\nSession: ";
        r += Gbmz::English(Gbmz::SessionStateLabel(m_status.session));
        r += " - " + Gbmz::English(m_status.message) + "\n";
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Process: %lu, image stamp 0x%08X\n",
                      static_cast<unsigned long>(m_status.pid), m_status.imageStamp);
        r += buf;
        for (size_t i = 0; i < Gbmz::kPatchCount; ++i) {
            const auto& st = m_status.patches[i];
            const std::string detail = Gbmz::English(st.detail);
            std::snprintf(buf, sizeof(buf), "%s: %s, matches %d, rva 0x%X, original [%s]",
                          Gbmz::Definitions()[i].id, Gbmz::English(Gbmz::PatchStateLabel(st.state)).c_str(),
                          st.matches, st.rva, st.originalHex.c_str());
            r += buf;
            if (!detail.empty()) r += " - " + detail;
            r += "\n";
        }
        r += "Recent:\n";
        for (const auto& line : m_status.recent) r += "  " + line + "\n";
        return r;
    }

    void DrawSectionDiagnostics() {
        if (PluginUi::BeginDiagnostics(I18n::Tr("diag_tip"))) {
            // Short columns fit their content; the long byte string takes the rest
            // and wraps, so no id is cut on a narrow tab.
            const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
            if (ImGui::BeginTable("###patches", 4, flags)) {
                ImGui::TableSetupColumn(I18n::TrId("diag_col_feature"), ImGuiTableColumnFlags_WidthFixed, 0.0f);
                ImGui::TableSetupColumn(I18n::TrId("diag_col_state"), ImGuiTableColumnFlags_WidthFixed, 0.0f);
                ImGui::TableSetupColumn(I18n::TrId("diag_col_location"), ImGuiTableColumnFlags_WidthFixed, 0.0f);
                ImGui::TableSetupColumn(I18n::TrId("diag_col_bytes"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < Gbmz::kPatchCount; ++i) {
                    const auto& st = m_status.patches[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(Gbmz::Definitions()[i].id);   // the stable id, as in the settings file
                    ImGui::TableNextColumn();
                    ImGui::TextColored(PatchColor(st.state), "%s", I18n::Tr(Gbmz::PatchStateLabel(st.state)));
                    if (!st.detail.Empty()) PluginUi::Tip(Text(st.detail).c_str());
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
            ImGui::TextDisabled("%s", I18n::Tr("diag_recent"));
            if (m_status.recent.empty()) {
                ImGui::TextDisabled("%s", I18n::Tr("diag_recent_none"));
            } else {
                // The log's own lines (English, as in the POEFixer log).
                ImGui::PushTextWrapPos(0.0f);
                for (const auto& line : m_status.recent) ImGui::TextUnformatted(line.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::Spacing();
            const std::string copyLabel = IconLabel(ICON_FA_CLIPBOARD, I18n::Key("diag_copy"), "copy");
            if (PluginUi::SubtleButton(copyLabel.c_str())) {
                const std::string report = BuildReport();
                ImGui::SetClipboardText(report.c_str());
            }
            PluginUi::Tip(I18n::Tr("diag_copy_tip"));
            PluginUi::EndDiagnostics();
        }
    }
};

extern "C" PLUGIN_API PluginSDK::Plugin* CreatePlugin() { return new GetBackMyZoomPlugin(); }

extern "C" PLUGIN_API void DestroyPlugin(PluginSDK::Plugin* p) { delete p; }
