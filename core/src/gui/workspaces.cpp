#include <gui/workspaces.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/tuner.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <core.h>
#include <module.h>
#include <signal_path/signal_path.h>
#include <utils/flog.h>
#include <set>
#include <cmath>
#include "../../decoder_modules/radio/src/radio_interface.h"

namespace workspaces {

    static const char* KEYS[_COUNT] = { "aviation", "ham", "explore" };
    static const char* LABELS[_COUNT] = { "Aviação", "Radioamador", "Explorar" };
    static const char* RADIO_VFO = "Radio";

    // Sintonia inicial de cada aba (usada na primeira vez)
    struct Tuning {
        double freq;
        int mode;
        float bandwidth;
    };
    static const Tuning DEFAULTS[_COUNT] = {
        { 118.1e6, RADIO_IFACE_MODE_AM, 8333.0f },   // banda aérea
        { 145.0e6, RADIO_IFACE_MODE_NFM, 12500.0f }, // 2 m
        { 0, -1, 0 },                                // Explorar: mantém o que estiver
    };

    // Painéis de cada aba (nomes como aparecem no menu lateral)
    static const std::set<std::string> AVIATION_MENUS = {
        "Source", "Radio", "Frequency Manager", "ADS-B", "Recorder", "Noise Reduction logmmse", "Sinks"
    };
    static const std::set<std::string> HAM_MENUS = {
        "Source", "Radio", "Frequency Manager", "Recorder", "Noise Reduction logmmse", "Sinks",
        "FT8/FT4 Decoder", "Scanner", "Band Plan"
    };

    static Workspace cur = AVIATION;
    static bool initialized = false;

    Workspace current() { return cur; }

    bool isMenuVisible(const std::string& name) {
        switch (cur) {
        case AVIATION: return AVIATION_MENUS.count(name) > 0;
        case HAM: return HAM_MENUS.count(name) > 0;
        default: return true;
        }
    }

    void init() {
        core::configManager.acquire();
        if (core::configManager.conf.contains("workspace")) {
            std::string k = core::configManager.conf["workspace"];
            for (int i = 0; i < _COUNT; i++) {
                if (k == KEYS[i]) { cur = (Workspace)i; }
            }
        }
        core::configManager.release();
        initialized = true;
    }

    // ------------------------------------------------------------------ estado
    static bool radioExists() {
        return sigpath::vfoManager.vfoExists(RADIO_VFO) && core::modComManager.interfaceExists(RADIO_VFO);
    }

    // Lista de canais do Frequency Manager usada em cada aba
    static ModuleManager::Instance* freqManager() {
        for (auto& [name, inst] : core::moduleManager.instances) {
            if (inst.module.info && inst.instance && std::string(inst.module.info->name) == "frequency_manager") { return inst.instance; }
        }
        return NULL;
    }

    static std::string currentList() {
        auto fm = freqManager();
        if (!fm) { return ""; }
        json j = json::parse(fm->handleDebugCommand("get_current_list", ""), nullptr, false);
        if (j.is_discarded() || !j.contains("current_list") || !j["current_list"].is_string()) { return ""; }
        return j["current_list"];
    }

    static void selectList(const std::string& list) {
        auto fm = freqManager();
        if (fm && !list.empty()) { fm->handleDebugCommand("set_current_list", list); }
    }

    static void saveState(Workspace ws) {
        std::string list = currentList();
        if (!list.empty()) {
            core::configManager.acquire();
            core::configManager.conf["workspaceState"][KEYS[ws]]["list"] = list;
            core::configManager.release(true);
        }
        if (!radioExists()) { return; }
        int mode = -1;
        float bw = 0;
        core::modComManager.callInterface(RADIO_VFO, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);
        core::modComManager.callInterface(RADIO_VFO, RADIO_IFACE_CMD_GET_BANDWIDTH, NULL, &bw);
        double freq = gui::waterfall.getCenterFrequency() + sigpath::vfoManager.getOffset(RADIO_VFO);
        core::configManager.acquire();
        json& s = core::configManager.conf["workspaceState"][KEYS[ws]];
        s["frequency"] = freq;
        s["mode"] = mode;
        s["bandwidth"] = bw;
        core::configManager.release(true);
    }

