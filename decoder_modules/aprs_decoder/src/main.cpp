// Módulo APRS para SDR++: decodifica o canal APRS (145,570 MHz no Brasil) dentro da
// faixa que o RTL-SDR já está recebendo, lista as estações, posições e mensagens e
// manda as estações para o mapa do ADS-B (o mesmo mapa no navegador, inclusive no celular).
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
#include <utils/cty.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <algorithm>
#include "aprs.h"

#ifdef _WIN32
#include <shellapi.h>
#endif
#ifdef __ANDROID__
namespace backend { void openUrl(const std::string& url); }
#endif

SDRPP_MOD_INFO{
    /* Name:            */ "aprs_decoder",
    /* Description:     */ "APRS (VHF 1200 bit/s) com estações no mapa",
    /* Author:          */ "SDR+Easy",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

static const double CH_BW = 12500.0;
static const double DEFAULT_FREQ = 145.570e6;   // APRS no Brasil
static const double STATION_TTL = 3 * 3600.0;   // some da lista depois de 3 h sem ouvir

static double wallSec() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(system_clock::now().time_since_epoch()).count();
}

static std::string timeText(double t) {
    time_t tt = (time_t)t;
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    char b[16];
    strftime(b, sizeof(b), "%H:%M:%S", &lt);
    return b;
}

static std::string agoText(double s) {
    char b[32];
    if (s < 60) { snprintf(b, sizeof(b), "%.0f s", s); }
    else if (s < 3600) { snprintf(b, sizeof(b), "%.0f min", s / 60); }
    else { snprintf(b, sizeof(b), "%.1f h", s / 3600); }
    return b;
}

static void openUrl(const std::string& url) {
#if defined(__ANDROID__)
    backend::openUrl(url);
#elif defined(_WIN32)
    ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open '" + url + "' >/dev/null 2>&1 &";
    if (system(cmd.c_str()) != 0) { flog::warn("APRS: não consegui abrir o navegador"); }
#else
    std::string cmd = "xdg-open '" + url + "' >/dev/null 2>&1 &";
    if (system(cmd.c_str()) != 0) { flog::warn("APRS: não consegui abrir o navegador"); }
#endif
}

static double distKm(double la1, double lo1, double la2, double lo2) {
    const double R = 6371.0, k = M_PI / 180.0;
    double dla = (la2 - la1) * k, dlo = (lo2 - lo1) * k;
    double a = std::sin(dla / 2) * std::sin(dla / 2) + std::cos(la1 * k) * std::cos(la2 * k) * std::sin(dlo / 2) * std::sin(dlo / 2);
    return 2 * R * std::asin(std::sqrt((std::min)(1.0, a)));
}

static std::string jsonSafe(const std::string& s) {
    // json do nlohmann recusa UTF-8 inválido: mantém as sequências válidas (acentos)
    // e troca só os bytes soltos (Latin-1 de alguns rádios) por '?'
    std::string o;
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = len > 0 && i + len <= n;
        for (int k = 1; ok && k < len; k++) { ok = ((unsigned char)s[i + k] >> 6) == 2; }
        if (ok) {
            o.append(s, i, len);
            i += len;
        }
        else {
            o += '?';
            i++;
        }
    }
    return o;
}

class APRSModule : public ModuleManager::Instance {
    struct Station {
        std::string key, call;
        aprs::Report rep;          // último relatório com posição (ou o último de todos)
        bool hasPos = false;
        double first = 0, last = 0, lastPos = 0;
        int packets = 0;
        bool direct = false;
        std::string path, raw, status;
        std::deque<std::pair<double, double>> track;
        bool object = false;
    };
    struct Msg {
        double time;
        std::string from, to, text;
    };

public:
    APRSModule(std::string name) {
        this->name = name;
        config.acquire();
        freq = config.conf.value("freq", DEFAULT_FREQ);
        logToFile = config.conf.value("logToFile", true);
        config.release();
        freqMHz = freq / 1e6;

        demod.onFrame = [this](const aprs::Frame& f) { onFrame(f); };
        gui::menu.registerEntry(name, menuHandler, this, this);
        frameHandler.handler = onGuiFrame;
        frameHandler.ctx = this;
        gui::mainWindow.onWaterfallDrawn.bindHandler(&frameHandler);
    }

