// Módulo ADS-B (1090 MHz) para SDR++: decodifica Mode S/ADS-B, mostra um radar
// no painel e serve um mapa (OpenStreetMap) no navegador, inclusive para o celular.
#include <utils/net.h>
#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <gui/tuner.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <dsp/sink/handler_sink.h>
#include <utils/flog.h>
#include <utils/cty.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <cmath>
#include <algorithm>
#include "modes.h"
#include "adsb_web_page.h"

#ifdef _WIN32
#include <shellapi.h>
#endif
#ifdef __ANDROID__
namespace backend { void openUrl(const std::string& url); }   // core/backends/android
#endif

SDRPP_MOD_INFO{
    /* Name:            */ "adsb_decoder",
    /* Description:     */ "ADS-B (1090 MHz) com radar e mapa",
    /* Author:          */ "SDR++Brown simples",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

static const double ADSB_FREQ = 1090e6;
static const double ADSB_SR = 2.4e6;   // taxa nativa do RTL-SDR: sem reamostragem

static double nowSec() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

static void openUrl(const std::string& url) {
#if defined(__ANDROID__)
    backend::openUrl(url);
#elif defined(_WIN32)
    ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open '" + url + "' >/dev/null 2>&1 &";
    if (system(cmd.c_str()) != 0) { flog::warn("ADS-B: não consegui abrir o navegador"); }
#else
    std::string cmd = "xdg-open '" + url + "' >/dev/null 2>&1 &";
    if (system(cmd.c_str()) != 0) { flog::warn("ADS-B: não consegui abrir o navegador"); }
#endif
}

// ------------------------------------------------------------------ servidor HTTP mínimo
class MapServer {
public:
    std::function<std::string()> getJson;

    bool start(bool lan, int port) {
        stop();
        try {
            listener = net::listen(lan ? "0.0.0.0" : "127.0.0.1", port);
        }
        catch (const std::exception& e) {
            error = std::string("porta ") + std::to_string(port) + " indisponível";
            flog::error("ADS-B: falha ao abrir servidor do mapa: {}", e.what());
            return false;
        }
        error.clear();
        this->port = port;
        this->lan = lan;
        running = true;
        worker = std::thread(&MapServer::loop, this);
        return true;
    }

    void stop() {
        if (!running) { return; }
        running = false;
        if (listener) { listener->stop(); }
        if (worker.joinable()) { worker.join(); }
        listener.reset();
    }

    ~MapServer() { stop(); }

    bool isRunning() const { return running; }
    int port = 8091;
    bool lan = false;
    std::string error;

private:
    std::shared_ptr<net::Listener> listener;
    std::thread worker;
    std::atomic<bool> running { false };

    void loop() {
        while (running) {
            std::shared_ptr<net::Socket> sock;
            try { sock = listener->accept(); }
            catch (...) { break; }
            if (!sock) { break; }
            try { serve(sock); }
            catch (const std::exception& e) { flog::warn("ADS-B: erro HTTP: {}", e.what()); }
            sock->close();
        }
    }

    void serve(std::shared_ptr<net::Socket> sock) {
        std::string line;
        if (sock->recvline(line, 4096, 3000) <= 0) { return; }
        // Descarta os cabeçalhos
        std::string h;
        for (int i = 0; i < 100; i++) {
            if (sock->recvline(h, 8192, 3000) <= 0) { break; }
            if (h == "\r" || h.empty()) { break; }
        }
        std::string path = "/";
        size_t a = line.find(' ');
        size_t b = (a == std::string::npos) ? a : line.find(' ', a + 1);
        if (a != std::string::npos && b != std::string::npos) { path = line.substr(a + 1, b - a - 1); }
        size_t q = path.find('?');
        if (q != std::string::npos) { path = path.substr(0, q); }

        std::string body, type, status = "200 OK";
        if (path == "/" || path == "/index.html") {
            body = ADSB_WEB_PAGE;
            type = "text/html; charset=utf-8";
        }
        else if (path == "/data/aircraft.json") {
            body = getJson ? getJson() : "{}";
            type = "application/json";
        }
        else {
            status = "404 Not Found";
            body = "nao encontrado";
            type = "text/plain";
        }
        std::string resp = "HTTP/1.1 " + status + "\r\nContent-Type: " + type +
                           "\r\nContent-Length: " + std::to_string(body.size()) +
                           "\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
        sock->sendstr(resp);
        sock->send((const uint8_t*)body.data(), body.size());
    }
};

// ------------------------------------------------------------------ módulo
class ADSBModule : public ModuleManager::Instance {
public:
    ADSBModule(std::string name) {
        this->name = name;

        config.acquire();
        useGrid = config.conf.value("useGrid", true);
        manualLat = config.conf.value("lat", 0.0);
        manualLon = config.conf.value("lon", 0.0);
        port = config.conf.value("port", 8091);
        allowLan = config.conf.value("allowLan", false);
        radarRangeKm = config.conf.value("radarRangeKm", 200);
        config.release();

        demod.onMessage = [this](const modes::Message& m) { db.handle(m, nowSec()); };
        demod.isKnownIcao = [this](uint32_t icao) { return db.isKnown(icao); };
        server.getJson = [this]() {
            std::string j = db.toJson(nowSec(), true);
            return std::string("{\"running\":") + (running ? "true" : "false") + "," + j.substr(1);
        };

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~ADSBModule() {
        stopAdsb();
        server.stop();
        gui::menu.removeEntry(name);
    }

    void postInit() {}

    void enable() { enabled = true; }

    void disable() {
        stopAdsb();
        server.stop();
        enabled = false;
    }

    bool isEnabled() { return enabled; }

    std::string handleDebugCommand(const std::string& cmd, const std::string& args) override {
        if (cmd == "start") { startAdsb(); return "{\"status\":\"ok\"}"; }
        if (cmd == "stop") { stopAdsb(); return "{\"status\":\"ok\"}"; }
        if (cmd == "get_aircraft") { return server.getJson(); }
        if (cmd == "start_map_server") {
            bool ok = ensureServer();
            return std::string("{\"status\":\"") + (ok ? "ok" : "error") + "\",\"port\":" + std::to_string(server.port) + "}";
        }
        if (cmd == "get_stats") {
            return "{\"preambles\":" + std::to_string(demod.preambles) + ",\"accepted\":" + std::to_string(demod.accepted) +
                   ",\"rejected\":" + std::to_string(demod.rejected) + ",\"running\":" + (running ? "true" : "false") + "}";
        }
        return "{}";
    }

private:
    // -------------------------------------------------------------- liga/desliga
    void stopScannerIfRunning() {
        for (auto& [n, inst] : core::moduleManager.instances) {
            if (inst.module.info && std::string(inst.module.info->name) == "frequency_manager" && inst.instance) {
                inst.instance->handleDebugCommand("stop_scanner", "");
            }
        }
    }

    void startAdsb() {
        if (running || !enabled) { return; }
        stopScannerIfRunning();   // um RTL-SDR só: o scanner da banda aérea não pode mexer na sintonia
        prevCenter = gui::waterfall.getCenterFrequency();
        // Guarda a frequência do rádio (não só o centro da tela) para voltar exatamente nela
        prevRadioFreq = sigpath::vfoManager.vfoExists("Radio") ? prevCenter + sigpath::vfoManager.getOffset("Radio") : 0;
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, ADSB_SR, ADSB_SR, ADSB_SR, ADSB_SR, true);
        if (!vfo) { return; }
        demod.reset();
        sink.init(vfo->output, dataHandler, this);
        sink.start();
        tuner::tune(tuner::TUNER_MODE_CENTER, name, ADSB_FREQ);
        running = true;
        ensureServer();
        flog::info("ADS-B: iniciado em 1090 MHz");
    }

    void stopAdsb() {
        if (!running) { return; }
        running = false;
        sink.stop();
        sigpath::vfoManager.deleteVFO(vfo);
        vfo = NULL;
        if (prevRadioFreq > 0 && sigpath::vfoManager.vfoExists("Radio")) { tuner::tune(tuner::TUNER_MODE_NORMAL, "Radio", prevRadioFreq); }
        else if (prevCenter > 0) { tuner::tune(tuner::TUNER_MODE_CENTER, "", prevCenter); }
        flog::info("ADS-B: parado");
    }

    bool ensureServer() {
        if (server.isRunning() && server.lan == allowLan && server.port == port) { return true; }
        return server.start(allowLan, port);
    }

    static void dataHandler(dsp::complex_t* data, int count, void* ctx) {
        ADSBModule* _this = (ADSBModule*)ctx;
        if ((int)_this->mag.size() < count) { _this->mag.resize(count); }
        float* m = _this->mag.data();
        for (int i = 0; i < count; i++) {
            m[i] = sqrtf(data[i].re * data[i].re + data[i].im * data[i].im);
        }
        _this->demod.process(m, count);
        double t = nowSec();
        if (t - _this->lastExpire > 1.0) {
            _this->db.expire(t, 60.0);
            _this->lastExpire = t;
        }
    }

    // -------------------------------------------------------------- posição da estação
    void updateReceiver() {
        bool valid = false;
        double lat = 0, lon = 0;
        if (useGrid) {
            std::string grid = sigpath::iqFrontEnd.operatorLocation;
            if (grid.size() >= 4) {
                auto ll = utils::gridToLatLng(grid);
                if (ll.isValid() && !(ll.lat == 0 && ll.lon == 0)) { lat = ll.lat; lon = ll.lon; valid = true; }
            }
        }
        else if (!(manualLat == 0 && manualLon == 0)) {
            lat = manualLat;
            lon = manualLon;
            valid = true;
        }
        db.setReceiver(lat, lon, valid);
    }

    void saveConfig() {
        config.acquire();
        config.conf["useGrid"] = useGrid;
        config.conf["lat"] = manualLat;
        config.conf["lon"] = manualLon;
        config.conf["port"] = port;
        config.conf["allowLan"] = allowLan;
        config.conf["radarRangeKm"] = radarRangeKm;
        config.release(true);
    }

    // -------------------------------------------------------------- interface
    static ImU32 altColor(const modes::Aircraft& a, float alpha = 1.0f) {
        if (a.squawk == 7500 || a.squawk == 7600 || a.squawk == 7700) { return IM_COL32(255, 60, 60, (int)(255 * alpha)); }
        if (!a.hasAltitude) { return IM_COL32(160, 170, 180, (int)(255 * alpha)); }
        float t = std::clamp(a.altitude / 40000.0f, 0.0f, 1.0f);
        ImVec4 c = (ImVec4)ImColor::HSV((30.0f + t * 270.0f) / 360.0f, 0.85f, 0.95f);
        c.w = alpha;
        return ImGui::ColorConvertFloat4ToU32(c);
    }

    static std::string label(const modes::Aircraft& a) {
        if (!a.callsign.empty()) { return a.callsign; }
        char b[8];
        snprintf(b, sizeof(b), "%06X", a.icao);
        return b;
    }

    void drawRadar(const std::vector<modes::Aircraft>& list, float size) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##adsb_radar", ImVec2(size, size));
        bool hovered = ImGui::IsItemHovered();
        ImVec2 mouse = ImGui::GetIO().MousePos;

        ImVec2 c(p0.x + size / 2, p0.y + size / 2);
        float R = size / 2 - 6;
        dl->AddRectFilled(p0, ImVec2(p0.x + size, p0.y + size), IM_COL32(8, 14, 22, 255), 6.0f);
        ImU32 ringCol = IM_COL32(60, 120, 170, 110);
        for (int k = 1; k <= 4; k++) {
            dl->AddCircle(c, R * k / 4.0f, ringCol, 64, 1.0f);
            char t[16];
            snprintf(t, sizeof(t), "%d km", radarRangeKm * k / 4);
            dl->AddText(ImVec2(c.x + 3, c.y - R * k / 4.0f), IM_COL32(90, 150, 200, 170), t);
        }
        dl->AddLine(ImVec2(c.x - R, c.y), ImVec2(c.x + R, c.y), IM_COL32(60, 120, 170, 60));
        dl->AddLine(ImVec2(c.x, c.y - R), ImVec2(c.x, c.y + R), IM_COL32(60, 120, 170, 60));
        dl->AddText(ImVec2(c.x - 4, p0.y + 2), IM_COL32(200, 220, 240, 220), "N");

        // Centro: a estação, ou a média dos aviões se a posição não foi definida
        double lat0 = db.receiverLat(), lon0 = db.receiverLon();
        bool haveCenter = db.receiverValid();
        if (!haveCenter) {
            int n = 0;
            double sl = 0, so = 0;
            for (auto& a : list) { if (a.hasPosition) { sl += a.lat; so += a.lon; n++; } }
            if (n) { lat0 = sl / n; lon0 = so / n; haveCenter = true; }
        }
        if (db.receiverValid()) { dl->AddCircleFilled(c, 4, IM_COL32(80, 180, 255, 255)); }
        if (!haveCenter) {
            ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + 10, c.y - 8), IM_COL32(150, 170, 190, 255), "Nenhum avião com posição ainda");
            return;
        }

        const double kmPerDegLat = 110.57;
        double kmPerDegLon = 111.32 * cos(lat0 * 3.14159265358979 / 180.0);
        const modes::Aircraft* hover = nullptr;
        float bestD = 10.0f;
        dl->PushClipRect(p0, ImVec2(p0.x + size, p0.y + size), true);
        for (auto& a : list) {
            if (!a.hasPosition) { continue; }
            double dx = (a.lon - lon0) * kmPerDegLon, dy = (a.lat - lat0) * kmPerDegLat;
            if (sqrt(dx * dx + dy * dy) > radarRangeKm * 1.05) { continue; }
            ImVec2 p(c.x + (float)(dx / radarRangeKm * R), c.y - (float)(dy / radarRangeKm * R));
            ImU32 col = altColor(a);

            // Rastro
            ImVec2 prev;
            bool hasPrev = false;
            for (auto& tp : a.trail) {
                double tx = (tp.lon - lon0) * kmPerDegLon, ty = (tp.lat - lat0) * kmPerDegLat;
                ImVec2 q(c.x + (float)(tx / radarRangeKm * R), c.y - (float)(ty / radarRangeKm * R));
                if (hasPrev) { dl->AddLine(prev, q, altColor(a, 0.35f), 1.0f); }
                prev = q;
                hasPrev = true;
            }

            // Avião: triângulo apontando para o rumo
            float ang = (float)(a.track * 3.14159265358979 / 180.0);
            auto rot = [&](float x, float y) {
                return ImVec2(p.x + x * cosf(ang) - y * sinf(ang), p.y + x * sinf(ang) + y * cosf(ang));
            };
            dl->AddTriangleFilled(rot(0, -7), rot(4.5f, 5), rot(-4.5f, 5), col);
            dl->AddText(ImVec2(p.x + 7, p.y - 7), IM_COL32(220, 230, 240, 230), label(a).c_str());

            float d = sqrtf((mouse.x - p.x) * (mouse.x - p.x) + (mouse.y - p.y) * (mouse.y - p.y));
            if (hovered && d < bestD) { bestD = d; hover = &a; }
        }
        dl->PopClipRect();

        if (hover) {
            ImGui::BeginTooltip();
            ImGui::Text("%s  (%06X)", label(*hover).c_str(), hover->icao);
            if (hover->hasAltitude) { ImGui::Text("Altitude: %d ft", hover->altitude); }
            if (hover->hasVelocity) { ImGui::Text("Velocidade: %.0f kt  rumo %.0f°", hover->groundSpeed, hover->track); }
            if (hover->hasVerticalRate) { ImGui::Text("Subida: %+d ft/min", hover->verticalRate); }
            if (hover->squawk >= 0) { ImGui::Text("Squawk: %04d", hover->squawk); }
            if (db.receiverValid()) {
                ImGui::Text("Distância: %.1f km", modes::distanceKm(db.receiverLat(), db.receiverLon(), hover->lat, hover->lon));
            }
            ImGui::EndTooltip();
        }
    }

    static void menuHandler(void* ctx) {
        ADSBModule* _this = (ADSBModule*)ctx;
        float w = ImGui::GetContentRegionAvail().x;
        _this->updateReceiver();

        if (!_this->enabled) { style::beginDisabled(); }

        // Liga / desliga
        if (!_this->running) {
            if (ImGui::Button("Iniciar ADS-B (1090 MHz)##adsb_start", ImVec2(w, 0))) { _this->startAdsb(); }
            ImGui::TextWrapped("O RTL-SDR fica todo em 1090 MHz: banda aérea e scanner param enquanto o ADS-B estiver ligado.");
        }
        else {
            char lbl[96];
            snprintf(lbl, sizeof(lbl), "Parar ADS-B e voltar para %.3f MHz##adsb_stop", (_this->prevRadioFreq > 0 ? _this->prevRadioFreq : _this->prevCenter) / 1e6);
            if (ImGui::Button(lbl, ImVec2(w, 0))) { _this->stopAdsb(); }
        }

        // Estatísticas
        auto list = _this->db.snapshot();
        double t = nowSec();
        if (t - _this->rateTime >= 1.0) {
            uint64_t total = _this->db.totalMessages;
            _this->msgRate = (float)((total - _this->rateLastTotal) / (t - _this->rateTime));
            _this->rateLastTotal = total;
            _this->rateTime = t;
        }
        int withPos = 0;
        for (auto& a : list) { if (a.hasPosition) { withPos++; } }
        if (_this->running) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%d aviões  |  %d com posição  |  %.0f msg/s",
                               (int)list.size(), withPos, _this->msgRate);
            if (gui::waterfall.getBandwidth() < 2.0e6) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Aumente o sample rate do RTL-SDR para 2,4 MHz");
            }
        }

        // Mapa
        if (ImGui::Button("Abrir mapa no navegador##adsb_map", ImVec2(w, 0))) {
            if (_this->ensureServer()) {
                openUrl("http://127.0.0.1:" + std::to_string(_this->server.port) + "/");
            }
        }
        if (!_this->server.error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Mapa: %s", _this->server.error.c_str());
        }
        if (ImGui::Checkbox("Abrir também no celular (mesma rede)##adsb_lan", &_this->allowLan)) {
            _this->saveConfig();
            if (_this->server.isRunning()) { _this->ensureServer(); }
        }
        if (_this->allowLan) {
            if (!_this->server.isRunning()) { _this->ensureServer(); }
            for (auto& [ifname, info] : _this->interfaces()) {
                ImGui::TextDisabled("  http://%s:%d", ifname.c_str(), _this->server.port);
            }
        }

        // Radar
        ImGui::LeftLabel("Alcance do radar");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        const int ranges[] = { 25, 50, 100, 200, 300, 400 };
        int sel = 3;
        for (int i = 0; i < 6; i++) { if (ranges[i] == _this->radarRangeKm) { sel = i; } }
        if (ImGui::Combo("##adsb_range", &sel, "25 km\00050 km\000100 km\000200 km\000300 km\000400 km\000")) {
            _this->radarRangeKm = ranges[sel];
            _this->saveConfig();
        }
        _this->drawRadar(list, w);

        // Lista
        std::sort(list.begin(), list.end(), [&](const modes::Aircraft& x, const modes::Aircraft& y) {
            if (x.hasPosition != y.hasPosition) { return x.hasPosition; }
            if (x.hasPosition && _this->db.receiverValid()) {
                return modes::distanceKm(_this->db.receiverLat(), _this->db.receiverLon(), x.lat, x.lon) <
                       modes::distanceKm(_this->db.receiverLat(), _this->db.receiverLon(), y.lat, y.lon);
            }
            return x.lastSeen > y.lastSeen;
        });
        if (!list.empty()) {
            ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
            float h = (std::min<float>)(200.0f * style::uiScale, (list.size() + 1.5f) * ImGui::GetTextLineHeightWithSpacing());
            if (ImGui::BeginTable("##adsb_tbl", 4, flags, ImVec2(0, h))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Voo", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Alt ft");
                ImGui::TableSetupColumn("kt");
                ImGui::TableSetupColumn("km");
                ImGui::TableHeadersRow();
                for (auto& a : list) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (a.hasPosition) { ImGui::TextUnformatted(label(a).c_str()); }
                    else { ImGui::TextDisabled("%s", label(a).c_str()); }
                    ImGui::TableSetColumnIndex(1);
                    if (a.hasAltitude) { ImGui::Text("%d", a.altitude); } else { ImGui::TextDisabled("-"); }
                    ImGui::TableSetColumnIndex(2);
                    if (a.hasVelocity) { ImGui::Text("%.0f", a.groundSpeed); } else { ImGui::TextDisabled("-"); }
                    ImGui::TableSetColumnIndex(3);
                    if (a.hasPosition && _this->db.receiverValid()) {
                        ImGui::Text("%.0f", modes::distanceKm(_this->db.receiverLat(), _this->db.receiverLon(), a.lat, a.lon));
                    }
                    else { ImGui::TextDisabled("-"); }
                }
                ImGui::EndTable();
            }
        }

        // Posição da estação
        if (ImGui::TreeNode("Sua posição##adsb_pos")) {
            std::string grid = sigpath::iqFrontEnd.operatorLocation;
            char lbl[96];
            snprintf(lbl, sizeof(lbl), "Usar o \"My Grid\" da fonte (%s)##adsb_grid", grid.empty() ? "vazio" : grid.c_str());
            if (ImGui::Checkbox(lbl, &_this->useGrid)) { _this->saveConfig(); }
            if (!_this->useGrid) {
                ImGui::LeftLabel("Latitude");
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                if (ImGui::InputDouble("##adsb_lat", &_this->manualLat, 0.0, 0.0, "%.5f")) {
                    _this->manualLat = std::clamp(_this->manualLat, -90.0, 90.0);
                    _this->saveConfig();
                }
                ImGui::LeftLabel("Longitude");
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                if (ImGui::InputDouble("##adsb_lon", &_this->manualLon, 0.0, 0.0, "%.5f")) {
                    _this->manualLon = std::clamp(_this->manualLon, -180.0, 180.0);
                    _this->saveConfig();
                }
            }
            if (!_this->db.receiverValid()) {
                ImGui::TextWrapped("Sem posição: o radar centraliza nos aviões e a distância não aparece.");
            }
            ImGui::TreePop();
        }

        if (!_this->enabled) { style::endDisabled(); }
    }

    std::vector<std::pair<std::string, int>> interfaces() {
        double t = nowSec();
        if (t - ifTime > 10.0) {
            ifCache.clear();
            try {
                for (auto& [n, info] : net::listInterfaces()) {
                    uint32_t ip = info.address;
                    if ((ip >> 24) == 127 || ip == 0) { continue; }
                    char b[32];
                    snprintf(b, sizeof(b), "%u.%u.%u.%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
                    ifCache.push_back({ b, 0 });
                }
            }
            catch (...) {}
            ifTime = t;
        }
        return ifCache;
    }

    std::string name;
    bool enabled = true;
    std::atomic<bool> running { false };

    VFOManager::VFO* vfo = NULL;
    dsp::sink::Handler<dsp::complex_t> sink;
    std::vector<float> mag;
    modes::Demodulator demod { ADSB_SR };
    modes::AircraftDB db;
    MapServer server;
    double lastExpire = 0;
    double prevCenter = 0;
    double prevRadioFreq = 0;

    // Config
    bool useGrid = true;
    double manualLat = 0, manualLon = 0;
    int port = 8091;
    bool allowLan = false;
    int radarRangeKm = 200;

    // Estatística
    float msgRate = 0;
    uint64_t rateLastTotal = 0;
    double rateTime = 0;
    std::vector<std::pair<std::string, int>> ifCache;
    double ifTime = -100;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["useGrid"] = true;
    def["lat"] = 0.0;
    def["lon"] = 0.0;
    def["port"] = 8091;
    def["allowLan"] = false;
    def["radarRangeKm"] = 200;
    config.setPath(std::string(core::getRoot()) + "/adsb_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new ADSBModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (ADSBModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
