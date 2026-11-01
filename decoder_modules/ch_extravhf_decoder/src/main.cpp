


#if defined(__MACH__) || defined(__ANDROID__)
#define register
#endif



#include <itpp/itcomm.h>
#ifndef ITCOMM_H
// KEEP THIS CRAP TOGETHER DONT REMOVE INCLUDE.
#error "ITCOMM_H is not defined"
#endif


#include <imgui.h>
#include "../../src/gui/style.h"
#include "../../../core/src/config.h"


#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include "../../radio/src/radio_module_interface.h"
#include <core.h>
#include <utils/optionlist.h>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <map>
#include <gui/tuner.h>
#include <gui/main_window.h>
#include <utils/flog.h>
#include "./demod.h"
#include "demodulators/dsd.h"

ConfigManager config;

#define CONCAT(a, b) ((std::string(a) + b).c_str())


class VhfVoiceRadioModule : public ModuleManager::Instance {
public:

    std::string name;

    VhfVoiceRadioModule(std::string name) {
        this->name = name;
        config.acquire();
        followTrunk = config.conf.value("followTrunk", false);
        skipEncrypted = config.conf.value("skipEncrypted", true);
        tgFilter = config.conf.value("tgFilter", std::string());
        cqpsk = config.conf.value("cqpsk", false);
        demod::DSD::setCqpskAll(cqpsk);
        if (config.conf.contains("names") && config.conf["names"].is_object()) {
            for (auto& [k, v] : config.conf["names"].items()) {
                if (v.is_string()) { names[k] = v.get<std::string>(); }
            }
        }
        config.release();
        gui::menu.registerEntry(name, menuHandler, this, NULL);
        frameHandler.handler = onFrame;
        frameHandler.ctx = this;
        gui::mainWindow.onWaterfallDrawn.bindHandler(&frameHandler);
    }

    ~VhfVoiceRadioModule() {
        gui::mainWindow.onWaterfallDrawn.unbindHandler(&frameHandler);
        gui::menu.removeEntry(name);
    }

    // P25 troncalizado
    bool followTrunk = false;
    bool skipEncrypted = true;
    std::string tgFilter;
    bool onVoice = false;
    double ccFreq = 0, voiceFreq = 0, voiceStart = 0, lastActivity = 0, leftAt = -100;
    uint32_t followTg = 0, followSrc = 0, lastTg = 0;
    EventHandler<ImGuiContext*> frameHandler;

    // Registro de chamadas digitais (DMR / P25)
    struct Call {
        std::string mode;          // "DMR" ou "P25"
        double start = 0, last = 0;
        time_t wall = 0;
        uint32_t tg = 0, src = 0;
        int slot = -1;
        bool group = true;
        bool encrypted = false;
        int algid = 0x80;
        int keyid = 0;
        bool emergency = false;
        int code = -1;             // color code (DMR) ou NAC (P25)
        double freq = 0;
    };
    bool cqpsk = false;
    bool callActive = false;
    Call cur;
    std::deque<Call> calls;
    uint32_t lastLcSeen = 0;
    std::map<std::string, std::string> names;   // "TG:2000" / "ID:64250" -> nome
    char nameNum[16] = "";
    char nameText[48] = "";
    int nameKind = 0;   // 0 = TG, 1 = ID

    EventHandler<std::string> moduleCreatedListener;
    EventHandler<std::string> moduleDeleteListener;

    struct Injection {
        RadioModuleInterface *radio;
        VhfVoiceRadioModule *thiz;
        EventHandler<ImGuiContext *> drawModeButtonsHandler;
        RadioModuleInterface::demodProviderFunction demodFunction;
    };

    static const int RADIO_DEMOD_DSD = 0x1301;
    static const int RADIO_DEMOD_OLDDSD = 0x1302;

    bool injected = false;

    std::vector<std::shared_ptr<Injection>> injections;