    ~APRSModule() {
        gui::mainWindow.onWaterfallDrawn.unbindHandler(&frameHandler);
        closeVfo();
        gui::menu.removeEntry(name);
    }

    void postInit() {}
    void enable() { enabled = true; }
    void disable() {
        pending = 2;
        closeVfo();
        running = false;
        enabled = false;
    }
    bool isEnabled() { return enabled; }

    std::string handleDebugCommand(const std::string& cmd, const std::string& args) override {
        // VFOs só na thread da interface: o pedido é atendido no próximo quadro
        if (cmd == "start") { pending = 1; return "{\"status\":\"ok\"}"; }
        if (cmd == "stop") { pending = 2; return "{\"status\":\"ok\"}"; }
        if (cmd == "get_stats") {
            std::lock_guard<std::mutex> lck(mtx);
            json j;
            j["running"] = running.load();
            j["in_range"] = inRange.load();
            j["freq"] = freq;
            j["packets"] = packets;
            j["stations"] = (int)stations.size();
            j["bad_fcs"] = demod.badFcs;
            j["last"] = jsonSafe(lastSummary);
            return j.dump();
        }
        if (cmd == "get_stations") { return stationsJson(args == "map"); }
        if (cmd == "get_messages") {
            std::lock_guard<std::mutex> lck(mtx);
            json a = json::array();
            for (auto& m : msgs) { a.push_back({ { "time", m.time }, { "from", m.from }, { "to", m.to }, { "text", jsonSafe(m.text) } }); }
            return a.dump();
        }
        if (cmd == "inject") {
            // Teste sem antena: monta o quadro (TNC2 "SRC>DST,PATH:info"), gera o áudio
            // AFSK e passa pelo mesmo demodulador do rádio
            std::string t = args.empty() ? "PY2TST-9>APRS,WIDE1-1:!2333.03S/04637.99W>Teste do SDR+Easy" : args;
            size_t gt = t.find('>'), co = t.find(':');
            if (gt == std::string::npos || co == std::string::npos || gt > co) { return "{\"error\":\"formato SRC>DST,PATH:info\"}"; }
            aprs::Frame f;
            f.src = t.substr(0, gt);
            std::string hdr = t.substr(gt + 1, co - gt - 1);
            size_t c = 0;
            while (true) {
                size_t n = hdr.find(',', c);
                std::string part = hdr.substr(c, n == std::string::npos ? std::string::npos : n - c);
                if (f.dst.empty()) { f.dst = part; }
                else { f.path.push_back(part); }
                if (n == std::string::npos) { break; }
                c = n + 1;
            }
            f.info = t.substr(co + 1);
            auto audio = aprs::synthesize(f, 30);
            std::vector<float> pad(4800, 0.0f);
            uint64_t before = demod.frames;
            {
                std::lock_guard<std::mutex> lck(demodMtx);
                demod.process(pad.data(), (int)pad.size());
                demod.process(audio.data(), (int)audio.size());
                demod.process(pad.data(), (int)pad.size());
            }
            return std::string("{\"decoded\":") + (demod.frames > before ? "true" : "false") + "}";
        }
        return "{}";
    }

private:
    // -------------------------------------------------------------- recepção
    static void dataHandler(dsp::complex_t* data, int count, void* ctx) {
        APRSModule* _this = (APRSModule*)ctx;
        if ((int)_this->audio.size() < count) { _this->audio.resize(count); }
        // FM: diferença de fase entre amostras
        dsp::complex_t p = _this->prev;
        for (int i = 0; i < count; i++) {
            dsp::complex_t s = data[i];
            float re = s.re * p.re + s.im * p.im, im = s.im * p.re - s.re * p.im;
            _this->audio[i] = atan2f(im, re);
            p = s;
        }
        _this->prev = p;
        std::lock_guard<std::mutex> lck(_this->demodMtx);
        _this->demod.process(_this->audio.data(), count);
    }

