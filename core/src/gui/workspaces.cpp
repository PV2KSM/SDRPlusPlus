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
#include <map>
#include <cmath>
#include <vector>
#include <algorithm>
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
        "Source", "Radio", "Frequency Manager", "ADS-B", "ACARS", "Recorder", "Noise Reduction logmmse", "Sinks"
    };
    static const std::set<std::string> HAM_MENUS = {
        "Source", "Radio", "Frequency Manager", "Recorder", "Noise Reduction logmmse", "Sinks",
        "FT8/FT4 Decoder", "Scanner", "Band Plan", "VHF Digital Modes"
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

    std::string displayName(const std::string& name) {
        static const std::map<std::string, std::string> N = {
            { "Source", "Fonte (receptor)" }, { "Radio", "Rádio" }, { "Frequency Manager", "Canais e scanner" },
            { "Recorder", "Gravador" }, { "Sinks", "Saída de áudio" }, { "Noise Reduction logmmse", "Redução de ruído" },
            { "ADS-B", "ADS-B · radar" }, { "ACARS", "ACARS · CPDLC" }, { "Band Plan", "Plano de bandas" },
            { "Display", "Tela" }, { "Theme", "Tema" }, { "VFO Color", "Cores dos VFOs" },
            { "Module Manager", "Módulos" }, { "Scanner", "Varredura de faixa" }, { "FT8/FT4 Decoder", "FT8 / FT4" },
            { "VHF Digital Modes", "DMR · P25" }, { "TETRA Demodulator", "TETRA" },
        };
        auto it = N.find(name);
        return it == N.end() ? name : it->second;
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
            if (mod == "acars_decoder") { inst.instance->handleDebugCommand("stop", ""); }
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

    // ------------------------------------------------------------------ modos e larguras
    struct ModeInfo {
        std::string name;
        int id;
    };
    static const int MODE_DSD = 0x1301;

    // Modos oferecidos em cada aba (Explorar: todos os que o Radio tiver)
    static const std::vector<int> AVIATION_MODES = { RADIO_IFACE_MODE_AM, RADIO_IFACE_MODE_NFM, MODE_DSD };
    static const std::vector<int> HAM_MODES = { RADIO_IFACE_MODE_NFM, RADIO_IFACE_MODE_USB, RADIO_IFACE_MODE_LSB,
                                                RADIO_IFACE_MODE_CW, RADIO_IFACE_MODE_AM, MODE_DSD };

    static std::vector<ModeInfo> radioModes;
    static double modesFetchedAt = -100;

    static double nowSec() {
        return ImGui::GetTime();
    }

    static ModuleManager::Instance* instanceOf(const std::string& moduleName) {
        for (auto& [name, inst] : core::moduleManager.instances) {
            if (inst.module.info && inst.instance && moduleName == inst.module.info->name) { return inst.instance; }
        }
        return NULL;
    }

    static ModuleManager::Instance* radioInstance() {
        auto it = core::moduleManager.instances.find(RADIO_VFO);
        if (it == core::moduleManager.instances.end() || !it->second.instance) { return NULL; }
        return it->second.instance;
    }

    static void refreshModes() {
        // Os modos digitais (DSD) entram depois que o plugin carrega: relê de tempos em tempos
        if (nowSec() - modesFetchedAt < (radioModes.empty() ? 1.0 : 5.0)) { return; }
        modesFetchedAt = nowSec();
        auto r = radioInstance();
        if (!r) { return; }
        json j = json::parse(r->handleDebugCommand("list_demods", ""), nullptr, false);
        if (j.is_discarded() || !j.contains("demods")) { return; }
        std::vector<ModeInfo> m;
        for (auto& d : j["demods"]) {
            m.push_back({ d.value("name", std::string("?")), d.value("id", -1) });
        }
        radioModes = m;
    }

    static const ModeInfo* findMode(int id) {
        for (auto& m : radioModes) {
            if (m.id == id) { return &m; }
        }
        return NULL;
    }

    static std::string modeLabel(const ModeInfo& m) {
        if (m.id == MODE_DSD) { return "DMR"; }
        if (m.name == "oldDSD") { return "DSD (antigo)"; }
        return m.name;
    }

    // Larguras sugeridas por modo (Hz). A aba Aviação tem as da banda aérea.
    static std::vector<float> presetsFor(Workspace ws, int mode) {
        switch (mode) {
        case RADIO_IFACE_MODE_AM:
            if (ws == AVIATION) { return { 6000, 8333, 12500, 25000 }; }
            return { 4000, 6000, 9000, 12500 };
        case RADIO_IFACE_MODE_NFM:
            if (ws == AVIATION) { return { 8333, 12500, 25000 }; }
            return { 6250, 12500, 16000, 25000 };
        case RADIO_IFACE_MODE_WFM: return { 100000, 150000, 200000, 250000 };
        case RADIO_IFACE_MODE_DSB: return { 4000, 6000, 9000 };
        case RADIO_IFACE_MODE_USB:
        case RADIO_IFACE_MODE_LSB: return { 1800, 2400, 2700, 3000 };
        case RADIO_IFACE_MODE_CW: return { 200, 500, 800 };
        case MODE_DSD: return { 12500, 25000, 50000 };
        default: return {};
        }
    }

    // Passo dos botões - / + de largura em cada modo
    static float stepFor(int mode) {
        switch (mode) {
        case MODE_DSD: return 5000;
        case RADIO_IFACE_MODE_WFM: return 25000;
        case RADIO_IFACE_MODE_NFM: return 2500;
        case RADIO_IFACE_MODE_AM:
        case RADIO_IFACE_MODE_DSB: return 500;
        case RADIO_IFACE_MODE_USB:
        case RADIO_IFACE_MODE_LSB: return 100;
        case RADIO_IFACE_MODE_CW: return 50;
        default: return 1000;
        }
    }

    static std::string bwText(float hz) {
        char buf[32];
        if (hz >= 100000) { snprintf(buf, sizeof(buf), "%.0fk", hz / 1000.0f); }
        else if (hz >= 1000) {
            float k = hz / 1000.0f;
            if (std::fabs(k - std::round(k)) < 0.01f) { snprintf(buf, sizeof(buf), "%.0fk", k); }
            else if (std::fabs(k * 10 - std::round(k * 10)) < 0.05f) { snprintf(buf, sizeof(buf), "%.1fk", k); }
            else { snprintf(buf, sizeof(buf), "%.2fk", k); }
        }
        else { snprintf(buf, sizeof(buf), "%.0f Hz", hz); }
        return buf;
    }

    // Largura escolhida em cada aba, por modo: workspaceState[aba]["bw"][modo]
    static float storedBandwidth(Workspace ws, int mode) {
        float bw = 0;
        core::configManager.acquire();
        json& all = core::configManager.conf["workspaceState"];
        if (all.contains(KEYS[ws]) && all[KEYS[ws]].contains("bw")) {
            json& b = all[KEYS[ws]]["bw"];
            std::string k = std::to_string(mode);
            if (b.contains(k) && b[k].is_number()) { bw = b[k]; }
        }
        core::configManager.release();
        return bw;
    }

    static void storeBandwidth(Workspace ws, int mode, float bw) {
        core::configManager.acquire();
        core::configManager.conf["workspaceState"][KEYS[ws]]["bw"][std::to_string(mode)] = bw;
        core::configManager.release(true);
    }

    static int getMode() {
        int mode = -1;
        if (radioExists()) { core::modComManager.callInterface(RADIO_VFO, RADIO_IFACE_CMD_GET_MODE, NULL, &mode); }
        return mode;
    }

    static float getBandwidth() {
        float bw = 0;
        if (radioExists()) { core::modComManager.callInterface(RADIO_VFO, RADIO_IFACE_CMD_GET_BANDWIDTH, NULL, &bw); }
        return bw;
    }

    static void setBandwidth(float bw) {
        if (radioExists() && bw > 0) { core::modComManager.callInterface(RADIO_VFO, RADIO_IFACE_CMD_SET_BANDWIDTH, &bw, NULL); }
    }

    static void bandwidthLimits(float& lo, float& hi) {
        lo = 100;
        hi = 250000;
        auto r = radioInstance();
        if (!r) { return; }
        json j = json::parse(r->handleDebugCommand("get_vfo_bandwidth", ""), nullptr, false);
        if (j.is_discarded()) { return; }
        lo = j.value("min_bandwidth", (double)lo);
        hi = j.value("max_bandwidth", (double)hi);
        if (hi <= lo) { hi = lo + 1; }
    }

    // Troca de modo dentro da aba: aplica a última largura usada neste modo nesta aba
    static void selectMode(int mode) {
        if (!radioExists()) { return; }
        if (getMode() != mode) { core::modComManager.callInterface(RADIO_VFO, RADIO_IFACE_CMD_SET_MODE, &mode, NULL); }
        float bw = storedBandwidth(cur, mode);
        if (bw <= 0 && cur == AVIATION && mode == RADIO_IFACE_MODE_AM) { bw = 8333; }
        if (bw > 0) { setBandwidth(bw); }
    }

    // Guarda a largura atual sempre que ela muda (pelo painel, pelo arraste na cascata ou pelos atalhos)
    static int watchedMode = -2;
    static float watchedBw = -1;
    static Workspace watchedWs = _COUNT;
    static bool scannerActive = false;   // atualizado pela barra de status
    static void trackBandwidth(int mode, float bw) {
        if (mode < 0 || bw <= 0) { return; }
        // Com o scanner rodando a largura vem dos canais da lista: não é escolha do usuário
        if (scannerActive) { watchedMode = -2; return; }
        if (mode != watchedMode || cur != watchedWs) {
            watchedMode = mode;
            watchedWs = cur;
            watchedBw = bw;
            return;
        }
        if (std::fabs(bw - watchedBw) > 0.5f && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            watchedBw = bw;
            storeBandwidth(cur, mode, bw);
        }
    }

    // ------------------------------------------------------------------ status (cache)
    struct Status {
        bool scanning = false, receiving = false, holding = false;
        std::string station;
        bool adsbRunning = false;
        int aircraft = 0;
        uint64_t adsbMsgs = 0;
        bool acarsRunning = false;
        int acarsMsgs = 0;
        std::string acarsLast;
        bool dmrSync = false;
        int dmrOffset = 0;
        std::string dmrLast;
        std::string digLabel = "DMR";
        std::string dmrInfo;
        float tone = 0;
    };
    static Status status;
    static double statusAt = -100;

    static void refreshStatus(int mode) {
        if (nowSec() - statusAt < 0.4) { return; }
        statusAt = nowSec();
        Status s;
        if (auto fm = freqManager()) {
            json j = json::parse(fm->handleDebugCommand("get_scanner_status", ""), nullptr, false);
            if (!j.is_discarded()) {
                s.scanning = j.value("scanning", false);
                s.receiving = j.value("receiving", false);
                s.holding = j.value("holding", false);
                if (j.contains("current_station") && j["current_station"].is_string()) { s.station = j["current_station"]; }
            }
        }
        if (auto ad = instanceOf("adsb_decoder")) {
            json j = json::parse(ad->handleDebugCommand("get_stats", ""), nullptr, false);
            if (!j.is_discarded()) {
                s.adsbRunning = j.value("running", false);
                s.aircraft = j.value("aircraft", 0);
                s.adsbMsgs = j.value("accepted", (uint64_t)0);
            }
        }
        if (auto ac = instanceOf("acars_decoder")) {
            json j = json::parse(ac->handleDebugCommand("get_stats", ""), nullptr, false);
            if (!j.is_discarded()) {
                s.acarsRunning = j.value("running", false);
                s.acarsMsgs = j.value("messages", 0);
                s.acarsLast = j.value("last", std::string());
            }
        }
        if (mode == RADIO_IFACE_MODE_NFM) {
            if (auto r = radioInstance()) {
                json j = json::parse(r->handleDebugCommand("get_tone", ""), nullptr, false);
                if (!j.is_discarded()) { s.tone = j.value("tone", 0.0f); }
            }
        }
        if (mode == MODE_DSD) {
            for (auto& [name, inst] : core::moduleManager.instances) {
                if (!inst.module.info || !inst.instance || std::string(inst.module.info->name) != "ch_extravhf_decoder") { continue; }
                json j = json::parse(inst.instance->handleDebugCommand("get_dmr_status", RADIO_VFO), nullptr, false);
                if (j.is_discarded() || j.contains("error")) { continue; }
                bool sync = j.contains("sync") && j["sync"].is_boolean() && j["sync"].get<bool>();
                bool voice = j.contains("voice") && j["voice"].is_boolean() && j["voice"].get<bool>();
                int cc = (j.contains("color_code") && j["color_code"].is_number()) ? j["color_code"].get<int>() : -1;
                int off = (j.contains("offset_hz") && j["offset_hz"].is_number()) ? j["offset_hz"].get<int>() : 0;
                s.dmrSync = sync;
                s.dmrOffset = off;
                s.dmrInfo = std::string(voice ? "voz" : "sincronizado") + (cc >= 0 ? " · CC " + std::to_string(cc) : "");
                // última chamada identificada (talkgroup/ID) — mostrada por 15 s
                static uint32_t lastLcCount = 0;
                static double lastLcAt = -100;
                uint32_t lcCount = (j.contains("lc_count") && j["lc_count"].is_number()) ? j["lc_count"].get<uint32_t>() : 0;
                if (lcCount != lastLcCount) { lastLcCount = lcCount; lastLcAt = nowSec(); }
                bool isP25 = j.contains("p25") && j["p25"].is_boolean() && j["p25"].get<bool>();
                s.digLabel = isP25 ? "P25" : "DMR";
                if (isP25) {
                    // P25: NAC, tipo de quadro e, na voz, talkgroup/origem
                    int nac = j.value("p25_nac", 0);
                    std::string type = (j.contains("p25_type") && j["p25_type"].is_string()) ? j["p25_type"].get<std::string>() : "";
                    char b[32];
                    snprintf(b, sizeof(b), "NAC %03X", nac);
                    s.dmrInfo = std::string(type == "TSDU" ? "controle" : (voice ? "voz" : "sincronizado")) + " · " + b;
                    uint32_t tg = j.value("p25_tg", 0u), src = j.value("p25_src", 0u);
                    int alg = j.value("p25_algid", 0x80);
                    if (type != "TSDU" && (tg || src)) {
                        std::string call = "TG " + std::to_string(tg) + " <- " + std::to_string(src);
                        if (alg != 0x80 && alg != 0) { call += " (criptografada)"; }
                        if (j.contains("p25_emergency") && j["p25_emergency"].is_boolean() && j["p25_emergency"].get<bool>()) { call += " EMERGÊNCIA"; }
                        s.dmrInfo += " · " + call;
                        s.dmrLast = call;
                    }
                }
                else if (lcCount > 0 && (sync || nowSec() - lastLcAt < 15)) {
                    bool grp = !j.contains("group") || !j["group"].is_boolean() || j["group"].get<bool>();
                    uint32_t dst = j.value("dst", 0u), src = j.value("src", 0u);
                    int slot = j.value("lc_slot", -1);
                    std::string call = std::string(grp ? "TG " : "ID ") + std::to_string(dst) + " <- " + std::to_string(src);
                    if (slot >= 0) { call += " (slot " + std::to_string(slot + 1) + ")"; }
                    if (j.contains("emergency") && j["emergency"].is_boolean() && j["emergency"].get<bool>()) { call += " EMERGÊNCIA"; }
                    if (j.contains("privacy") && j["privacy"].is_boolean() && j["privacy"].get<bool>()) { call += " (criptografada)"; }
                    s.dmrInfo += " · " + call;
                    s.dmrLast = call;
                }
                if (std::abs(off) >= 300) {
                    char b[48];
                    snprintf(b, sizeof(b), " · AFC %+.1f kHz", off / 1000.0);
                    s.dmrInfo += b;
                }
                break;
            }
        }
        status = s;
        scannerActive = s.scanning;
    }

    // ------------------------------------------------------------------ desenho
    static float chipH() {
        return ImGui::GetFrameHeight() * 1.15f;
    }

    // Botão em forma de pílula; devolve true se clicado
    static bool chip(const char* id, const std::string& label, bool on, float minW = 0, ImU32 onColor = 0) {
        ImGuiStyle& st = ImGui::GetStyle();
        ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        float h = chipH();
        float w = (std::max)(ts.x + st.FramePadding.x * 2.6f, minW);
        ImVec2 a = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
        bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 b(a.x + w, a.y + h);
        ImU32 accent = onColor ? onColor : ImGui::GetColorU32(ImGuiCol_CheckMark);
        if (on) {
            ImVec4 c = ImGui::ColorConvertU32ToFloat4(accent);
            dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.22f)), h / 2);
            dl->AddRect(a, b, accent, h / 2, 0, 1.2f * style::uiScale);
        }
        else {
            dl->AddRectFilled(a, b, ImGui::GetColorU32(hovered ? ImGuiCol_ButtonHovered : ImGuiCol_FrameBg), h / 2);
            dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), h / 2, 0, 1.0f);
        }
        dl->AddText(ImVec2(a.x + (w - ts.x) / 2, a.y + (h - ts.y) / 2), ImGui::GetColorU32(on ? ImGuiCol_Text : ImGuiCol_TextDisabled), label.c_str());
        return clicked;
    }

    // Quebra de linha automática entre grupos quando a largura acaba
    static float flowLeft = 0, flowRight = 0;
    static void flowNext(float nextW, float spacing) {
        ImGui::SameLine(0, spacing);
        if (ImGui::GetCursorScreenPos().x + nextW > flowRight) {
            ImGui::NewLine();
            ImGui::SetCursorScreenPos(ImVec2(flowLeft, ImGui::GetCursorScreenPos().y));
        }
    }

    static void groupLabel(const char* txt) {
        ImGui::AlignTextToFramePadding();
        float h = chipH();
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 ts = ImGui::CalcTextSize(txt);
        ImGui::Dummy(ImVec2(ts.x, h));
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x, p.y + (h - ts.y) / 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), txt);
    }

    static float chipWidth(const std::string& label) {
        return ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.6f;
    }

    // Abas grandes, no estilo de seletor segmentado
    static void drawSegmentTabs(float totalW, float h) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiStyle& st = ImGui::GetStyle();
        float pad = 3.0f * style::uiScale;
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p0, ImVec2(p0.x + totalW, p0.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg), h / 2);
        dl->AddRect(p0, ImVec2(p0.x + totalW, p0.y + h), ImGui::GetColorU32(ImGuiCol_Border), h / 2);
        float tw = (totalW - 2 * pad) / _COUNT;
        float ih = h - 2 * pad;
        ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
        ImVec4 ac = ImGui::ColorConvertU32ToFloat4(accent);
        for (int i = 0; i < _COUNT; i++) {
            ImVec2 a(p0.x + pad + tw * i, p0.y + pad);
            ImVec2 b(a.x + tw, a.y + ih);
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(i);
            bool clicked = ImGui::InvisibleButton("##workspace_tab", ImVec2(tw, ih));
            bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            bool on = (cur == i);
            if (on) {
                dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(ac.x, ac.y, ac.z, 0.85f)), ih / 2);
            }
            else if (hovered) {
                dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_ButtonHovered), ih / 2);
            }
            ImVec2 ts = ImGui::CalcTextSize(LABELS[i]);
            ImU32 txt = on ? IM_COL32(8, 14, 20, 255) : ImGui::GetColorU32(ImGuiCol_Text);
            dl->AddText(ImVec2(a.x + (tw - ts.x) / 2, a.y + (ih - ts.y) / 2), txt, LABELS[i]);
            if (clicked) { switchTo((Workspace)i); }
            (void)st;
        }
        ImGui::SetCursorScreenPos(p0);
        ImGui::Dummy(ImVec2(totalW, h));
    }

    // Procura o sinal mais forte perto do Rádio (na última linha do espectro) e
    // sintoniza no centro dele. Resolve o RTL-SDR com erro de ppm, quando o sinal
    // aparece alguns kHz ao lado da frequência certa.
    static std::string centerMsg;
    static double centerMsgAt = -100;
    static void centerOnSignal(float bw) {
        if (!sigpath::vfoManager.vfoExists(RADIO_VFO)) { return; }
        // DMR: o AFC do decodificador já mediu o erro com precisão
        if (getMode() == MODE_DSD && std::abs(status.dmrOffset) >= 200) {
            double f0 = gui::waterfall.getCenterFrequency() + sigpath::vfoManager.getOffset(RADIO_VFO);
            double f = std::round((f0 + status.dmrOffset) / 100.0) * 100.0;
            tuner::tune(tuner::TUNER_MODE_NORMAL, RADIO_VFO, f);
            char buf[96];
            snprintf(buf, sizeof(buf), "DMR em %.4f MHz (AFC %+.1f kHz)", f / 1e6, status.dmrOffset / 1000.0);
            centerMsg = buf;
            centerMsgAt = nowSec();
            return;
        }
        int width = 0;
        float* fft = gui::waterfall.acquireLatestFFT(width);
        if (!fft || width < 16) {
            gui::waterfall.releaseLatestFFT();
            return;
        }
        std::vector<float> row(fft, fft + width);
        gui::waterfall.releaseLatestFFT();
        double center = gui::waterfall.getCenterFrequency();
        double viewBw = gui::waterfall.getViewBandwidth();
        double start = center + gui::waterfall.getViewOffset() - viewBw / 2.0;
        double hzPerPx = viewBw / width;
        double f0 = center + sigpath::vfoManager.getOffset(RADIO_VFO);
        std::vector<float> sorted(row);
        std::nth_element(sorted.begin(), sorted.begin() + width / 2, sorted.end());
        float floorDb = sorted[width / 2];
        double span = (std::max)(25000.0, (double)bw * 2.0);
        int a = (int)std::floor((f0 - span - start) / hzPerPx), b = (int)std::ceil((f0 + span - start) / hzPerPx);
        a = (std::max)(a, 0);
        b = (std::min)(b, width - 1);
        int best = -1;
        for (int i = a; i <= b; i++) {
            if (best < 0 || row[i] > row[best]) { best = i; }
        }
        if (best < 0 || row[best] < floorDb + 8.0f) {
            centerMsg = "Nenhum sinal forte perto";
            centerMsgAt = nowSec();
            return;
        }
        // região contínua acima do ruído em volta do pico; centro pela potência
        float thr = (std::max)(floorDb + 4.0f, row[best] - 20.0f);
        int l = best, r = best;
        while (l > a && row[l - 1] > thr) { l--; }
        while (r < b && row[r + 1] > thr) { r++; }
        double sw = 0, sf = 0;
        for (int i = l; i <= r; i++) {
            double p = std::pow(10.0, (row[i] - floorDb) / 10.0);
            sw += p;
            sf += p * (start + (i + 0.5) * hzPerPx);
        }
        double f = std::round((sf / sw) / 100.0) * 100.0;
        tuner::tune(tuner::TUNER_MODE_NORMAL, RADIO_VFO, f);
        char buf[96];
        snprintf(buf, sizeof(buf), "Sinal em %.4f MHz (%+.1f kHz)", f / 1e6, (f - f0) / 1000.0);
        centerMsg = buf;
        centerMsgAt = nowSec();
        flog::info("Centralizar: {}", centerMsg);
    }

    // ------------------------------------------------------------------ faixas
    // Trecho de uma faixa: ao tocar, sintoniza já com o modo e a largura certos
    struct Seg {
        double lo, hi;
        const char* name;
        int mode;
        float bw;
    };
    struct Mark {
        double f;
        const char* name;
    };
    struct Band {
        const char* label;
        double lo, hi;
        double home;      // onde cai ao tocar no botão da faixa
        int mode;
        float bw;
        bool hf;          // abaixo de 24 MHz: precisa do modo HF do RTL-SDR v3
        std::vector<Seg> plan;
    };

    static const int AM = RADIO_IFACE_MODE_AM, FM = RADIO_IFACE_MODE_NFM, USB = RADIO_IFACE_MODE_USB,
                     LSB = RADIO_IFACE_MODE_LSB, CW = RADIO_IFACE_MODE_CW;

    // Aviação: 108-137 MHz (logo depois do FM comercial)
    static const std::vector<Seg> AIR_PLAN = {
        { 108.000e6, 118.000e6, "Navegação · VOR / ILS", AM, 12500 },
        { 118.000e6, 137.000e6, "Comunicação · torre, aproximação, centro", AM, 8333 },
    };
    static const std::vector<Mark> AIR_MARKS = {
        { 121.500e6, "121,500 emergência" },
        { 131.550e6, "ACARS" },
    };

    // Radioamador no Brasil: limites da Resolução Anatel 697/2018; plano de 2 m e 70 cm
    // conforme o plano de frequências usado no país (IARU Região 2)
    static const std::vector<Band> HAM_BANDS = {
        { "6 m", 50e6, 54e6, 50.125e6, USB, 2700, false, {} },
        { "2 m", 144e6, 148e6, 145.000e6, FM, 12500, false, {
            { 144.000e6, 144.100e6, "CW", CW, 500 },
            { 144.100e6, 144.300e6, "SSB", USB, 2700 },
            { 144.300e6, 144.500e6, "Satélite", FM, 12500 },
            { 144.500e6, 144.600e6, "FM simplex", FM, 12500 },
            { 144.600e6, 144.900e6, "Repetidoras (entrada)", FM, 12500 },
            { 144.900e6, 145.100e6, "Pacote", FM, 12500 },
            { 145.100e6, 145.200e6, "FM simplex", FM, 12500 },
            { 145.200e6, 145.500e6, "Repetidoras (saída)", FM, 12500 },
            { 145.500e6, 145.565e6, "Todos os modos", FM, 12500 },
            { 145.565e6, 145.575e6, "APRS", FM, 12500 },
            { 145.575e6, 148.000e6, "FM simplex e repetidoras", FM, 12500 },
        } },
        { "70 cm", 430e6, 440e6, 433.000e6, FM, 12500, false, {
            { 430.000e6, 431.000e6, "Todos os modos", FM, 12500 },
            { 431.000e6, 432.000e6, "Dados", FM, 12500 },
            { 432.000e6, 432.100e6, "CW", CW, 500 },
            { 432.100e6, 433.000e6, "SSB · CW", USB, 2700 },
            { 433.000e6, 433.500e6, "FM simplex", FM, 12500 },
            { 433.500e6, 433.600e6, "APRS", FM, 12500 },
            { 433.600e6, 434.000e6, "FM simplex", FM, 12500 },
            { 434.000e6, 435.000e6, "Repetidoras (entrada)", FM, 12500 },
            { 435.000e6, 438.000e6, "Satélite", FM, 12500 },
            { 438.000e6, 439.000e6, "Todos os modos", FM, 12500 },
            { 439.000e6, 440.000e6, "Repetidoras (saída)", FM, 12500 },
        } },
        { "23 cm", 1240e6, 1300e6, 1296.200e6, USB, 2700, false, {} },
        { "160 m", 1.800e6, 2.000e6, 1.840e6, LSB, 2700, true, {} },
        { "80 m", 3.500e6, 4.000e6, 3.700e6, LSB, 2700, true, {} },
        { "40 m", 7.000e6, 7.300e6, 7.100e6, LSB, 2700, true, {} },
        { "30 m", 10.100e6, 10.150e6, 10.120e6, USB, 2700, true, {} },
        { "20 m", 14.000e6, 14.350e6, 14.200e6, USB, 2700, true, {} },
        { "17 m", 18.068e6, 18.168e6, 18.120e6, USB, 2700, true, {} },
        { "15 m", 21.000e6, 21.450e6, 21.200e6, USB, 2700, true, {} },
        { "12 m", 24.890e6, 24.990e6, 24.940e6, USB, 2700, true, {} },
        { "10 m", 28.000e6, 29.700e6, 28.500e6, USB, 2700, true, {} },
    };

    static double radioFreq() {
        if (!sigpath::vfoManager.vfoExists(RADIO_VFO)) { return 0; }
        return gui::waterfall.getCenterFrequency() + sigpath::vfoManager.getOffset(RADIO_VFO);
    }

    // Abaixo de 24 MHz o RTL-SDR v3 só recebe em amostragem direta (ramo Q).
    // Liga sozinho ao ir para HF/NDB e desliga ao voltar, se foi a aba que ligou.
    static bool dsByUs = false;
    static void autoDirectSampling(double f) {
        auto src = instanceOf("rtl_sdr_source");
        if (!src) { return; }
        bool hf = f < 24e6;
        if (hf) {
            json j = json::parse(src->handleDebugCommand("get_direct_sampling", ""), nullptr, false);
            if (!j.is_discarded() && j.value("mode", 0) == 0) {
                src->handleDebugCommand("set_direct_sampling", "2");
                dsByUs = true;
            }
        }
        else if (dsByUs) {
            src->handleDebugCommand("set_direct_sampling", "0");
            dsByUs = false;
        }
    }

    static void tuneTo(double f, int mode, float bw) {
        if (!radioExists()) { return; }
        autoDirectSampling(f);
        if (mode >= 0 && getMode() != mode) { core::modComManager.callInterface(RADIO_VFO, RADIO_IFACE_CMD_SET_MODE, &mode, NULL); }
        if (bw > 0) {
            setBandwidth(bw);
            if (mode >= 0) { storeBandwidth(cur, mode, bw); }
            watchedBw = bw;
        }
        tuner::tune(tuner::TUNER_MODE_NORMAL, RADIO_VFO, f);
    }

    static const Band* bandOf(double f) {
        for (auto& b : HAM_BANDS) {
            if (f >= b.lo && f <= b.hi) { return &b; }
        }
        return NULL;
    }

    static std::string mhzText(double f, int dec) {
        char b[32];
        snprintf(b, sizeof(b), "%.*f", dec, f / 1e6);
        std::string s = b;
        for (auto& c : s) { if (c == '.') { c = ','; } }
        return s;
    }

    // Régua da faixa: trechos coloridos, marcas e a posição do Rádio. Toque = sintoniza.
    static void drawRuler(const char* id, double lo, double hi, const std::vector<Seg>& segs, const std::vector<Mark>& marks, double step, int defMode, float defBw) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float w = flowRight - flowLeft;
        float h = chipH() * 1.55f;
        float lineH = ImGui::GetTextLineHeight();
        ImGui::SetCursorScreenPos(ImVec2(flowLeft, ImGui::GetCursorScreenPos().y));
        ImVec2 a = ImGui::GetCursorScreenPos();
        ImVec2 b(a.x + w, a.y + h);
        bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
        bool hovered = ImGui::IsItemHovered();
        // Escala por trechos: cada trecho ganha largura pela raiz do seu tamanho, assim
        // os estreitos (APRS, CW...) aparecem com nome. Toque e marcas seguem a mesma escala.
        std::vector<float> sx;   // borda esquerda de cada trecho, em px
        std::vector<float> sw;   // largura de cada trecho, em px
        if (!segs.empty()) {
            double tot = 0;
            for (auto& sg : segs) { tot += std::sqrt(sg.hi - sg.lo); }
            float x = a.x;
            for (auto& sg : segs) {
                float ww = (float)(std::sqrt(sg.hi - sg.lo) / tot) * w;
                sx.push_back(x);
                sw.push_back(ww);
                x += ww;
            }
        }
        auto xOf = [&](double f) {
            for (size_t i = 0; i < segs.size(); i++) {
                if (f >= segs[i].lo && (f < segs[i].hi || i + 1 == segs.size())) {
                    return sx[i] + (float)((f - segs[i].lo) / (segs[i].hi - segs[i].lo)) * sw[i];
                }
            }
            return a.x + (float)((f - lo) / (hi - lo)) * w;
        };
        auto fOf = [&](float x) {
            for (size_t i = 0; i < segs.size(); i++) {
                if (x < sx[i] + sw[i] || i + 1 == segs.size()) {
                    return segs[i].lo + (double)((x - sx[i]) / sw[i]) * (segs[i].hi - segs[i].lo);
                }
            }
            return lo + (double)((x - a.x) / w) * (hi - lo);
        };
        float r = 7.0f * style::uiScale;
        dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_FrameBg), r);
        static const ImU32 SEG_COLORS[] = { IM_COL32(29, 42, 64, 255), IM_COL32(23, 50, 80, 255), IM_COL32(40, 36, 64, 255), IM_COL32(28, 58, 92, 255) };
        int ci = 0;
        for (auto& s : segs) {
            float x0 = xOf(s.lo), x1 = xOf(s.hi);
            bool apr = std::string(s.name) == "APRS";
            ImU32 c = apr ? IM_COL32(90, 62, 22, 255) : SEG_COLORS[ci++ % 4];
            bool L = s.lo <= lo, R = s.hi >= hi;
            ImDrawFlags fl = (L && R) ? ImDrawFlags_RoundCornersAll : (L ? ImDrawFlags_RoundCornersLeft : (R ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone));
            dl->AddRectFilled(ImVec2(x0, a.y), ImVec2(x1, b.y), c, r, fl);
            if (x1 - x0 > 6) { dl->AddLine(ImVec2(x1, a.y), ImVec2(x1, b.y), ImGui::GetColorU32(ImGuiCol_WindowBg)); }
            // nome e limites, se couberem
            float pad = 6.0f * style::uiScale;
            ImVec2 ts = ImGui::CalcTextSize(s.name);
            if (x1 - x0 > ts.x + 2 * pad) {
                dl->PushClipRect(ImVec2(x0, a.y), ImVec2(x1, b.y), true);
                float ty = (h > 2.2f * lineH) ? a.y + h / 2 - lineH : a.y + (h - lineH) / 2;
                dl->AddText(ImVec2(x0 + pad, ty), apr ? IM_COL32(246, 196, 120, 255) : ImGui::GetColorU32(ImGuiCol_Text), s.name);
                std::string rng = mhzText(s.lo, 3) + " - " + mhzText(s.hi, 3);
                if (h > 2.2f * lineH && x1 - x0 > ImGui::CalcTextSize(rng.c_str()).x + 2 * pad) {
                    dl->AddText(ImVec2(x0 + pad, ty + lineH), ImGui::GetColorU32(ImGuiCol_TextDisabled), rng.c_str());
                }
                dl->PopClipRect();
            }
        }
        if (segs.empty()) {
            std::string rng = mhzText(lo, 3) + " - " + mhzText(hi, 3) + " MHz";
            ImVec2 ts = ImGui::CalcTextSize(rng.c_str());
            dl->AddText(ImVec2(a.x + (w - ts.x) / 2, a.y + (h - ts.y) / 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), rng.c_str());
        }
        for (auto& m : marks) {
            float x = xOf(m.f);
            dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), IM_COL32(240, 162, 74, 255), 2.0f * style::uiScale);
        }
        double f0 = radioFreq();
        if (f0 >= lo && f0 <= hi) {
            float x = xOf(f0);
            dl->AddLine(ImVec2(x, a.y - 3), ImVec2(x, b.y + 3), IM_COL32(255, 255, 255, 255), 2.5f * style::uiScale);
        }
        // posição sob o mouse: dica e toque
        if (hovered) {
            float mx = ImGui::GetIO().MousePos.x;
            double f = fOf(mx);
            f = std::round(f / step) * step;
            f = (std::max)(lo, (std::min)(hi, f));
            const Seg* seg = NULL;
            for (auto& s : segs) {
                if (f >= s.lo && f < s.hi) { seg = &s; }
            }
            dl->AddLine(ImVec2(mx, a.y), ImVec2(mx, b.y), IM_COL32(255, 255, 255, 90));
            ImGui::SetTooltip("%s MHz%s%s", mhzText(f, 4).c_str(), seg ? " · " : "", seg ? seg->name : "");
            if (clicked) { tuneTo(f, seg ? seg->mode : defMode, seg ? seg->bw : defBw); }
        }
        // limites e marcas embaixo
        ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + 2));
        ImVec2 la = ImGui::GetCursorScreenPos();
        ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        std::string sl = mhzText(lo, lo < 30e6 ? 3 : 0), sh = mhzText(hi, hi < 30e6 ? 3 : 0);
        dl->AddText(la, dim, sl.c_str());
        dl->AddText(ImVec2(a.x + w - ImGui::CalcTextSize(sh.c_str()).x, la.y), dim, sh.c_str());
        float lastEnd = la.x + ImGui::CalcTextSize(sl.c_str()).x + 12;
        for (auto& m : marks) {
            float x = xOf(m.f) - 2;
            float tw = ImGui::CalcTextSize(m.name).x;
            if (x < lastEnd || x + tw > a.x + w - ImGui::CalcTextSize(sh.c_str()).x - 8) { continue; }
            dl->AddText(ImVec2(x, la.y), IM_COL32(240, 162, 74, 255), m.name);
            lastEnd = x + tw + 12;
        }
        ImGui::Dummy(ImVec2(w, lineH));
    }

    // Botão de atalho (sintoniza direto); fica aceso se o Rádio estiver ali
    static void shortcut(const char* id, const std::string& label, double f, int mode, float bw, double tol, bool first) {
        if (!first) { flowNext(chipWidth(label), 4.0f * style::uiScale); }
        else { ImGui::SetCursorScreenPos(ImVec2(flowLeft, ImGui::GetCursorScreenPos().y)); }
        bool on = std::fabs(radioFreq() - f) < tol;
        if (chip(id, label, on)) { tuneTo(f, mode, bw); }
    }

    static bool inAviation(double f) {
        return (f >= 108e6 && f <= 137e6) || (f >= 225e6 && f <= 400e6) || (f >= 190e3 && f <= 1750e3);
    }

    static void drawAviationBand() {
        double f = radioFreq();
        drawRuler("##air_ruler", 108e6, 137e6, AIR_PLAN, AIR_MARKS, 25000, AM, 8333);
        shortcut("##air_nav", "VOR / ILS", 112.000e6, AM, 12500, 4e6, true);
        shortcut("##air_twr", "Torre e aproximação", 118.100e6, AM, 8333, 1e3, false);
        shortcut("##air_121", "121,500 emergência", 121.500e6, AM, 8333, 1e3, false);
        shortcut("##air_acars", "ACARS 131,550", 131.550e6, AM, 12500, 1e3, false);
        shortcut("##air_mil", "Militar 225-400", 243.000e6, AM, 12500, 1e3, false);
        if (f > 0 && !inAviation(f)) {
            std::string l = "Fora da faixa de aviação · voltar";
            flowNext(chipWidth(l), 12.0f * style::uiScale);
            if (chip("##air_back", l, true, 0, IM_COL32(245, 176, 65, 255))) { tuneTo(118.100e6, AM, 8333); }
        }
    }

    static const Band* hamBandShown = NULL;
    static void drawHamBand() {
        double f = radioFreq();
        const Band* cb = bandOf(f);
        if (cb) { hamBandShown = cb; }
        if (!hamBandShown) { hamBandShown = &HAM_BANDS[1]; }
        float sp = 4.0f * style::uiScale;
        // botões das faixas: VHF/UHF e HF
        ImGui::SetCursorScreenPos(ImVec2(flowLeft, ImGui::GetCursorScreenPos().y));
        groupLabel("VHF / UHF");
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) {
                flowNext(chipWidth("HF") + 4, 14.0f * style::uiScale);
                groupLabel("HF");
            }
            for (auto& b : HAM_BANDS) {
                if (b.hf != (pass == 1)) { continue; }
                flowNext(chipWidth(b.label), sp);
                std::string id = std::string("##hb_") + b.label;
                if (chip(id.c_str(), b.label, cb == &b)) { tuneTo(b.home, b.mode, b.bw); hamBandShown = &b; }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s - %s MHz%s", mhzText(b.lo, 3).c_str(), mhzText(b.hi, 3).c_str(),
                                      b.hf ? "\nLiga sozinho o modo HF do RTL-SDR v3 (amostragem direta)" : "");
                }
            }
        }
        const Band& b = *hamBandShown;
        double step = b.hf ? 500 : (b.lo > 400e6 ? 12500 : 5000);
        drawRuler("##ham_ruler", b.lo, b.hi, b.plan, {}, step, b.mode, b.bw);
    }

    // Modos da aba Aviação: o que a aviação usa (não os modos do rádio em si)
    static void drawAviationModes(int mode, float gap, float sp, bool compact) {
        double f = radioFreq();
        bool acars = status.acarsRunning;
        bool nav = mode == AM && f >= 108e6 && f < 118e6;
        bool ndb = mode == AM && f > 0 && f < 2e6;
        bool voice = mode == AM && !nav && !ndb && !acars;
        if (!compact) {
            flowNext(chipWidth("MODO") + 4, gap);
            groupLabel("MODO");
            flowNext(chipWidth("AM voz"), sp);
        }
        if (chip("##am_voz", "AM voz", voice)) {
            if (acars) { if (auto ac = instanceOf("acars_decoder")) { ac->handleDebugCommand("stop", ""); } }
            if (f >= 118e6 && f <= 137e6) { selectMode(AM); }
            else { tuneTo(118.100e6, AM, 8333); }
        }
        flowNext(chipWidth("ACARS · CPDLC"), sp);
        if (chip("##acars", "ACARS · CPDLC", acars)) {
            if (auto ac = instanceOf("acars_decoder")) { ac->handleDebugCommand(acars ? "stop" : "start", ""); }
        }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Mensagens de dados das aeronaves (ACARS). CPDLC e ADS-C chegam dentro dele.\nAs mensagens aparecem no painel ACARS · CPDLC."); }
        flowNext(chipWidth("VOR · ILS"), sp);
        if (chip("##vor", "VOR · ILS", nav)) { tuneTo(f >= 108e6 && f < 118e6 ? f : 112.000e6, AM, 12500); }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Radiofaróis VOR e localizador do ILS: 108 a 117,975 MHz, em AM.\nO indicativo da estação é transmitido em Morse (tom de 1020 Hz)."); }
        flowNext(chipWidth("NDB · ADF"), sp);
        if (chip("##ndb", "NDB · ADF", ndb)) { tuneTo(f > 190e3 && f < 1750e3 ? f : 300e3, AM, 3000); }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Radiofaróis NDB (o ADF do avião): 190 a 1750 kHz, em AM com indicativo em Morse.\nLiga sozinho o modo HF do RTL-SDR v3 (amostragem direta)."); }
    }

    static void drawModeAndBandwidth(bool compact) {
        refreshModes();
        int mode = getMode();
        float bw = getBandwidth();
        trackBandwidth(mode, bw);
        float sp = 4.0f * style::uiScale;
        float gap = (compact ? 8.0f : 18.0f) * style::uiScale;

        // ---- modos
        std::vector<int> ids;
        if (cur == AVIATION) { ids = AVIATION_MODES; }
        else if (cur == HAM) { ids = HAM_MODES; }
        else {
            for (auto& m : radioModes) { ids.push_back(m.id); }
        }
        bool first = true;
        if (cur == AVIATION) { drawAviationModes(mode, gap, sp, compact); ids.clear(); }
        else if (!compact) { flowNext(chipWidth("MODO") + 4, gap); groupLabel("MODO"); first = false; }
        for (int id : ids) {
            const ModeInfo* m = findMode(id);
            if (!m) { continue; }
            std::string label = modeLabel(*m);
            if (cur == HAM && id == RADIO_IFACE_MODE_NFM) { label = "FM"; }
            if (cur == HAM && id == MODE_DSD) { label = "DMR · P25"; }
            if (!first) { flowNext(chipWidth(label), sp); }
            first = false;
            std::string cid = "##mode_" + std::to_string(id);
            if (chip(cid.c_str(), label, mode == id)) { selectMode(id); }
        }
        bool modeInTab = std::find(ids.begin(), ids.end(), mode) != ids.end() || (cur == AVIATION && mode == AM);
        if (!modeInTab && mode >= 0) {
            // Modo atual não é desta aba (ex.: escolhido em outro painel): mostra mesmo assim
            const ModeInfo* m = findMode(mode);
            std::string label = m ? modeLabel(*m) : std::string("?");
            flowNext(chipWidth(label), sp);
            chip("##mode_cur", label, true);
        }

        // ---- largura de banda
        std::vector<float> presets = presetsFor(cur, mode);
        float lo, hi;
        static double limitsAt = -100;
        static float cLo = 100, cHi = 250000;
        if (nowSec() - limitsAt > 1.0) { bandwidthLimits(cLo, cHi); limitsAt = nowSec(); }
        lo = cLo;
        hi = cHi;

        flowNext(chipWidth("LARGURA") + 4, gap);
        groupLabel("LARGURA");
        for (int i = 0; i < (int)presets.size(); i++) {
            float p = presets[i];
            if (p < lo - 0.5f || p > hi + 0.5f) { continue; }
            std::string label = bwText(p);
            flowNext(chipWidth(label), sp);
            std::string cid = "##bw_" + std::to_string(i);
            if (chip(cid.c_str(), label, std::fabs(bw - p) < 1.0f)) {
                setBandwidth(p);
                storeBandwidth(cur, mode, p);
                watchedBw = p;
            }
        }
        // passo - / + (DMR: de 5 em 5 kHz, até 50 kHz)
        {
            float st = stepFor(mode);
            std::string lbl = "-" + bwText(st), lbl2 = "+" + bwText(st);
            flowNext(chipWidth(lbl) + chipWidth(lbl2) + sp, sp);
            auto apply = [&](float v) {
                v = std::round(v / st) * st;
                v = (std::max)(lo, (std::min)(hi, v));
                setBandwidth(v);
                storeBandwidth(cur, mode, v);
                watchedBw = v;
            };
            if (chip("##bw_minus", lbl, false)) { apply(bw - st); }
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Diminui %s", bwText(st).c_str()); }
            ImGui::SameLine(0, sp);
            if (chip("##bw_plus", lbl2, false)) { apply(bw + st); }
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Aumenta %s (máximo %s)", bwText(st).c_str(), bwText(hi).c_str()); }
        }

        // ajuste fino em kHz
        float w = 92.0f * style::uiScale;
        flowNext(w, sp);
        float khz = bw / 1000.0f;
        ImGui::SetNextItemWidth(w);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, (chipH() - ImGui::GetFontSize()) / 2));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, chipH() / 2);
        float step = (hi - lo) > 50000 ? 1.0f : 0.05f;
        if (ImGui::DragFloat("##bw_fine", &khz, step, lo / 1000.0f, hi / 1000.0f, khz < 10 ? "%.2f kHz" : "%.1f kHz", ImGuiSliderFlags_AlwaysClamp)) {
            setBandwidth(khz * 1000.0f);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            storeBandwidth(cur, mode, khz * 1000.0f);
            watchedBw = khz * 1000.0f;
        }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Arraste ou dê duplo clique para digitar.\nA largura fica salva para este modo nesta aba."); }
        ImGui::PopStyleVar(2);

        // ---- centralizar no sinal (erro de ppm do RTL-SDR)
        flowNext(chipWidth("CENTRALIZAR"), sp);
        if (chip("##center_sig", "CENTRALIZAR", false)) { centerOnSignal(bw); }
        if (ImGui::IsItemHovered()) {
            if (nowSec() - centerMsgAt < 6 && !centerMsg.empty()) { ImGui::SetTooltip("%s", centerMsg.c_str()); }
            else { ImGui::SetTooltip("Sintoniza o Rádio no centro do sinal mais forte por perto.\nÚtil quando o RTL-SDR tem erro de ppm e o sinal aparece ao lado."); }
        }

        // ---- squelch
        if (radioExists()) {
            bool sq = false;
            float lvl = -50;
            core::modComManager.callInterface(RADIO_VFO, RADIO_IFACE_CMD_GET_SQUELCH_ENABLED, NULL, &sq);
            core::modComManager.callInterface(RADIO_VFO, RADIO_IFACE_CMD_GET_SQUELCH_LEVEL, NULL, &lvl);
            float sw = 96.0f * style::uiScale;
            flowNext(chipWidth("SQUELCH") + sp + sw, gap);
            if (chip("##sql_on", "SQUELCH", sq, 0, IM_COL32(245, 176, 65, 255))) {
                bool v = !sq;
                core::modComManager.callInterface(RADIO_VFO, RADIO_IFACE_CMD_SET_SQUELCH_ENABLED, &v, NULL);
            }
            ImGui::SameLine(0, sp);
            ImGui::SetNextItemWidth(sw);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, (chipH() - ImGui::GetFontSize()) / 2));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, chipH() / 2);
            ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, chipH() / 2);
            if (!sq) { ImGui::BeginDisabled(); }
            if (ImGui::SliderFloat("##sql_lvl", &lvl, -100.0f, 0.0f, "%.0f dB")) {
                core::modComManager.callInterface(RADIO_VFO, RADIO_IFACE_CMD_SET_SQUELCH_LEVEL, &lvl, NULL);
            }
            if (!sq) { ImGui::EndDisabled(); }
            ImGui::PopStyleVar(3);
        }
    }

    void drawControlBar(bool compact) {
        if (!initialized) { init(); }
        ImGuiStyle& st = ImGui::GetStyle();
        ImVec2 oldSpacing = st.ItemSpacing;
        st.ItemSpacing = ImVec2(4.0f * style::uiScale, 6.0f * style::uiScale);
        float padX = 10.0f * style::uiScale;
        float padY = 7.0f * style::uiScale;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImDrawListSplitter sp;
        sp.Split(dl, 2);
        sp.SetCurrentChannel(dl, 1);

        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float fullW = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorScreenPos(ImVec2(p0.x + padX, p0.y + padY));
        ImGui::BeginGroup();
        ImGui::PushClipRect(p0, ImVec2(p0.x + fullW, p0.y + 10000), true);

        // limita a área útil para a quebra automática
        float innerW = fullW - 2 * padX;
        flowLeft = p0.x + padX;
        flowRight = p0.x + fullW - padX;

        if (!compact) {
            float tabsW = (std::min)(innerW, 300.0f * style::uiScale);
            drawSegmentTabs(tabsW, chipH() + 6.0f * style::uiScale);
        }
        drawModeAndBandwidth(compact);
        // régua e atalhos da faixa (só nas abas de uso)
        if (cur == AVIATION) { drawAviationBand(); }
        else if (cur == HAM) { drawHamBand(); }

        ImGui::PopClipRect();
        ImGui::EndGroup();
        ImVec2 g1 = ImGui::GetItemRectMax();

        ImVec2 p1(p0.x + fullW, g1.y + padY);
        sp.SetCurrentChannel(dl, 0);
        ImU32 bg = ImGui::GetColorU32(ImGuiCol_ChildBg, 1.0f);
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_WindowBg));
        dl->AddRectFilledMultiColor(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), ImGui::GetColorU32(ImGuiCol_FrameBg),
                                    ImGui::GetColorU32(ImGuiCol_WindowBg), ImGui::GetColorU32(ImGuiCol_WindowBg));
        dl->AddLine(ImVec2(p0.x, p1.y - 1), ImVec2(p1.x, p1.y - 1), ImGui::GetColorU32(ImGuiCol_Border));
        (void)bg;
        sp.Merge(dl);

        ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
        ImGui::Dummy(ImVec2(fullW, 0));
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
        st.ItemSpacing = oldSpacing;
    }

    float statusBarHeight() {
        return ImGui::GetTextLineHeight() + 10.0f * style::uiScale;
    }

    void drawStatusBar() {
        int mode = getMode();
        refreshStatus(mode);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        float h = statusBarHeight();
        ImVec2 p1(p0.x + w, p0.y + h);
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg));
        dl->AddLine(p0, ImVec2(p1.x, p0.y), ImGui::GetColorU32(ImGuiCol_Border));

        float x = p0.x + 10.0f * style::uiScale;
        float ty = p0.y + (h - ImGui::GetTextLineHeight()) / 2;
        float dotR = 3.5f * style::uiScale;
        ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        ImU32 txt = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 GREEN = IM_COL32(70, 210, 120, 255), AMBER = IM_COL32(245, 176, 65, 255), BLUE = IM_COL32(76, 194, 255, 255), GRAY = IM_COL32(90, 104, 120, 255);
        auto item = [&](ImU32 dot, const std::string& label, const std::string& value) {
            dl->AddCircleFilled(ImVec2(x + dotR, p0.y + h / 2), dotR, dot);
            x += dotR * 2 + 6.0f * style::uiScale;
            dl->AddText(ImVec2(x, ty), dim, label.c_str());
            x += ImGui::CalcTextSize(label.c_str()).x + 5.0f * style::uiScale;
            dl->AddText(ImVec2(x, ty), txt, value.c_str());
            x += ImGui::CalcTextSize(value.c_str()).x + 22.0f * style::uiScale;
        };

        // sintonia atual
        double freq = gui::waterfall.getCenterFrequency() + (sigpath::vfoManager.vfoExists(RADIO_VFO) ? sigpath::vfoManager.getOffset(RADIO_VFO) : 0);
        char fbuf[64];
        snprintf(fbuf, sizeof(fbuf), "%.4f MHz", freq / 1e6);
        const ModeInfo* m = findMode(mode);
        std::string ms = m ? modeLabel(*m) : std::string("-");
        item(BLUE, LABELS[cur], std::string(fbuf) + "  " + ms + "  " + bwText(getBandwidth()));
        if (mode == RADIO_IFACE_MODE_NFM && status.tone > 0) {
            char tb[32];
            snprintf(tb, sizeof(tb), "CTCSS %.1f Hz", status.tone);
            item(AMBER, "Tom", tb);
        }

        if (nowSec() - centerMsgAt < 6 && !centerMsg.empty()) { item(AMBER, "Centralizar", centerMsg); }
        if (status.scanning) {
            std::string v = status.receiving ? "recebendo" : (status.holding ? "segurando" : "procurando");
            if (!status.station.empty() && (status.receiving || status.holding)) { v += " · " + status.station; }
            item(status.receiving ? AMBER : (status.holding ? BLUE : GREEN), "Scanner", v);
        }
        else {
            item(GRAY, "Scanner", "parado");
        }
        if (mode == MODE_DSD) {
            item(status.dmrSync ? GREEN : GRAY, status.digLabel, status.dmrSync ? status.dmrInfo : (status.dmrLast.empty() ? std::string("sem sinal") : "sem sinal · última " + status.dmrLast));
        }
        if (cur != HAM) {
            if (instanceOf("adsb_decoder")) {
                item(status.adsbRunning ? GREEN : GRAY, "ADS-B", status.adsbRunning ? std::to_string(status.aircraft) + " aeronaves" : std::string("parado"));
            }
            if (instanceOf("acars_decoder")) {
                std::string v = status.acarsRunning ? std::to_string(status.acarsMsgs) + " msgs" : std::string("parado");
                if (status.acarsRunning && !status.acarsLast.empty()) { v += " · " + status.acarsLast; }
                item(status.acarsRunning ? GREEN : GRAY, "ACARS", v);
            }
        }
        ImGui::Dummy(ImVec2(w, h));
    }

    void drawTabs() {
        if (!initialized) { init(); }
        float w = ImGui::GetContentRegionAvail().x;
        drawSegmentTabs(w, ImGui::GetFrameHeight() * 1.45f);
        ImGui::Dummy(ImVec2(w, 6.0f * style::uiScale));
    }
}
