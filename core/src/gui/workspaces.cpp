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

    std::string displayName(const std::string& name) {
        static const std::map<std::string, std::string> N = {
            { "Source", "Fonte (receptor)" }, { "Radio", "Rádio" }, { "Frequency Manager", "Canais e scanner" },
            { "Recorder", "Gravador" }, { "Sinks", "Saída de áudio" }, { "Noise Reduction logmmse", "Redução de ruído" },
            { "ADS-B", "ADS-B · radar" }, { "ACARS", "ACARS · CPDLC" }, { "Band Plan", "Plano de bandas" },
            { "Display", "Tela" }, { "Theme", "Tema" }, { "VFO Color", "Cores dos VFOs" },
            { "Module Manager", "Módulos" }, { "Scanner", "Varredura de faixa" }, { "FT8/FT4 Decoder", "FT8 / FT4" },
            { "VHF Digital Modes", "Modos digitais (DSD)" },
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
        case MODE_DSD: return { 12500 };
        default: return {};
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
        std::string dmrInfo;
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
        if (mode == MODE_DSD) {
            for (auto& [name, inst] : core::moduleManager.instances) {
                if (!inst.module.info || !inst.instance || std::string(inst.module.info->name) != "ch_extravhf_decoder") { continue; }
                json j = json::parse(inst.instance->handleDebugCommand("get_dmr_status", RADIO_VFO), nullptr, false);
                if (j.is_discarded() || j.contains("error")) { continue; }
                bool sync = j.contains("sync") && j["sync"].is_boolean() && j["sync"].get<bool>();
                bool voice = j.contains("voice") && j["voice"].is_boolean() && j["voice"].get<bool>();
                int cc = (j.contains("color_code") && j["color_code"].is_number()) ? j["color_code"].get<int>() : -1;
                s.dmrSync = sync;
                s.dmrInfo = std::string(voice ? "voz" : "sincronizado") + (cc >= 0 ? " · CC " + std::to_string(cc) : "");
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
        if (!compact) { flowNext(chipWidth("MODO") + 4, gap); groupLabel("MODO"); first = false; }
        for (int id : ids) {
            const ModeInfo* m = findMode(id);
            if (!m) { continue; }
            std::string label = modeLabel(*m);
            if (!first) { flowNext(chipWidth(label), sp); }
            first = false;
            std::string cid = "##mode_" + std::to_string(id);
            if (chip(cid.c_str(), label, mode == id)) { selectMode(id); }
        }
        bool modeInTab = std::find(ids.begin(), ids.end(), mode) != ids.end();
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

        if (status.scanning) {
            std::string v = status.receiving ? "recebendo" : (status.holding ? "segurando" : "procurando");
            if (!status.station.empty() && (status.receiving || status.holding)) { v += " · " + status.station; }
            item(status.receiving ? AMBER : (status.holding ? BLUE : GREEN), "Scanner", v);
        }
        else {
            item(GRAY, "Scanner", "parado");
        }
        if (mode == MODE_DSD) {
            item(status.dmrSync ? GREEN : GRAY, "DMR", status.dmrSync ? status.dmrInfo : std::string("sem sinal"));
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