    void openVfo(double offset) {
        vfo = sigpath::vfoManager.createVFO("APRS", ImGui::WaterfallVFO::REF_CENTER, offset, CH_BW, aprs::SAMPLE_RATE, CH_BW, CH_BW, true);
        if (!vfo) { return; }
        demod.reset();
        if (!sinkReady) {
            sink.init(vfo->output, dataHandler, this);
            sinkReady = true;
        }
        else {
            sink.setInput(vfo->output);
        }
        sink.start();
    }

    void closeVfo() {
        if (!vfo) { return; }
        sink.stop();
        sigpath::vfoManager.deleteVFO(vfo);
        vfo = NULL;
    }

    // Abre/fecha/move o VFO conforme a faixa recebida (thread da interface)
    void syncVfo() {
        double center = gui::waterfall.getCenterFrequency();
        double half = gui::waterfall.getBandwidth() / 2.0;
        double off = freq - center;
        bool fits = running && std::fabs(off) + CH_BW / 2 < half * 0.95;
        inRange = fits;
        if (fits && !vfo) { openVfo(off); }
        else if (!fits && vfo) { closeVfo(); }
        else if (fits && vfo && std::fabs(vfo->getOffset() - off) > 1.0) { vfo->setOffset(off); }
    }

    void start() {
        if (running || !enabled) { return; }
        running = true;
        // Se o canal está fora da faixa recebida, leva a faixa até ele (o Rádio continua onde está, se couber)
        double center = gui::waterfall.getCenterFrequency();
        double half = gui::waterfall.getBandwidth() / 2.0;
        if (std::fabs(freq - center) + CH_BW / 2 >= half * 0.9) {
            double radio = sigpath::vfoManager.vfoExists("Radio") ? center + sigpath::vfoManager.getOffset("Radio") : 0;
            double nc = (radio > 0 && std::fabs(radio - freq) < half * 1.4) ? (radio + freq) / 2.0 : freq;
            tuner::tune(tuner::TUNER_MODE_CENTER, "", nc);
        }
        syncVfo();
        flog::info("APRS: iniciado em {:.3f} MHz", freq / 1e6);
    }

    void stop() {
        if (!running) { return; }
        running = false;
        closeVfo();
        inRange = false;
        flog::info("APRS: parado");
    }

    static void onGuiFrame(ImGuiContext* ctx, void* p) {
        APRSModule* _this = (APRSModule*)p;
        int req = _this->pending.exchange(0);
        if (req == 1) { _this->start(); }
        else if (req == 2) { _this->stop(); }
        if (_this->running) { _this->syncVfo(); }
    }

    // -------------------------------------------------------------- quadros
    void onFrame(const aprs::Frame& f) {
        aprs::Report r;
        aprs::parse(f, r);
        double now = wallSec();
        std::string raw = f.tnc2();
        std::lock_guard<std::mutex> lck(mtx);
        packets++;
        if (logToFile) {
            std::ofstream o(std::string(core::getRoot()) + "/aprs.log", std::ios::app);
            if (o) { o << timeText(now) << " " << raw << "\n"; }
        }
        if (r.type == aprs::Report::MESSAGE) {
            // as estações reenviam até receber "ack": mostra uma vez só
            bool dup = false;
            for (auto& m : msgs) {
                if (now - m.time < 1800 && m.from == f.src && m.to == r.msgTo && m.text == r.msgText) { dup = true; }
            }
            if (!dup && r.msgText.rfind("ack", 0) != 0 && r.msgText.rfind("rej", 0) != 0) {
                msgs.push_front({ now, f.src, r.msgTo, r.msgText });
                while (msgs.size() > 50) { msgs.pop_back(); }
            }
        }
        bool obj = r.type == aprs::Report::OBJECT || r.type == aprs::Report::ITEM;
        std::string key = obj ? ";" + r.name : f.src;
        if (obj && r.killed) {
            stations.erase(key);
            return;
        }
        Station& s = stations[key];
        if (s.packets == 0) {
            s.first = now;
            s.key = key;
            s.call = obj ? r.name : f.src;
            s.object = obj;
        }
        s.packets++;
        s.last = now;
        s.direct = f.direct;
        s.path.clear();
        for (auto& p : f.path) { s.path += (s.path.empty() ? "" : ",") + p; }
        s.raw = raw;
        if (r.type == aprs::Report::STATUS) { s.status = r.comment; }
        if (r.hasPos) {
            s.rep = r;
            s.hasPos = true;
            s.lastPos = now;
            if (s.track.empty() || std::fabs(s.track.back().first - r.lat) > 1e-5 || std::fabs(s.track.back().second - r.lon) > 1e-5) {
                s.track.push_back({ r.lat, r.lon });
                while (s.track.size() > 60) { s.track.pop_front(); }
            }
        }
        else if (!s.hasPos) {
            s.rep = r;
        }
        char b[160];
        snprintf(b, sizeof(b), "%s · %s", s.call.c_str(), aprs::typeName(r.type));
        lastSummary = b;
        expire(now);
    }