    static void restoreState(Workspace ws) {
        Tuning t = DEFAULTS[ws];
        std::string list;
        core::configManager.acquire();
        json& all = core::configManager.conf["workspaceState"];
        if (all.contains(KEYS[ws])) {
            json& s = all[KEYS[ws]];
            list = s.value("list", std::string());
            t.freq = s.value("frequency", t.freq);
            t.mode = s.value("mode", t.mode);
            t.bandwidth = s.value("bandwidth", t.bandwidth);
        }
        core::configManager.release();

        selectList(list);
        if (t.freq <= 0 || !radioExists()) { return; }
        if (t.mode >= 0) {
            int curMode = -1;
            core::modComManager.callInterface(RADIO_VFO, RADIO_IFACE_CMD_GET_MODE, NULL, &curMode);
            if (curMode != t.mode) { core::modComManager.callInterface(RADIO_VFO, RADIO_IFACE_CMD_SET_MODE, &t.mode, NULL); }
        }
        if (t.bandwidth > 0) {
            core::modComManager.callInterface(RADIO_VFO, RADIO_IFACE_CMD_SET_BANDWIDTH, &t.bandwidth, NULL);
        }
        tuner::tune(tuner::TUNER_MODE_NORMAL, RADIO_VFO, t.freq);
    }

    // O RTL-SDR tem um sintonizador só: ao trocar de aba, para o que estiver
    // mexendo na sintonia (scanner da lista e ADS-B).
    static void stopTuningUsers() {
        for (auto& [name, inst] : core::moduleManager.instances) {
            if (!inst.module.info || !inst.instance) { continue; }
            std::string mod = inst.module.info->name;
            if (mod == "frequency_manager") { inst.instance->handleDebugCommand("stop_scanner", ""); }
            if (mod == "adsb_decoder") { inst.instance->handleDebugCommand("stop", ""); }
        }
    }

    void switchTo(Workspace ws) {
        if (ws == cur || ws < 0 || ws >= _COUNT) { return; }
        stopTuningUsers();
        saveState(cur);
        cur = ws;
        restoreState(ws);
        core::configManager.acquire();
        core::configManager.conf["workspace"] = KEYS[ws];
        core::configManager.release(true);
        flog::info("Aba: {}", LABELS[ws]);
    }

    // ------------------------------------------------------------------ desenho
    void drawTabs() {
        if (!initialized) { init(); }
        float w = ImGui::GetContentRegionAvail().x;
        float h = ImGui::GetFrameHeight() * 1.45f;
        float pad = 3.0f * style::uiScale;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGuiStyle& st = ImGui::GetStyle();

        dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h + 2 * pad), ImGui::GetColorU32(ImGuiCol_FrameBg), st.FrameRounding + pad);
        float tw = (w - 2 * pad) / _COUNT;
        ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
        ImU32 activeBg = ImGui::GetColorU32(ImGuiCol_ButtonActive);
        ImU32 hoverBg = ImGui::GetColorU32(ImGuiCol_ButtonHovered);
        ImU32 textOn = ImGui::GetColorU32(ImGuiCol_Text);
        ImU32 textOff = ImGui::GetColorU32(ImGuiCol_TextDisabled);

        for (int i = 0; i < _COUNT; i++) {
            ImVec2 a(p0.x + pad + tw * i, p0.y + pad);
            ImVec2 b(a.x + tw, a.y + h);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(i);
            bool clicked = ImGui::InvisibleButton("##workspace_tab", ImVec2(tw, h));
            bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            bool on = (cur == i);
            if (on) {
                dl->AddRectFilled(a, b, activeBg, st.FrameRounding);
                dl->AddRectFilled(ImVec2(a.x + tw * 0.2f, b.y - 2.5f * style::uiScale), ImVec2(b.x - tw * 0.2f, b.y), accent, 2.0f);
            }
            else if (hovered) {
                dl->AddRectFilled(a, b, hoverBg, st.FrameRounding);
            }
            ImVec2 ts = ImGui::CalcTextSize(LABELS[i]);
            dl->AddText(ImVec2(a.x + (tw - ts.x) / 2, a.y + (h - ts.y) / 2), on ? textOn : textOff, LABELS[i]);
            if (clicked) { switchTo((Workspace)i); }
        }
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h + 2 * pad));
        ImGui::Dummy(ImVec2(w, 6.0f * style::uiScale));
    }
}