    std::shared_ptr<Injection> getInjection(RadioModuleInterface *radio) {
        for(auto& x: injections) {
            if (x->radio == radio) {
                return x;
            }
        }
        auto rv =std::make_shared<Injection>();
        injections.emplace_back(rv);
        rv->radio = radio;
        rv->thiz = this;
        rv->demodFunction = [](int id) -> demod::Demodulator* {
            switch (id) {
                case RADIO_DEMOD_DSD:  return new demod::DSD(); break;
                case RADIO_DEMOD_OLDDSD:  return new demod::OldDSD(); break;
                default:                        return NULL; break;
            }
        };
        return rv;
    }

    void injectIntoRadio(RadioModuleInterface *radio) {
        auto inj = getInjection(radio);
        inj->drawModeButtonsHandler.ctx = inj.get();
        //inj->drawModeButtonsHandler.handler = onDrawModeButtons;
        //radio->onDrawModeButtons.bindHandler(&inj->drawModeButtonsHandler);
        radio->demodulatorProviders.emplace_back(inj->demodFunction);
        radio->radioModes.emplace_back();
        radio->radioModes.back().first = "DSD";
        radio->radioModes.back().second = RADIO_DEMOD_DSD;
        radio->radioModes.emplace_back();
        radio->radioModes.back().first = "oldDSD";
        radio->radioModes.back().second = RADIO_DEMOD_OLDDSD;

    }

    static void onModuleCreated(std::string modName, void *ctx) {
        auto _this = (VhfVoiceRadioModule *)ctx;
        auto radio = (RadioModuleInterface *)core::moduleManager.getInterface(modName, "RadioModuleInterface");
        if (radio) {
            _this->injectIntoRadio(radio);
        }

    }

    static void onModuleDelete(std::string modName, void *ctx) {
        auto _this = (VhfVoiceRadioModule *)ctx;
        auto radio = (RadioModuleInterface *)core::moduleManager.getInterface(modName, "RadioModuleInterface");
        if (radio) {
            _this->uninjectFromRadio(radio);
        }
    }

    void inject() {
        if (!injected) {
            injected = true;
            moduleCreatedListener.handler = onModuleCreated;
            moduleCreatedListener.ctx = this;
            moduleDeleteListener.handler = onModuleDelete;
            moduleDeleteListener.ctx = this;
            core::moduleManager.onInstanceCreated.bindHandler(&moduleCreatedListener);
            core::moduleManager.onInstanceDelete.bindHandler(&moduleDeleteListener);
            int ix = 0;
            auto radios = core::moduleManager.getAllInterfaces<RadioModuleInterface>("RadioModuleInterface");
            for (auto &r: radios) {
                injectIntoRadio(r);
                ix++;
            }
        }
    }

    void uninjectFromRadio(RadioModuleInterface *radio) {
        auto inj = getInjection(radio);
        //radio->onDrawModeButtons.unbindHandler(&inj->drawModeButtonsHandler);
        auto wh = std::find(radio->demodulatorProviders.begin(), radio->demodulatorProviders.end(), inj->demodFunction);
        if (wh != radio->demodulatorProviders.end()) {
            radio->demodulatorProviders.erase(wh);
        }
        for(int i=0; i<radio->radioModes.size(); i++) {
            if (radio->radioModes[i].second == RADIO_DEMOD_DSD || radio->radioModes[i].second == RADIO_DEMOD_OLDDSD) {
                radio->radioModes.erase(radio->radioModes.begin() + i);
                i--;
            }
        }
    }

    void uninject() {
        if (injected) {
            core::moduleManager.onInstanceCreated.unbindHandler(&moduleCreatedListener);
            core::moduleManager.onInstanceDelete.unbindHandler(&moduleDeleteListener);
            for (auto x: core::moduleManager.instances) {
                Instance *pInstance = x.second.instance;
                auto radio = (RadioModuleInterface *) pInstance->getInterface("RadioModuleInterface");
                if (radio) {
                    uninjectFromRadio(radio);
                }
            }
            injected = false;
        }
    }

    void postInit() {
        inject();
    }

    void enable() {
        enabled = true;
        inject();
    }

    void disable() {
        enabled = false;
        uninject();
    }

    bool isEnabled() {
        return enabled;
    }