    void expire(double now) {
        for (auto it = stations.begin(); it != stations.end();) {
            if (now - it->second.last > STATION_TTL) { it = stations.erase(it); }
            else { ++it; }
        }
    }

    // Posição da estação de recepção: locator do operador ou a posição do ADS-B
    bool receiver(double& lat, double& lon) {
        std::string grid = sigpath::iqFrontEnd.operatorLocation;
        if (grid.size() >= 4) {
            auto ll = utils::gridToLatLng(grid);
            if (ll.isValid() && !(ll.lat == 0 && ll.lon == 0)) {
                lat = ll.lat;
                lon = ll.lon;
                return true;
            }
        }
        return false;
    }

    std::string stationsJson(bool forMap) {
        double rlat = 0, rlon = 0;
        bool rx = receiver(rlat, rlon);
        double now = wallSec();
        std::lock_guard<std::mutex> lck(mtx);
        json a = json::array();
        for (auto& [k, s] : stations) {
            if (forMap && !s.hasPos) { continue; }
            json j;
            j["call"] = jsonSafe(s.call);
            j["object"] = s.object;
            j["age"] = now - s.last;
            j["packets"] = s.packets;
            j["direct"] = s.direct;
            j["path"] = jsonSafe(s.path);
            j["sym"] = std::string(1, s.rep.symTable) + std::string(1, s.rep.symCode);
            j["sym_name"] = aprs::symbolName(s.rep.symTable, s.rep.symCode);
            j["comment"] = jsonSafe(s.rep.comment);
            j["status"] = jsonSafe(s.status);
            j["raw"] = jsonSafe(s.raw);
            if (s.hasPos) {
                j["lat"] = s.rep.lat;
                j["lon"] = s.rep.lon;
                if (rx) { j["dist_km"] = distKm(rlat, rlon, s.rep.lat, s.rep.lon); }
                if (s.rep.speedKmh >= 0) { j["speed_kmh"] = s.rep.speedKmh; }
                if (s.rep.course >= 0) { j["course"] = s.rep.course; }
                if (s.rep.altM > -1e8) { j["alt_m"] = s.rep.altM; }
                json t = json::array();
                for (auto& p : s.track) { t.push_back({ p.first, p.second }); }
                j["track"] = t;
            }
            a.push_back(j);
        }
        return a.dump();
    }

    void saveConfig() {
        config.acquire();
        config.conf["freq"] = freq;
        config.conf["logToFile"] = logToFile;
        config.release(true);
    }

    // -------------------------------------------------------------- painel
    static ModuleManager::Instance* adsb() {
        for (auto& [n, inst] : core::moduleManager.instances) {
            if (inst.module.info && inst.instance && std::string(inst.module.info->name) == "adsb_decoder") { return inst.instance; }
        }
        return NULL;
    }

