// Módulo ACARS para SDR++: decodifica vários canais ACARS (VHF AM-MSK 2400 bit/s)
// ao mesmo tempo, dentro da faixa que o RTL-SDR já está recebendo, e mostra as
// mensagens com CPDLC/ADS-C decodificados pela libacars.
#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <gui/tuner.h>
#include <gui/main_window.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <dsp/sink/handler_sink.h>
#include <utils/flog.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <algorithm>
#include "acars.h"
#include "apps.h"

SDRPP_MOD_INFO{
    /* Name:            */ "acars_decoder",
    /* Description:     */ "ACARS com CPDLC/ADS-C (libacars)",
    /* Author:          */ "SDR++Brown simples",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

static const double CH_BW = 10000.0;
static const int MAX_MESSAGES = 500;

// Canais ACARS mais comuns (o 131,550 é o canal mundial principal)
static const double DEFAULT_CHANNELS[] = { 131.550e6, 131.725e6, 131.825e6, 131.450e6, 131.125e6, 130.025e6, 129.125e6 };
static const bool DEFAULT_ENABLED[] = { true, true, true, true, true, true, false };

static std::mutex appsMtx;   // a libacars é chamada por várias threads de canal

static double nowSec() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

static std::string freqText(double hz) {
    char b[32];
    snprintf(b, sizeof(b), "%.3f", hz / 1e6);
    return b;
}

static std::string timeText(double t, bool date = false) {
    time_t tt = (time_t)t;
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    char b[32];
    strftime(b, sizeof(b), date ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S", &lt);
    return b;
}

class ACARSModule : public ModuleManager::Instance {
    struct Channel {
        double freq = 0;
        bool enabled = true;
        bool inRange = false;
        VFOManager::VFO* vfo = NULL;
        dsp::sink::Handler<dsp::complex_t> sink;
        acars::ChannelDecoder dec;
        std::vector<std::complex<float>> tmp;
        std::atomic<int> messages { 0 };
        std::atomic<double> lastBurst { 0 };
        ACARSModule* owner = NULL;
        std::string vfoName;
        bool sinkReady = false;
    };

public:
    ACARSModule(std::string name) {
        this->name = name;
        config.acquire();
        if (config.conf.contains("channels") && config.conf["channels"].is_array()) {
            for (auto& c : config.conf["channels"]) {
                addChannel(c.value("freq", 0.0), c.value("enabled", true));
            }
        }
        if (channels.empty()) {
            for (int i = 0; i < (int)(sizeof(DEFAULT_CHANNELS) / sizeof(double)); i++) { addChannel(DEFAULT_CHANNELS[i], DEFAULT_ENABLED[i]); }
        }
        hideSqueaks = config.conf.value("hideSqueaks", true);
        onlyApps = config.conf.value("onlyApps", false);
        logToFile = config.conf.value("logToFile", true);
        config.release();

        gui::menu.registerEntry(name, menuHandler, this, this);
        frameHandler.handler = onFrame;
        frameHandler.ctx = this;
        gui::mainWindow.onWaterfallDrawn.bindHandler(&frameHandler);
    }

    ~ACARSModule() {
        gui::mainWindow.onWaterfallDrawn.unbindHandler(&frameHandler);
        stop();
        gui::menu.removeEntry(name);
    }

    void postInit() {}
    void enable() { enabled = true; }
    void disable() {
        stop();
        enabled = false;
    }
    bool isEnabled() { return enabled; }

    std::string handleDebugCommand(const std::string& cmd, const std::string& args) override {
        // VFOs só podem ser mexidos na thread da interface: o pedido é feito no próximo quadro
        if (cmd == "start") { pending = 1; return "{\"status\":\"ok\"}"; }
        if (cmd == "stop") { pending = 2; return "{\"status\":\"ok\"}"; }
        if (cmd == "get_stats") {
            std::lock_guard<std::mutex> lck(msgMtx);
            json j;
            j["running"] = running.load();
            j["messages"] = totalMessages.load();
            j["last"] = lastSummary;
            json ch = json::array();
            for (auto& c : channels) {
                ch.push_back({ { "freq", c->freq }, { "enabled", c->enabled }, { "in_range", c->inRange },
                               { "messages", c->messages.load() }, { "bursts", c->dec.bursts } });
            }
            j["channels"] = ch;
            return j.dump();
        }
        if (cmd == "get_messages") {
            std::lock_guard<std::mutex> lck(msgMtx);
            json arr = json::array();
            for (auto& m : messages) {
                arr.push_back({ { "time", m.time }, { "freq", m.freq }, { "reg", m.reg }, { "flight", m.flight },
                                { "label", m.label }, { "block", std::string(1, m.blockId) }, { "downlink", m.downlink },
                                { "text", acars::sanitize(m.text) }, { "app", m.app }, { "summary", m.summary }, { "decoded", m.decoded } });
            }
            return arr.dump();
        }
        if (cmd == "inject") {
            // Teste sem antena: gera uma rajada sintética e passa pelo mesmo caminho do rádio.
            // args: "uplink" | "cpdlc" | "adsc" | "" (descida comum)
            acars::Message m;
            m.mode = '2';
            m.reg = "PR-XYZ";
            m.downlink = true;
            m.blockId = '2';
            m.msgNo = "M12A";
            m.flight = "AD4321";
            m.label = "H1";
            m.text = "#DFBPOS S2314.3W04552.1 FL350 SBGR SBCF";
            if (args == "cpdlc") {
                m.label = "AA";
                m.downlink = false;
                m.blockId = 'D';
                m.reg = "9V-SVG";
                m.text = "/AKLCDYA.AT1.9V-SVG21D0755D84AD067448398722949A7521C8AB4A1C8EAB5CE393";
            }
            else if (args == "adsc") {
                m.label = "B6";
                m.reg = "VT-ANB";
                m.text = "/BOMASAI.ADS.VT-ANB072501A070A988CA73248F0E5DC10200000F5EE1ABC000102B885E0A19F5";
            }
            else if (args == "uplink") {
                m.label = "5Z";
                m.downlink = false;
                m.blockId = 'A';
                m.text = "/ METAR SBSJ 301800Z 12008KT 9999 FEW030 26/14 Q1016";
            }
            auto env = acars::synthesize(m, 25, (unsigned)totalMessages.load() + 1, 2);
            Channel* ch = channels.empty() ? NULL : channels[0].get();
            for (auto& c : channels) {
                if (std::fabs(c->freq - 131.550e6) < 500) { ch = c.get(); }
            }
            acars::ChannelDecoder dec;
            bool got = false;
            dec.onMessage = [&](acars::Message& o) { got = true; deliver(ch, o); };
            std::vector<std::complex<float>> iq;
            for (int i = 0; i < 4800; i++) { iq.push_back(std::complex<float>(0.02f, 0)); }
            for (float e : env) { iq.push_back(std::complex<float>(e * 0.5f, 0)); }
            for (int i = 0; i < 4800; i++) { iq.push_back(std::complex<float>(0.02f, 0)); }
            dec.process(iq.data(), (int)iq.size());
            return std::string("{\"decoded\":") + (got ? "true" : "false") + "}";
        }
        return "{}";
    }

private:
    // -------------------------------------------------------------- canais
    void addChannel(double f, bool en) {
        if (f < 100e6 || f > 200e6) { return; }
        for (auto& c : channels) {
            if (std::fabs(c->freq - f) < 500) { return; }
        }
        auto c = std::make_unique<Channel>();
        c->freq = f;
        c->enabled = en;
        c->owner = this;
        c->vfoName = "ACARS " + freqText(f);
        c->dec.onMessage = [ch = c.get()](acars::Message& m) { ch->owner->deliver(ch, m); };
        channels.push_back(std::move(c));
        std::sort(channels.begin(), channels.end(), [](const std::unique_ptr<Channel>& a, const std::unique_ptr<Channel>& b) { return a->freq < b->freq; });
    }

    void saveConfig() {
        config.acquire();
        json arr = json::array();
        for (auto& c : channels) { arr.push_back({ { "freq", c->freq }, { "enabled", c->enabled } }); }
        config.conf["channels"] = arr;
        config.conf["hideSqueaks"] = hideSqueaks;
        config.conf["onlyApps"] = onlyApps;
        config.conf["logToFile"] = logToFile;
        config.release(true);
    }

    static void dataHandler(dsp::complex_t* data, int count, void* ctx) {
        Channel* c = (Channel*)ctx;
        if ((int)c->tmp.size() < count) { c->tmp.resize(count); }
        for (int i = 0; i < count; i++) { c->tmp[i] = std::complex<float>(data[i].re, data[i].im); }
        bool was = c->dec.inBurst();
        c->dec.process(c->tmp.data(), count);
        if (was || c->dec.inBurst()) { c->lastBurst = nowSec(); }
    }

    void openChannel(Channel* c, double offset) {
        c->vfo = sigpath::vfoManager.createVFO(c->vfoName, ImGui::WaterfallVFO::REF_CENTER, offset, CH_BW, acars::SAMPLE_RATE, CH_BW, CH_BW, true);
        if (!c->vfo) { return; }
        c->dec.reset();
        if (!c->sinkReady) {
            c->sink.init(c->vfo->output, dataHandler, c);
            c->sinkReady = true;
        }
        else {
            c->sink.setInput(c->vfo->output);
        }
        c->sink.start();
    }

    void closeChannel(Channel* c) {
        if (!c->vfo) { return; }
        c->sink.stop();
        sigpath::vfoManager.deleteVFO(c->vfo);
        c->vfo = NULL;
    }

    // Recalcula a posição dos canais sempre que a sintonia muda (roda na thread da interface)
    void syncChannels() {
        double center = gui::waterfall.getCenterFrequency();
        double half = gui::waterfall.getBandwidth() / 2.0;
        for (auto& cp : channels) {
            Channel* c = cp.get();
            double off = c->freq - center;
            bool fits = running && c->enabled && std::fabs(off) + CH_BW / 2 < half * 0.95;
            c->inRange = fits;
            if (fits && !c->vfo) { openChannel(c, off); }
            else if (!fits && c->vfo) { closeChannel(c); }
            else if (fits && c->vfo && std::fabs(c->vfo->getOffset() - off) > 1.0) { c->vfo->setOffset(off); }
        }
    }

    static void onFrame(ImGuiContext* ctx, void* p) {
        ACARSModule* _this = (ACARSModule*)p;
        int req = _this->pending.exchange(0);
        if (req == 1) { _this->start(); }
        else if (req == 2) { _this->stop(); }
        if (_this->running) { _this->syncChannels(); }
        if (_this->showWindow) { _this->drawWindow(); }
    }

    void stopScannerIfRunning() {
        for (auto& [n, inst] : core::moduleManager.instances) {
            if (inst.module.info && std::string(inst.module.info->name) == "frequency_manager" && inst.instance) {
                inst.instance->handleDebugCommand("stop_scanner", "");
            }
        }
    }

    void start() {
        if (running || !enabled) { return; }
        stopScannerIfRunning();   // o scanner tiraria a faixa de cima dos canais
        running = true;
        // Centraliza a faixa recebida nos canais ligados, se algum estiver de fora
        double lo = 1e12, hi = 0;
        for (auto& c : channels) {
            if (!c->enabled) { continue; }
            lo = std::min(lo, c->freq);
            hi = std::max(hi, c->freq);
        }
        if (hi > 0) {
            double center = gui::waterfall.getCenterFrequency();
            double half = gui::waterfall.getBandwidth() / 2.0;
            bool allIn = (lo - center) > -half * 0.9 && (hi - center) < half * 0.9;
            if (!allIn) {
                double radio = sigpath::vfoManager.vfoExists("Radio") ? center + sigpath::vfoManager.getOffset("Radio") : 0;
                tuner::tune(tuner::TUNER_MODE_CENTER, "", (lo + hi) / 2.0);
                // Coloca o rádio no canal principal para ouvir as rajadas
                double main = 131.550e6;
                bool hasMain = false;
                for (auto& c : channels) { hasMain |= c->enabled && std::fabs(c->freq - main) < 500; }
                if (!hasMain) { main = lo; }
                double nc = (lo + hi) / 2.0;
                if (sigpath::vfoManager.vfoExists("Radio") && std::fabs(main - nc) < half * 0.8) {
                    tuner::tune(tuner::TUNER_MODE_NORMAL, "Radio", main);
                }
                (void)radio;
            }
        }
        syncChannels();
        flog::info("ACARS: iniciado");
    }

    void stop() {
        if (!running) { return; }
        running = false;
        for (auto& c : channels) {
            closeChannel(c.get());
            c->inRange = false;
        }
        flog::info("ACARS: parado");
    }

    // -------------------------------------------------------------- mensagens
    void deliver(Channel* ch, acars::Message& m) {
        m.freq = ch ? ch->freq : 0;
        m.time = (double)std::time(nullptr);
        {
            std::lock_guard<std::mutex> lck(appsMtx);
            acars::decodeApps(m);
        }
        if (ch) { ch->messages++; }
        totalMessages++;
        std::string line;
        {
            std::lock_guard<std::mutex> lck(msgMtx);
            messages.push_front(m);
            while ((int)messages.size() > MAX_MESSAGES) { messages.pop_back(); }
            lastSummary = (m.reg.empty() ? std::string("?") : m.reg) + " " + m.label + (m.app.empty() ? "" : " " + m.app);
        }
        flog::info("ACARS {} {} {} {} {}", freqText(m.freq), m.reg, m.flight, m.label, acars::sanitize(m.text));
        if (logToFile) {
            std::ofstream f(std::string(core::getRoot()) + "/acars_log.txt", std::ios::app);
            if (f) {
                f << timeText(m.time, true) << "  " << freqText(m.freq) << "  " << (m.downlink ? "desce" : "sobe ") << "  "
                  << m.reg << "  " << m.flight << "  label " << m.label << "  bloco " << m.blockId << "\n"
                  << acars::sanitize(m.text) << "\n";
                if (!m.decoded.empty()) { f << "[" << m.app << "] " << m.summary << "\n" << m.decoded << "\n"; }
                f << "\n";
            }
        }
    }

    bool visible(const acars::Message& m) const {
        if (hideSqueaks && m.text.empty() && (m.label == "_d" || m.label == "SQ" || m.label == "Q0")) { return false; }
        if (onlyApps && m.app.empty()) { return false; }
        if (!filter.empty()) {
            std::string hay = m.reg + " " + m.flight + " " + m.label + " " + m.text;
            std::string a = hay, b = filter;
            std::transform(a.begin(), a.end(), a.begin(), ::toupper);
            std::transform(b.begin(), b.end(), b.begin(), ::toupper);
            if (a.find(b) == std::string::npos) { return false; }
        }
        return true;
    }

    // Cartão de uma mensagem
    void drawMessage(const acars::Message& m, int idx, bool wide) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::PushID(idx);
        ImGui::BeginGroup();
        const ImU32 AMBER = IM_COL32(245, 176, 65, 255), BLUE = IM_COL32(76, 194, 255, 255), GREEN = IM_COL32(70, 210, 120, 255);
        ImVec4 dim = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
        ImGui::TextColored(dim, "%s  %s", timeText(m.time).c_str(), freqText(m.freq).c_str());
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(m.downlink ? GREEN : BLUE), m.downlink ? "avião > solo" : "solo > avião");
        if (!m.app.empty()) {
            ImGui::SameLine();
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImVec2 ts = ImGui::CalcTextSize(m.app.c_str());
            float pad = 4.0f * style::uiScale;
            dl->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + ts.x + 2 * pad, p.y + ts.y), AMBER, 3.0f * style::uiScale);
            dl->AddText(ImVec2(p.x + pad, p.y), IM_COL32(20, 20, 20, 255), m.app.c_str());
            ImGui::Dummy(ImVec2(ts.x + 2 * pad, ts.y));
        }
        std::string head = (m.reg.empty() ? std::string("(sem matrícula)") : m.reg);
        if (!m.flight.empty()) { head += "   voo " + m.flight; }
        ImGui::TextUnformatted(head.c_str());
        std::string desc = acars::labelDescription(m.label);
        ImGui::SameLine();
        ImGui::TextColored(dim, "label %s%s%s", m.label.c_str(), desc.empty() ? "" : " · ", desc.c_str());
        std::string t = acars::sanitize(m.text);
        if (!t.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            if (!wide && t.size() > 160) { t = t.substr(0, 160) + "..."; }
            ImGui::TextUnformatted(t.c_str());
            ImGui::PopTextWrapPos();
        }
        if (!m.summary.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(AMBER), "%s", m.summary.c_str());
            ImGui::PopTextWrapPos();
        }
        if (!m.decoded.empty()) {
            if (ImGui::TreeNodeEx("##dec", 0, "Detalhes %s", m.app.c_str())) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(m.decoded.c_str());
                ImGui::PopTextWrapPos();
                ImGui::TreePop();
            }
        }
        ImGui::EndGroup();
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        dl->AddLine(ImVec2(a.x, b.y + 3 * style::uiScale), ImVec2(a.x + ImGui::GetContentRegionAvail().x, b.y + 3 * style::uiScale), ImGui::GetColorU32(ImGuiCol_Border));
        ImGui::Dummy(ImVec2(0, 4 * style::uiScale));
        ImGui::PopID();
    }

    void drawFilters() {
        if (ImGui::Checkbox("Ocultar sinais de vida##acars_sq", &hideSqueaks)) { saveConfig(); }
        ImGui::SameLine();
        if (ImGui::Checkbox("Só CPDLC/ADS-C##acars_apps", &onlyApps)) { saveConfig(); }
        char buf[64];
        strncpy(buf, filter.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputTextWithHint("##acars_filter", "Filtrar por matrícula, voo, label ou texto", buf, sizeof(buf))) { filter = buf; }
    }

    void drawWindow() {
        ImGui::SetNextWindowSize(ImVec2(620 * style::uiScale, 520 * style::uiScale), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Mensagens ACARS", &showWindow)) {
            ImGui::End();
            return;
        }
        ImGui::Text("%d mensagens", totalMessages.load());
        ImGui::SameLine();
        if (ImGui::SmallButton("Limpar##acars_wclear")) {
            std::lock_guard<std::mutex> lck(msgMtx);
            messages.clear();
        }
        drawFilters();
        ImGui::BeginChild("##acars_wlist");
        {
            std::lock_guard<std::mutex> lck(msgMtx);
            int i = 0;
            for (auto& m : messages) {
                if (visible(m)) { drawMessage(m, i, true); }
                i++;
            }
        }
        ImGui::EndChild();
        ImGui::End();
    }

    static void menuHandler(void* ctx) {
        ACARSModule* _this = (ACARSModule*)ctx;
        float w = ImGui::GetContentRegionAvail().x;
        bool run = _this->running;

        if (!run) {
            if (ImGui::Button("Iniciar ACARS##acars_start", ImVec2(w, 0))) { _this->start(); }
        }
        else if (ImGui::Button("Parar ACARS##acars_stop", ImVec2(w, 0))) { _this->stop(); }

        // Canais: clique liga/desliga; ponto verde = rajada agora
        ImGui::TextDisabled("Canais (MHz): clique para ligar/desligar");
        double now = nowSec();
        float x0 = ImGui::GetCursorPosX();
        float right = x0 + w;
        bool changed = false;
        int del = -1;
        for (int i = 0; i < (int)_this->channels.size(); i++) {
            Channel* c = _this->channels[i].get();
            std::string label = freqText(c->freq);
            if (c->messages > 0) { label += " (" + std::to_string(c->messages.load()) + ")"; }
            float bw = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2 + 10 * style::uiScale;
            if (i > 0) {
                ImGui::SameLine();
                if (ImGui::GetCursorPosX() + bw > right) { ImGui::NewLine(); }
            }
            ImGui::PushID(i);
            ImVec4 col = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
            if (c->enabled) { col = ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]; }
            ImGui::PushStyleColor(ImGuiCol_Button, col);
            if (!c->enabled) { ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]); }
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::Button((label + "##ch").c_str(), ImVec2(bw, 0))) {
                c->enabled = !c->enabled;
                changed = true;
            }
            if (ImGui::BeginPopupContextItem("##chctx")) {
                if (ImGui::MenuItem("Remover canal")) { del = i; }
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\n%s\nRajadas: %llu  Mensagens: %d\nBotão direito: remover", c->vfoName.c_str(),
                                  !run ? "parado" : (c->inRange ? "recebendo" : (c->enabled ? "fora da faixa sintonizada" : "desligado")),
                                  (unsigned long long)c->dec.bursts, c->messages.load());
            }
            if (!c->enabled) { ImGui::PopStyleColor(); }
            ImGui::PopStyleColor();
            ImU32 dot = IM_COL32(90, 104, 120, 255);
            if (run && c->inRange) { dot = (now - c->lastBurst < 0.4) ? IM_COL32(70, 210, 120, 255) : IM_COL32(76, 194, 255, 200); }
            else if (run && c->enabled) { dot = IM_COL32(220, 90, 80, 255); }
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 6 * style::uiScale, p.y + ImGui::GetFrameHeight() / 2), 3 * style::uiScale, dot);
            ImGui::PopID();
        }
        if (del >= 0) {
            _this->closeChannel(_this->channels[del].get());
            _this->channels.erase(_this->channels.begin() + del);
            changed = true;
        }
        ImGui::SetNextItemWidth(w - 70 * style::uiScale);
        ImGui::InputDouble("##acars_newch", &_this->newChannel, 0, 0, "%.3f MHz");
        ImGui::SameLine();
        if (ImGui::Button("Incluir##acars_add", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            _this->addChannel(_this->newChannel * 1e6, true);
            changed = true;
        }
        if (changed) {
            _this->saveConfig();
            if (_this->running) { _this->syncChannels(); }
        }
        if (run) {
            int out = 0;
            for (auto& c : _this->channels) { out += (c->enabled && !c->inRange); }
            if (out) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(245, 176, 65, 255));
                ImGui::PushTextWrapPos(0);
                ImGui::Text("%d canal(is) fora da faixa sintonizada. Sintonize perto de 131 MHz ou pare e inicie de novo.", out);
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
        }
        if (ImGui::Checkbox("Gravar em acars_log.txt##acars_log", &_this->logToFile)) { _this->saveConfig(); }

        ImGui::Separator();
        ImGui::Text("Mensagens: %d", _this->totalMessages.load());
        ImGui::SameLine();
        float bw2 = ImGui::CalcTextSize("Abrir janela").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetCursorPosX(x0 + w - bw2);
        if (ImGui::SmallButton(_this->showWindow ? "Fechar janela##acars_win" : "Abrir janela##acars_win")) { _this->showWindow = !_this->showWindow; }

        // Últimas mensagens (resumo); a janela grande tem filtro e o texto completo
        std::lock_guard<std::mutex> lck(_this->msgMtx);
        int shown = 0, i = 0;
        for (auto& m : _this->messages) {
            if (shown >= 6) { break; }
            if (_this->visible(m)) {
                _this->drawMessage(m, i, false);
                shown++;
            }
            i++;
        }
        if (_this->messages.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled(run ? "Aguardando mensagens... (ACARS aparece em rajadas curtas, às vezes minutos de intervalo)"
                                    : "Inicie para decodificar ACARS. CPDLC e ADS-C (FANS-1/A) são decodificados automaticamente.");
            ImGui::PopTextWrapPos();
        }
    }

    std::string name;
    bool enabled = true;
    std::atomic<bool> running { false };
    std::atomic<int> pending { 0 };
    std::vector<std::unique_ptr<Channel>> channels;
    EventHandler<ImGuiContext*> frameHandler;

    std::mutex msgMtx;
    std::deque<acars::Message> messages;
    std::atomic<int> totalMessages { 0 };
    std::string lastSummary;

    bool showWindow = false;
    bool hideSqueaks = true;
    bool onlyApps = false;
    bool logToFile = true;
    std::string filter;
    double newChannel = 131.550;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(std::string(core::getRoot()) + "/acars_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new ACARSModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (ACARSModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