    std::string handleDebugCommand(const std::string& cmd, const std::string& args) override {
        if (cmd == "get_dmr_status") {
            std::string radioName = args.empty() ? "Radio" : args;
            demod::DSD::DebugStatus st;
            if (!demod::DSD::getStatusForRadio(radioName, st)) {
                return std::string("{\"error\": \"no active DSD demod for radio '") + radioName + "'\"}";
            }
            return debugStatusToJson(st, 0, false);
        }
        if (cmd == "get_p25_system") {
            std::string radioName = args.empty() ? "Radio" : args;
            std::string out;
            if (!demod::DSD::getP25SystemForRadio(radioName, out)) {
                return std::string("{\"error\": \"no active DSD demod for radio '") + radioName + "'\"}";
            }
            return out;
        }
        if (cmd == "wait_dmr_sync_voice") {
            std::string radioName = "Radio";
            int stableMs = 2000;
            int timeoutMs = 10000;
            parseWaitArgs(args, radioName, stableMs, timeoutMs);
            demod::DSD::DebugStatus st;
            int waitedMs = 0;
            bool ok = demod::DSD::waitForDMRSyncVoice(radioName, stableMs, timeoutMs, st, waitedMs);
            return debugStatusToJson(st, waitedMs, ok);
        }
        return "{}";
    }


private:
    static void parseWaitArgs(const std::string& args, std::string& radioName, int& stableMs, int& timeoutMs) {
        if (args.empty()) {
            return;
        }
        std::vector<std::string> parts;
        size_t start = 0;
        while (true) {
            size_t pos = args.find(',', start);
            if (pos == std::string::npos) {
                parts.push_back(args.substr(start));
                break;
            }
            parts.push_back(args.substr(start, pos - start));
            start = pos + 1;
        }
        if (parts.size() > 0 && !parts[0].empty()) { radioName = parts[0]; }
        if (parts.size() > 1 && !parts[1].empty()) {
            try { stableMs = std::stoi(parts[1]); } catch (...) {}
        }
        if (parts.size() > 2 && !parts[2].empty()) {
            try { timeoutMs = std::stoi(parts[2]); } catch (...) {}
        }
    }