    static void menuHandler(void* ctx) {
        APRSModule* _this = (APRSModule*)ctx;
        float w = ImGui::GetContentRegionAvail().x;
        ImVec4 dim = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
        ImVec4 green(0.27f, 0.82f, 0.47f, 1.0f), amber(0.96f, 0.69f, 0.25f, 1.0f);
        if (!_this->enabled) { style::beginDisabled(); }

        // liga/desliga + estado (linhas fixas: o painel não salta)
        bool run = _this->running;
        if (ImGui::Button(run ? "Parar APRS##aprs_run" : "Ouvir APRS##aprs_run", ImVec2(w * 0.5f - 4, 0))) { _this->pending = run ? 2 : 1; }
        ImGui::SameLine();
        if (ImGui::Button("Abrir mapa##aprs_map", ImVec2(w * 0.5f - 4, 0))) {
            if (auto a = adsb()) {
                json j = json::parse(a->handleDebugCommand("start_map_server", ""), nullptr, false);
                int port = (!j.is_discarded() && j.contains("port")) ? j["port"].get<int>() : 8091;
                openUrl("http://127.0.0.1:" + std::to_string(port) + "/");
            }
        }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Abre o mapa no navegador (o mesmo do ADS-B), com as estações APRS em laranja."); }
        size_t nst;
        uint64_t npk;
        {
            std::lock_guard<std::mutex> lck(_this->mtx);
            nst = _this->stations.size();
            npk = _this->packets;
        }
        if (!run) { ImGui::TextColored(dim, "Parado · %.3f MHz", _this->freq / 1e6); }
        else if (!_this->inRange) { ImGui::TextColored(amber, "%.3f MHz fora da faixa recebida", _this->freq / 1e6); }
        else { ImGui::TextColored(green, "Ouvindo %.3f MHz · %d estações", _this->freq / 1e6, (int)nst); }
        if (run && _this->inRange && ImGui::IsItemHovered()) { ImGui::SetTooltip("%d pacotes recebidos", (int)npk); }
        if (run && !_this->inRange) {
            if (ImGui::Button("Trazer a faixa para o APRS##aprs_tune", ImVec2(w, 0))) { tuner::tune(tuner::TUNER_MODE_CENTER, "", _this->freq); }
        }

        // estações
        _this->drawStations(w);

        // mensagens
        {
            std::lock_guard<std::mutex> lck(_this->mtx);
            if (!_this->msgs.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(dim, "MENSAGENS");
                int n = 0;
                for (auto& m : _this->msgs) {
                    if (n++ >= 5) { break; }
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextColored(dim, "%s", timeText(m.time).c_str());
                    ImGui::SameLine();
                    ImGui::Text("%s > %s: %s", m.from.c_str(), m.to.c_str(), m.text.c_str());
                    ImGui::PopTextWrapPos();
                }
            }
        }

        if (ImGui::TreeNode("Ajustes##aprs_adj")) {
            ImGui::LeftLabel("Frequência");
            ImGui::FillWidth();
            if (ImGui::InputDouble("##aprs_freq", &_this->freqMHz, 0.005, 0.025, "%.3f MHz")) {}
            if (ImGui::IsItemDeactivatedAfterEdit() && _this->freqMHz > 24 && _this->freqMHz < 1800) {
                _this->freq = std::round(_this->freqMHz * 1e6 / 100.0) * 100.0;
                _this->saveConfig();
            }
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Brasil e Américas: 145,570 (Brasil) / 144,390 (EUA) · Europa: 144,800 · ISS: 145,825"); }
            if (ImGui::Checkbox("Gravar pacotes em aprs.log##aprs_log", &_this->logToFile)) { _this->saveConfig(); }
            ImGui::TreePop();
        }
        if (!_this->enabled) { style::endDisabled(); }
    }

    void drawStations(float w) {
        double rlat = 0, rlon = 0;
        bool rx = receiver(rlat, rlon);
        double now = wallSec();
        std::vector<const Station*> list;
        std::lock_guard<std::mutex> lck(mtx);
        for (auto& [k, s] : stations) { list.push_back(&s); }
        std::sort(list.begin(), list.end(), [](const Station* a, const Station* b) { return a->last > b->last; });
        if (list.empty()) {
            ImGui::TextColored(ImGui::GetStyle().Colors[ImGuiCol_TextDisabled], "Nenhuma estação ouvida ainda");
            return;
        }
        float rows = (float)(std::min)((int)list.size(), 8);
        ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##aprs_st", 3, fl, ImVec2(w, (rows + 1.3f) * ImGui::GetTextLineHeightWithSpacing()))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Estação", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn(rx ? "km" : "Tipo", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("Há", ImGuiTableColumnFlags_WidthStretch, 0.6f);
            ImGui::TableHeadersRow();
            for (const Station* s : list) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                bool sel = selected == s->key;
                std::string lbl = s->call + (s->object ? " (obj)" : "") + "##st_" + s->key;
                if (ImGui::Selectable(lbl.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) { selected = sel ? "" : s->key; }
                ImGui::TableSetColumnIndex(1);
                if (rx && s->hasPos) { ImGui::Text("%.0f", distKm(rlat, rlon, s->rep.lat, s->rep.lon)); }
                else {
                    const char* sn = aprs::symbolName(s->rep.symTable, s->rep.symCode);
                    ImGui::TextUnformatted(s->hasPos ? (sn[0] ? sn : "posição") : "sem posição");
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(agoText(now - s->last).c_str());
            }
            ImGui::EndTable();
        }
        // detalhes da selecionada
        auto it = stations.find(selected);
        if (it == stations.end()) { return; }
        const Station& s = it->second;
        ImGui::PushTextWrapPos(0);
        ImGui::Text("%s", s.call.c_str());
        const char* sn = aprs::symbolName(s.rep.symTable, s.rep.symCode);
        if (sn[0]) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", sn);
        }
        if (s.hasPos) {
            ImGui::Text("Posição %.5f, %.5f", s.rep.lat, s.rep.lon);
            std::string mv;
            if (s.rep.speedKmh >= 0) { mv += std::to_string(s.rep.speedKmh) + " km/h"; }
            if (s.rep.course >= 0 && s.rep.speedKmh > 0) { mv += " · rumo " + std::to_string(s.rep.course) + "°"; }
            if (s.rep.altM > -1e8) { mv += (mv.empty() ? "" : " · ") + std::to_string((int)std::lround(s.rep.altM)) + " m"; }
            if (!mv.empty()) { ImGui::TextUnformatted(mv.c_str()); }
        }
        if (!s.rep.comment.empty()) { ImGui::Text("Comentário: %s", s.rep.comment.c_str()); }
        if (!s.status.empty()) { ImGui::Text("Status: %s", s.status.c_str()); }
        ImGui::TextDisabled("%s · %d pacotes · %s", s.direct ? "ouvido direto" : ("via " + s.path).c_str(), s.packets, timeText(s.last).c_str());
        ImGui::PopTextWrapPos();
    }

    std::string name;
    bool enabled = true;
    std::atomic<bool> running { false };
    std::atomic<bool> inRange { false };
    std::atomic<int> pending { 0 };
    double freq = DEFAULT_FREQ;
    double freqMHz = DEFAULT_FREQ / 1e6;
    bool logToFile = true;

    VFOManager::VFO* vfo = NULL;
    dsp::sink::Handler<dsp::complex_t> sink;
    bool sinkReady = false;
    dsp::complex_t prev = { 1.0f, 0.0f };
    std::vector<float> audio;
    std::mutex demodMtx;
    aprs::Demodulator demod;
    EventHandler<ImGuiContext*> frameHandler;

    std::mutex mtx;
    std::map<std::string, Station> stations;
    std::deque<Msg> msgs;
    uint64_t packets = 0;
    std::string lastSummary;
    std::string selected;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["freq"] = DEFAULT_FREQ;
    def["logToFile"] = true;
    config.setPath(std::string(core::getRoot()) + "/aprs_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new APRSModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (APRSModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