    static std::string escapeJson(const std::string& in) {
        std::string out;
        out.reserve(in.size() + 8);
        for (char c : in) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out += c; break;
            }
        }
        return out;
    }

    static std::string debugStatusToJson(const demod::DSD::DebugStatus& st, int waitedMs, bool ok) {
        return std::string("{\"status\":\"") + (ok ? "ok" : "timeout") +
               "\",\"available\":" + (st.available ? "true" : "false") +
               ",\"sync\":" + (st.sync ? "true" : "false") +
               ",\"dmr\":" + (st.dmr ? "true" : "false") +
               ",\"voice\":" + (st.voice ? "true" : "false") +
               ",\"mbe_decoding\":" + (st.mbe_decoding ? "true" : "false") +
               ",\"waited_ms\":" + std::to_string(waitedMs) +
               ",\"color_code\":" + std::to_string(st.color_code) +
               ",\"slot0_burst\":" + std::to_string(st.slot0_burst) +
               ",\"slot1_burst\":" + std::to_string(st.slot1_burst) +
               ",\"slot0_type\":\"" + escapeJson(st.slot0_type) +
               "\",\"slot1_type\":\"" + escapeJson(st.slot1_type) +
               "\",\"mbe_errorbar\":\"" + escapeJson(st.mbe_errorbar) +
               "\",\"offset_hz\":" + std::to_string((int)st.offset_hz) +
               ",\"src\":" + std::to_string(st.src) + ",\"dst\":" + std::to_string(st.dst) +
               ",\"group\":" + (st.group ? "true" : "false") + ",\"emergency\":" + (st.emergency ? "true" : "false") +
               ",\"privacy\":" + (st.privacy ? "true" : "false") + ",\"lc_slot\":" + std::to_string(st.lc_slot) +
               ",\"lc_count\":" + std::to_string(st.lc_count) +
               ",\"p25\":" + (st.p25 ? "true" : "false") + ",\"p25_nac\":" + std::to_string(st.p25_nac) +
               ",\"p25_tg\":" + std::to_string(st.p25_tg) + ",\"p25_src\":" + std::to_string(st.p25_src) +
               ",\"p25_algid\":" + std::to_string(st.p25_algid) + ",\"p25_emergency\":" + (st.p25_emergency ? "true" : "false") +
               ",\"p25_type\":\"" + escapeJson(st.p25_type) + "\"}";
    }

    // ------------------------------------------------------------------ P25 troncalizado
    // Segue as conversas: no canal de controle, quando sai uma concessão de voz
    // (talkgroup -> frequência), o Rádio vai até ela; quando a conversa acaba,
    // volta para o canal de controle.
    static double nowSec() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    bool tgAllowed(uint32_t tg) {
        if (tgFilter.empty()) { return true; }
        std::string f = tgFilter + ",";
        size_t a = 0;
        while (a < f.size()) {
            size_t b = f.find(',', a);
            std::string t = f.substr(a, b - a);
            t.erase(0, t.find_first_not_of(' '));
            if (!t.empty() && t.find_first_not_of("0123456789 ") == std::string::npos && (uint32_t)std::stoul(t) == tg) { return true; }
            a = b + 1;
        }
        return false;
    }

    double radioFreq() {
        if (!sigpath::vfoManager.vfoExists("Radio")) { return 0; }
        return gui::waterfall.getCenterFrequency() + sigpath::vfoManager.getOffset("Radio");
    }

    static void onFrame(ImGuiContext* ctx, void* p) {
        ((VhfVoiceRadioModule*)p)->followStep();
        ((VhfVoiceRadioModule*)p)->trackCalls();
    }

    // ------------------------------------------------------------------ registro de chamadas
    static std::string algName(int alg) {
        switch (alg) {
        case 0x80: return "aberta";
        case 0x81: return "DES-OFB";
        case 0x83: return "3DES";
        case 0x84: return "AES-256";
        case 0x85: return "AES-128";
        case 0x9F: return "DES-XL";
        case 0xA0: return "DVI-XL";
        case 0xA1: return "DVP-XL";
        case 0xAA: return "ADP (RC4)";
        default: {
            char b[24];
            snprintf(b, sizeof(b), "algoritmo 0x%02X", alg);
            return b;
        }
        }
    }

    std::string nameOf(const char* kind, uint32_t v) {
        auto it = names.find(std::string(kind) + ":" + std::to_string(v));
        return it == names.end() ? "" : it->second;
    }

    std::string label(const char* kind, uint32_t v) {
        if (v == 0) { return "?"; }
        std::string n = nameOf(kind, v);
        return std::to_string(v) + (n.empty() ? "" : " (" + n + ")");
    }

    static std::string frameText(const demod::DSD::DebugStatus& st) {
        if (st.p25) {
            const std::string& t = st.p25_type;
            if (t == "TSDU") { return "canal de controle (só dados, sem voz)"; }
            if (t == "LDU1" || t == "LDU2") { return "voz"; }
            if (t == "HDU") { return "início de chamada"; }
            if (t == "TDU" || t == "TDULC") { return "fim de chamada"; }
            if (t == "PDU") { return "pacote de dados"; }
            return t;
        }
        std::string a = st.slot0_type, b = st.slot1_type;
        auto tr = [](const std::string& x) -> std::string {
            if (x == "VOICE") { return "voz"; }
            if (x == "VOICE Header") { return "início de chamada"; }
            if (x == "TLC") { return "fim de chamada"; }
            if (x == "CSBK") { return "controle (CSBK)"; }
            if (x == "Idle") { return "ocioso"; }
            if (x == "PI Header") { return "cabeçalho de criptografia"; }
            if (x.empty()) { return "-"; }
            if (x.find("DATA") != std::string::npos || x.find("MBC") != std::string::npos) { return "dados"; }
            return x;
        };
        return "slot 1: " + tr(a) + " · slot 2: " + tr(b);
    }

    void closeCall() {
        if (!callActive) { return; }
        callActive = false;
        calls.push_front(cur);
        while (calls.size() > 200) { calls.pop_back(); }
        // CSV na pasta do programa
        std::string path = std::string(core::getRoot()) + "/chamadas_digitais.csv";
        bool exists = false;
        { FILE* t = fopen(path.c_str(), "r"); if (t) { exists = true; fclose(t); } }
        FILE* f = fopen(path.c_str(), "a");
        if (f) {
            if (!exists) { fprintf(f, "data_hora,duracao_s,modo,frequencia_mhz,cc_nac,slot,tipo,destino,nome_destino,origem,nome_origem,criptografia,emergencia\n"); }
            char tbuf[32];
            struct tm lt;
#ifdef _WIN32
            localtime_s(&lt, &cur.wall);
#else
            localtime_r(&cur.wall, &lt);
#endif
            strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &lt);
            fprintf(f, "%s,%.1f,%s,%.5f,%d,%d,%s,%u,%s,%u,%s,%s,%s\n", tbuf, cur.last - cur.start, cur.mode.c_str(), cur.freq / 1e6,
                    cur.code, cur.slot + 1, cur.group ? "grupo" : "privada", cur.tg, nameOf(cur.group ? "TG" : "ID", cur.tg).c_str(),
                    cur.src, nameOf("ID", cur.src).c_str(), cur.encrypted ? algName(cur.algid).c_str() : "aberta", cur.emergency ? "sim" : "nao");
            fclose(f);
        }
    }

    void trackCalls() {
        demod::DSD::DebugStatus st;
        double now = nowSec();
        if (!gui::mainWindow.isPlaying() || !demod::DSD::getStatusForRadio("Radio", st)) {
            if (callActive && now - cur.last > 2.0) { closeCall(); }
            return;
        }
        bool voice = false;
        Call id;
        if (st.p25) {
            voice = st.sync && (st.p25_type == "LDU1" || st.p25_type == "LDU2" || st.p25_type == "HDU");
            id.mode = "P25";
            id.tg = st.p25_tg;
            id.src = st.p25_src;
            id.algid = st.p25_algid;
            id.keyid = st.p25_kid;
            id.encrypted = st.p25_algid != 0x80 && st.p25_algid != 0x00;
            id.emergency = st.p25_emergency;
            id.code = st.p25_nac;
        }
        else if (st.dmr) {
            voice = st.sync && st.voice;
            id.mode = "DMR";
            if (st.lc_count > 0) {
                id.tg = st.dst;
                id.src = st.src;
                id.group = st.group;
                id.slot = st.lc_slot;
                id.encrypted = st.privacy;
                id.emergency = st.emergency;
            }
            id.code = st.color_code;
            // um LC novo durante a chamada pode ser outra pessoa falando
            if (st.lc_count != lastLcSeen) { lastLcSeen = st.lc_count; }
        }
        if (voice) {
            bool changed = callActive && ((id.tg && cur.tg && id.tg != cur.tg) || (id.src && cur.src && id.src != cur.src) || id.mode != cur.mode);
            if (changed) { closeCall(); }
            if (!callActive) {
                cur = id;
                cur.start = now;
                cur.wall = time(NULL);
                cur.freq = radioFreq();
                callActive = true;
            }
            if (id.tg) { cur.tg = id.tg; cur.group = id.group; }
            if (id.src) { cur.src = id.src; }
            if (id.slot >= 0) { cur.slot = id.slot; }
            if (id.code > 0) { cur.code = id.code; }
            cur.encrypted = cur.encrypted || id.encrypted;
            if (id.encrypted) { cur.algid = id.algid; cur.keyid = id.keyid; }
            cur.emergency = cur.emergency || id.emergency;
            cur.last = now;
        }
        else if (callActive && now - cur.last > 2.0) {
            closeCall();
        }
    }

    void drawReception() {
        ImVec4 dim = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
        ImVec4 amber(0.96f, 0.69f, 0.25f, 1.0f), green(0.27f, 0.82f, 0.47f, 1.0f), red(1.0f, 0.45f, 0.4f, 1.0f);
        demod::DSD::DebugStatus st;
        if (!demod::DSD::getStatusForRadio("Radio", st)) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(dim, "Escolha o modo DMR (DSD) no Rádio para ver as informações do sinal digital.");
            ImGui::PopTextWrapPos();
            return;
        }
        ImGui::TextColored(dim, "RECEPÇÃO AGORA");
        if (!st.sync) {
            ImGui::TextColored(dim, "Sem sinal digital sincronizado");
        }
        else if (st.p25) {
            ImGui::TextColored(green, "P25 fase 1 · NAC %03X", st.p25_nac);
        }
        else if (st.dmr) {
            ImGui::TextColored(green, "DMR · color code %d", st.color_code);
        }
        else {
            ImGui::TextColored(green, "Sinal digital sincronizado");
        }
        if (st.sync) {
            ImGui::PushTextWrapPos(0);
            ImGui::Text("Quadro: %s", frameText(st).c_str());
            ImGui::PopTextWrapPos();
        }
        if (std::abs(st.offset_hz) >= 50) { ImGui::TextColored(dim, "Correção de frequência (AFC): %+.1f kHz", st.offset_hz / 1000.0); }
        if (ImGui::Checkbox("Simulcast (LSM/CQPSK)##cqpsk", &cqpsk)) {
            demod::DSD::setCqpskAll(cqpsk);
            saveConfig();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Para P25 de várias torres na mesma frequência (comum em segurança pública).\n"
                              "Sinal: sincroniza, mas a constelação vira uma faixa contínua e o DUID sai 'UNK'.\n"
                              "Deixe desligado para DMR e P25 comum (C4FM).");
        }
        if (st.p25 && st.p25_type == "UNK" && !cqpsk) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(amber, "Quadros P25 ilegíveis: se for simulcast, ligue a opção acima.");
            ImGui::PopTextWrapPos();
        }
        if (callActive) {
            double dur = nowSec() - cur.start;
            ImGui::TextColored(amber, "Chamada %s em andamento · %.0f s", cur.group ? "de grupo" : "privada", dur);
            ImGui::Text("Para:  %s %s", cur.group ? "TG" : "ID", label(cur.group ? "TG" : "ID", cur.tg).c_str());
            ImGui::Text("De:    ID %s", label("ID", cur.src).c_str());
            if (cur.slot >= 0) { ImGui::Text("Slot:  %d", cur.slot + 1); }
            if (cur.encrypted) {
                std::string a = cur.mode == "P25" ? algName(cur.algid) : std::string("privacy do DMR");
                if (cur.mode == "P25" && cur.keyid) {
                    char b[24];
                    snprintf(b, sizeof(b), ", chave 0x%04X", cur.keyid);
                    a += b;
                }
                ImGui::TextColored(red, "Criptografada (%s): sem áudio", a.c_str());
            }
            else {
                ImGui::TextColored(green, "Aberta");
            }
            if (cur.emergency) { ImGui::TextColored(red, "EMERGÊNCIA"); }
        }
    }

    void drawCallLog() {
        ImVec4 dim = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
        ImVec4 red(1.0f, 0.45f, 0.4f, 1.0f);
        if (!ImGui::TreeNodeEx("##calllog", 0, "Registro de chamadas (%d)", (int)calls.size())) { return; }
        if (calls.empty()) {
            ImGui::TextColored(dim, "Nenhuma chamada ainda.");
        }
        else {
            if (ImGui::SmallButton("Limpar##calls")) { calls.clear(); }
            ImGui::SameLine();
            ImGui::TextColored(dim, "também em chamadas_digitais.csv");
            if (ImGui::BeginTable("##calltable", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                                  ImVec2(0, 220.0f * style::uiScale))) {
                ImGui::TableSetupColumn("Hora");
                ImGui::TableSetupColumn("Dur.");
                ImGui::TableSetupColumn("Para");
                ImGui::TableSetupColumn("De");
                ImGui::TableSetupColumn("");
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                for (auto& c : calls) {
                    ImGui::TableNextRow();
                    char tbuf[16];
                    struct tm lt;
#ifdef _WIN32
                    localtime_s(&lt, &c.wall);
#else
                    localtime_r(&c.wall, &lt);
#endif
                    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &lt);
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(tbuf);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.0fs", c.last - c.start);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%s %s", c.group ? "TG" : "ID", label(c.group ? "TG" : "ID", c.tg).c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s · %.4f MHz · %s %d%s", c.mode.c_str(), c.freq / 1e6, c.mode == "P25" ? "NAC" : "CC", c.code,
                                          c.slot >= 0 ? (" · slot " + std::to_string(c.slot + 1)).c_str() : "");
                    }
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(label("ID", c.src).c_str());
                    ImGui::TableSetColumnIndex(4);
                    if (c.emergency) { ImGui::TextColored(red, "EMERG"); ImGui::SameLine(); }
                    if (c.encrypted) { ImGui::TextColored(red, "cripto"); }
                }
                ImGui::EndTable();
            }
        }
        ImGui::TreePop();
    }

    void drawNames() {
        if (!ImGui::TreeNodeEx("##names", 0, "Nomes de TG e ID (%d)", (int)names.size())) { return; }
        float w = ImGui::GetContentRegionAvail().x;
        ImGui::RadioButton("TG##nk0", &nameKind, 0);
        ImGui::SameLine();
        ImGui::RadioButton("ID##nk1", &nameKind, 1);
        ImGui::SetNextItemWidth(w * 0.3f);
        ImGui::InputTextWithHint("##nnum", "número", nameNum, sizeof(nameNum), ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(w * 0.45f);
        ImGui::InputTextWithHint("##ntext", "nome", nameText, sizeof(nameText));
        ImGui::SameLine();
        if (ImGui::Button("Salvar##nsave") && nameNum[0]) {
            std::string k = std::string(nameKind ? "ID:" : "TG:") + nameNum;
            if (nameText[0]) { names[k] = nameText; }
            else { names.erase(k); }
            nameNum[0] = nameText[0] = 0;
            saveConfig();
        }
        std::string del;
        for (auto& [k, v] : names) {
            ImGui::PushID(k.c_str());
            if (ImGui::SmallButton("x")) { del = k; }
            ImGui::PopID();
            ImGui::SameLine();
            ImGui::Text("%s %s = %s", k.substr(0, 2).c_str(), k.substr(3).c_str(), v.c_str());
        }
        if (!del.empty()) {
            names.erase(del);
            saveConfig();
        }
        ImGui::TreePop();
    }

    void followStep() {
        if (!followTrunk) {
            if (onVoice) { goBack(); }
            return;
        }
        dsp::p25::System::View v;
        if (!demod::DSD::getP25ViewForRadio("Radio", v)) { return; }
        double now = nowSec();
        if (!onVoice) {
            if (!v.active) { return; }
            ccFreq = radioFreq();
            for (auto& g : v.grants) {
                if (now - g.time > 1.0 || g.freq <= 0) { continue; }
                if (std::fabs(g.freq - ccFreq) < 100) { continue; }
                if (skipEncrypted && g.encrypted) { continue; }
                if (!tgAllowed(g.group)) { continue; }
                if (g.group == lastTg && now - leftAt < 1.0) { continue; }
                followTg = g.group;
                followSrc = g.source;
                voiceFreq = g.freq;
                onVoice = true;
                voiceStart = lastActivity = now;
                tuner::tune(tuner::TUNER_MODE_NORMAL, "Radio", g.freq);
                char msg[96];
                snprintf(msg, sizeof(msg), "P25: seguindo TG %u em %.4f MHz", g.group, g.freq / 1e6);
                flog::info("{}", msg);
                break;
            }
        }
        else {
            demod::DSD::DebugStatus st;
            if (demod::DSD::getStatusForRadio("Radio", st) && st.sync && !(st.p25 && st.p25_type == "TSDU")) { lastActivity = now; }
            if (now - lastActivity > 1.5 || now - voiceStart > 120) { goBack(); }
        }
    }

    void goBack() {
        onVoice = false;
        lastTg = followTg;
        leftAt = nowSec();
        if (ccFreq > 0) { tuner::tune(tuner::TUNER_MODE_NORMAL, "Radio", ccFreq); }
    }

    void saveConfig() {
        config.acquire();
        config.conf["followTrunk"] = followTrunk;
        config.conf["skipEncrypted"] = skipEncrypted;
        config.conf["tgFilter"] = tgFilter;
        config.conf["cqpsk"] = cqpsk;
        json n = json::object();
        for (auto& [k, v] : names) { n[k] = v; }
        config.conf["names"] = n;
        config.release(true);
    }

    static void menuHandler(void* ctx) {
        auto _this = (VhfVoiceRadioModule*)ctx;
        float w = ImGui::GetContentRegionAvail().x;
        dsp::p25::System::View v;
        _this->drawReception();
        _this->drawCallLog();
        _this->drawNames();
        bool have = demod::DSD::getP25ViewForRadio("Radio", v);
        ImVec4 dim = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
        ImVec4 amber(0.96f, 0.69f, 0.25f, 1.0f);
        if (!have) { return; }
        ImGui::Separator();
        ImGui::TextColored(dim, "P25 TRONCALIZADO");
        if (v.tsbks == 0) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(dim, "Nenhum canal de controle P25 recebido. Sintonize a frequência de controle (sinal contínuo) do sistema troncalizado.");
            ImGui::PopTextWrapPos();
        }
        else {
            ImGui::TextColored(v.active ? ImVec4(0.27f, 0.82f, 0.47f, 1.0f) : dim, v.active ? "Canal de controle ativo" : "Canal de controle (sem sinal agora)");
            ImGui::Text("Sistema %03X  WACN %05X", v.sysid, v.wacn);
            ImGui::Text("RFSS %d  Site %d", v.rfss, v.site);
            if (v.ccFreq > 0) { ImGui::Text("Controle: %.4f MHz", v.ccFreq / 1e6); }
            ImGui::TextColored(dim, "%llu blocos de controle", (unsigned long long)v.tsbks);
        }
        ImGui::Separator();
        if (ImGui::Checkbox("Seguir conversas##p25_follow", &_this->followTrunk)) { _this->saveConfig(); }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Vai para a frequência de voz quando um talkgroup recebe canal\ne volta para o controle quando a conversa termina."); }
        if (ImGui::Checkbox("Pular criptografadas##p25_skipenc", &_this->skipEncrypted)) { _this->saveConfig(); }
        char buf[128];
        strncpy(buf, _this->tgFilter.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputTextWithHint("##p25_tgs", "Só estes TGs (ex.: 1001, 1002); vazio = todos", buf, sizeof(buf))) {
            _this->tgFilter = buf;
            _this->saveConfig();
        }
        if (_this->onVoice) {
            ImGui::TextColored(amber, "Ouvindo TG %u (origem %u) em %.4f MHz", _this->followTg, _this->followSrc, _this->voiceFreq / 1e6);
        }
        if (!v.grants.empty()) {
            ImGui::Separator();
            ImGui::TextColored(dim, "Conversas recentes (clique para ouvir):");
            double now = nowSec();
            int i = 0;
            for (auto& g : v.grants) {
                if (i >= 12) { break; }
                char lbl[160];
                double age = now - g.time;
                snprintf(lbl, sizeof(lbl), "TG %-6u %s %s  há %.0fs%s%s##g%d", g.group,
                         g.freq > 0 ? "" : "canal", g.freq > 0 ? (std::to_string(g.freq / 1e6).substr(0, 8) + " MHz").c_str() : std::to_string(g.channel).c_str(),
                         age, g.encrypted ? "  [cripto]" : "", g.emergency ? "  [EMERG]" : "", i);
                ImGui::PushStyleColor(ImGuiCol_Text, age < 3 ? amber : ImGui::GetStyle().Colors[ImGuiCol_Text]);
                if (ImGui::Selectable(lbl, false) && g.freq > 0) { tuner::tune(tuner::TUNER_MODE_NORMAL, "Radio", g.freq); }
                ImGui::PopStyleColor();
                i++;
            }
        }
        return;
    }

    bool enabled = true;
};


SDRPP_MOD_INFO{
    /* Name:            */ "ch_extravhf_decoder",
    /* Description:     */ "Additional modes for V/UHF voice",
    /* Author:          */ "cropinghigh",
    /* Version:         */ 0, 0, 6,
    /* Max instances    */ -1
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(std::string(core::getRoot()) + "/ch_extravhf_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new VhfVoiceRadioModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    auto mymod = (VhfVoiceRadioModule*)instance;
    mymod->uninject();
    delete mymod;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
