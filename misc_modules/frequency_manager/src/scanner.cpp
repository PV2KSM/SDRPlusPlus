#include "scanner.h"
#include "frequency_manager.h"  // For FrequencyManagerModule definition
#include <utils/flog.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <core.h>
#include <signal_path/signal_path.h>
#include <algorithm>
#include <ctime>
#include <fstream>
#include <filesystem>
#include "../../radio/src/radio_module_interface.h"

// IDs dos modos DSD registrados pelo ch_extravhf_decoder
static const int SCANNER_DEMOD_DSD = 0x1301;
static const int SCANNER_DEMOD_OLDDSD = 0x1302;

static const size_t MAX_LOG_ENTRIES = 200;
static const float SEEK_SETTLE_MS = 40.0f;   // descarta medições logo após sintonizar
static const float DSD_SETTLE_MS = 150.0f;   // idem para o sync digital
static const float DSD_STARTUP_MS = 600.0f;  // tempo para o demodulador DSD ficar disponível

static std::string nowString(const char* fmt) {
    std::time_t t = std::time(nullptr);
    std::tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), fmt, &tmv);
    return buf;
}

Scanner::Scanner(FrequencyManagerModule* module) : module(module) {
    snrMeterHandler.ctx = this;
    snrMeterHandler.handler = [](ImGui::SNRMeterExtPoint point, void* ctx) {
        ((Scanner*)ctx)->onSNRMeterExtPoint(point);
    };
    ImGui::onSNRMeterExtPoint.bindHandler(&snrMeterHandler);

    playStateHandler.ctx = this;
    playStateHandler.handler = [](bool playing, void* ctx) {
        ((Scanner*)ctx)->onPlayStateChange(playing);
    };
    gui::mainWindow.onPlayStateChange.bindHandler(&playStateHandler);

    auto& config = getFrequencyManagerConfig();
    config.acquire();
    if (!config.conf.contains("scanner") || !config.conf["scanner"].is_object()) {
        config.conf["scanner"] = json::object();
    }
    json& s = config.conf["scanner"];
    scanIntervalMs = s.value("scanIntervalMs", scanIntervalMs);
    dmrDwellMs = s.value("dmrDwellMs", dmrDwellMs);
    hangTimeSec = s.value("hangTimeSec", hangTimeSec);
    // Antes era tempo fixo em cada canal (10 s). Agora é só um limite opcional.
    listenTimeSec = s.value("maxListenSec", 0.0f);
    noiseFloor = s.value("noiseFloor", noiseFloor);
    // Configs antigas vinham com -120 (nível absoluto), mas o scanner compara
    // SNR: com -120 qualquer canal parecia ter sinal e ele nunca saía do lugar.
    if (noiseFloor < 0.0f || noiseFloor > 40.0f) { noiseFloor = 3.0f; }
    signalMarginDb = s.value("signalMarginDb", signalMarginDb);
    squelchEnabled = s.value("squelchEnabled", squelchEnabled);
    priorityIntervalSec = s.value("priorityIntervalSec", priorityIntervalSec);
    logToFile = s.value("logToFile", logToFile);
    config.release();
}

Scanner::~Scanner() {
    stop();
    ImGui::onSNRMeterExtPoint.unbindHandler(&snrMeterHandler);
    gui::mainWindow.onPlayStateChange.unbindHandler(&playStateHandler);
}

// ---------------------------------------------------------------- persistência

void Scanner::saveSettings() {
    auto& config = getFrequencyManagerConfig();
    config.acquire();
    json& s = config.conf["scanner"];
    s["scanIntervalMs"] = scanIntervalMs;
    s["dmrDwellMs"] = dmrDwellMs;
    s["hangTimeSec"] = hangTimeSec;
    s["maxListenSec"] = listenTimeSec;
    s["noiseFloor"] = noiseFloor;
    s["signalMarginDb"] = signalMarginDb;
    s["squelchEnabled"] = squelchEnabled;
    s["priorityIntervalSec"] = priorityIntervalSec;
    s["logToFile"] = logToFile;
    config.release(true);
}

void Scanner::loadListState() {
    lockedOut.clear();
    priorityStation.clear();
    if (listName.empty()) { return; }
    auto& config = getFrequencyManagerConfig();
    config.acquire();
    json& s = config.conf["scanner"];
    if (s.contains("lists") && s["lists"].contains(listName)) {
        json& l = s["lists"][listName];
        if (l.contains("lockout") && l["lockout"].is_array()) {
            for (auto& n : l["lockout"]) {
                if (n.is_string()) { lockedOut.insert(n.get<std::string>()); }
            }
        }
        priorityStation = l.value("priority", std::string());
    }
    config.release();
}

void Scanner::saveListState() {
    if (listName.empty()) { return; }
    auto& config = getFrequencyManagerConfig();
    config.acquire();
    json lock = json::array();
    for (auto& n : lockedOut) { lock.push_back(n); }
    config.conf["scanner"]["lists"][listName]["lockout"] = lock;
    config.conf["scanner"]["lists"][listName]["priority"] = priorityStation;
    config.release(true);
}

void Scanner::setLockedOut(const std::string& name, bool locked) {
    if (locked) { lockedOut.insert(name); }
    else { lockedOut.erase(name); }
    saveListState();
}

// ---------------------------------------------------------------- lista

void Scanner::setBookmarks(const std::string& newListName, const std::vector<std::string>& names, const std::map<std::string, FrequencyBookmark>& map) {
    bool listChanged = (newListName != listName);

    std::vector<std::string> sorted = names;
    std::stable_sort(sorted.begin(), sorted.end(), [&map](const std::string& a, const std::string& b) {
        return map.at(a).frequency < map.at(b).frequency;
    });

    if ((listChanged || sorted != bookmarks) && scanning) {
        stop();
    }

    bookmarksMap = map;
    bookmarks = sorted;

    if (listChanged) {
        listName = newListName;
        loadListState();
        currentStationIndex = 0;
    }

    if (!bookmarks.empty() && currentStationIndex < bookmarks.size()) {
        if (!scanning) {
            currentStation = bookmarks[currentStationIndex];
            currentBookmark = bookmarksMap.at(currentStation);
        }
    }
    else {
        currentStationIndex = 0;
        if (!scanning) { currentStation.clear(); }
    }
}

int Scanner::indexOf(const std::string& name) const {
    for (size_t i = 0; i < bookmarks.size(); i++) {
        if (bookmarks[i] == name) { return (int)i; }
    }
    return -1;
}

bool Scanner::findNextIndex(size_t from, size_t& out) const {
    size_t n = bookmarks.size();
    for (size_t step = 1; step <= n; step++) {
        size_t i = (from + step) % n;
        if (!lockedOut.count(bookmarks[i])) {
            out = i;
            return true;
        }
    }
    return false;
}

void Scanner::setCurrentFrequency(double freq) {}

void Scanner::setNoiseFloor(float noiseFloor) {
    this->noiseFloor = noiseFloor;
}

// ---------------------------------------------------------------- rádio / DSD

std::string Scanner::targetVfo() const {
    return currentBookmark.vfoName.empty() ? gui::waterfall.selectedVFO : currentBookmark.vfoName;
}

bool Scanner::bookmarkIsDsd(const FrequencyBookmark& bm) const {
    if (bm.modeIndex < 0) {
        return bm.demodId == SCANNER_DEMOD_DSD || bm.demodId == SCANNER_DEMOD_OLDDSD;
    }
    std::string vfo = bm.vfoName.empty() ? gui::waterfall.selectedVFO : bm.vfoName;
    if (vfo.empty()) { return false; }
    auto radio = (RadioModuleInterface*)core::moduleManager.getInterface(vfo, "RadioModuleInterface");
    if (!radio) { return false; }
    int id = radio->getDemodByIndex(bm.modeIndex);
    return id == SCANNER_DEMOD_DSD || id == SCANNER_DEMOD_OLDDSD;
}

// Lê o estado do decodificador DSD embutido, sem cabo virtual: o próprio
// módulo ch_extravhf_decoder expõe o status pelo canal de comandos.
bool Scanner::queryDsd(DsdStatus& out) const {
    out = DsdStatus{};
    std::string vfo = targetVfo();
    if (vfo.empty()) { return false; }
    for (auto& [instName, inst] : core::moduleManager.instances) {
        if (!inst.module.info || !inst.instance) { continue; }
        if (std::string(inst.module.info->name) != "ch_extravhf_decoder") { continue; }
        if (!inst.instance->isEnabled()) { continue; }
        std::string reply = inst.instance->handleDebugCommand("get_dmr_status", vfo);
        json j = json::parse(reply, nullptr, false);
        if (j.is_discarded() || j.contains("error")) { return false; }
        out.available = j.value("available", false);
        out.sync = j.value("sync", false);
        out.dmr = j.value("dmr", false);
        out.voice = j.value("voice", false);
        out.colorCode = j.value("color_code", 0);
        out.slot0 = j.value("slot0_type", std::string());
        out.slot1 = j.value("slot1_type", std::string());
        return out.available;
    }
    return false;
}

bool Scanner::analogActive() const {
    if (signalHistory.empty()) { return false; }
    float avg = signalHistorySum / signalHistory.size();
    return avg > (noiseFloor + signalMarginDb);
}

// ---------------------------------------------------------------- controle

void Scanner::resetSignal() {
    signalLevelSum = 0.0f;
    signalLevelCount = 0;
    signalHistory.clear();
    signalHistorySum = 0.0f;
    timeSinceLastSwitch = 0.0f;
    timeSinceSignal = 0.0f;
    receiveTime = 0.0f;
    peakSnr = 0.0f;
    lastDsd = DsdStatus{};
}

void Scanner::tuneTo(size_t index, bool priority) {
    if (index >= bookmarks.size()) { return; }
    currentStationIndex = index;
    onPriority = priority;
    currentStation = bookmarks[index];
    currentBookmark = bookmarksMap[currentStation];
    currentIsDsd = bookmarkIsDsd(currentBookmark);
    state = State::SEEK;
    resetSignal();
    applyBookmark(currentBookmark, gui::waterfall.selectedVFO);
}

void Scanner::start() {
    if (bookmarks.empty()) {
        flog::warn("Scanner: lista vazia");
        return;
    }
    size_t first;
    if (!findNextIndex(bookmarks.size() - 1, first)) {
        flog::warn("Scanner: todos os canais estão bloqueados");
        return;
    }
    scanning = true;
    hold = false;
    timeSincePriority = 0.0f;
    tuneTo(first, false);
    flog::info("Scanner started");
}

void Scanner::stop() {
    if (!scanning) return;
    if (state == State::RECEIVE) { endReceive(); }
    scanning = false;
    hold = false;
    onPriority = false;
    state = State::SEEK;
    if (squelchEnabled) {
        sigpath::sinkManager.setAllMuted(false);
    }
    flog::info("Scanner stopped");
}

void Scanner::nextStation() {
    if (!scanning || bookmarks.empty()) return;
    size_t base = onPriority ? returnIndex : currentStationIndex;
    size_t next;
    if (!findNextIndex(base, next)) {
        flog::warn("Scanner: todos os canais estão bloqueados");
        stop();
        return;
    }
    tuneTo(next, false);
}

void Scanner::beginReceive() {
    state = State::RECEIVE;
    timeSinceSignal = 0.0f;
    receiveTime = 0.0f;
}

void Scanner::endReceive() {
    std::string info;
    if (currentIsDsd && lastDsd.available) {
        info = lastDsd.dmr ? ("DMR CC" + std::to_string(lastDsd.colorCode)) : "Digital";
    }
    else {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f dB", peakSnr);
        info = buf;
    }
    // Desconta o hang time: queremos a duração da transmissão
    float secs = (std::max)(0.0f, (receiveTime - timeSinceSignal) / 1000.0f);
    if (secs >= 0.3f) {
        addLog(currentStation, currentBookmark.frequency, secs, info);
    }
    state = State::SEEK;
    timeSinceLastSwitch = 0.0f;
}

void Scanner::addLog(const std::string& name, double freq, float seconds, const std::string& info) {
    logEntries.push_front({ nowString("%H:%M:%S"), name, freq, seconds, info });
    while (logEntries.size() > MAX_LOG_ENTRIES) { logEntries.pop_back(); }

    if (!logToFile) { return; }
    try {
        std::string path = (std::string)core::args["root"] + "/scanner_log.csv";
        bool exists = std::filesystem::exists(path);
        std::ofstream f(path, std::ios::app);
        if (!f) { return; }
        if (!exists) { f << "data_hora,lista,canal,frequencia_hz,duracao_s,info\n"; }
        auto q = [](const std::string& s) {
            std::string o = "\"";
            for (char c : s) { o += (c == '"') ? std::string("\"\"") : std::string(1, c); }
            return o + "\"";
        };
        char fbuf[32], sbuf[32];
        snprintf(fbuf, sizeof(fbuf), "%.0f", freq);
        snprintf(sbuf, sizeof(sbuf), "%.1f", seconds);
        f << nowString("%Y-%m-%d %H:%M:%S") << "," << q(listName) << "," << q(name) << ","
          << fbuf << "," << sbuf << "," << q(info) << "\n";
    }
    catch (const std::exception& e) {
        flog::warn("Scanner: falha ao gravar log: {}", e.what());
    }
}

std::string Scanner::getLogJson() const {
    json arr = json::array();
    for (auto& e : logEntries) {
        arr.push_back({ { "time", e.time }, { "name", e.name }, { "frequency", e.frequency }, { "seconds", e.seconds }, { "info", e.info } });
    }
    return arr.dump();
}

void Scanner::onSNRMeterExtPoint(ImGui::SNRMeterExtPoint point) {
    float signalLevelDb = point.lastDrawnValue / (point.postSnrLocation.x / 90.0f);

    // Logo após sintonizar, o FFT ainda mostra o canal anterior
    if (scanning && timeSinceLastSwitch < SEEK_SETTLE_MS) { return; }

    signalLevelSum += signalLevelDb;
    signalLevelCount++;

    float currentTime = ImGui::GetTime() * 1000.0f;
    signalHistory.push_back({ signalLevelDb, currentTime });
    signalHistorySum += signalLevelDb;
    while (!signalHistory.empty() && (currentTime - signalHistory.front().time) > 500.0f) {
        signalHistorySum -= signalHistory.front().level;
        signalHistory.pop_front();
    }
}

void Scanner::update(float deltaTime) {
    if (!scanning || bookmarks.empty()) return;

    float dt = deltaTime * 1000.0f;
    timeSinceLastSwitch += dt;

    // Detecção: DSD (sync digital) ou SNR (analógico)
    bool dsdOk = false;
    if (currentIsDsd) {
        DsdStatus st;
        dsdOk = queryDsd(st);
        if (dsdOk) { lastDsd = st; }
    }
    // Logo após trocar para um canal DSD o demodulador ainda está sendo criado;
    // nesse intervalo não caímos para SNR (uma portadora analógica pararia o scanner).
    bool useDsd = currentIsDsd && (dsdOk || timeSinceLastSwitch < DSD_STARTUP_MS);

    if (squelchEnabled) {
        sigpath::sinkManager.setAllMuted(state != State::RECEIVE);
    }

    if (state == State::RECEIVE) {
        receiveTime += dt;
        bool active = useDsd ? lastDsd.sync : analogActive();
        if (!signalHistory.empty()) {
            peakSnr = (std::max)(peakSnr, signalHistorySum / signalHistory.size());
        }
        if (active) { timeSinceSignal = 0.0f; }
        else { timeSinceSignal += dt; }

        bool hangExpired = timeSinceSignal >= hangTimeSec * 1000.0f;
        bool maxExpired = listenTimeSec > 0.0f && receiveTime >= listenTimeSec * 1000.0f;
        if (hangExpired || (maxExpired && !hold)) {
            endReceive();
            if (!hold) { nextStation(); }
        }
        return;
    }

    // ---- SEEK
    bool detected = false;
    bool dwellOver = false;
    if (useDsd) {
        detected = timeSinceLastSwitch >= DSD_SETTLE_MS && lastDsd.sync;
        dwellOver = timeSinceLastSwitch >= (std::max)(dmrDwellMs, DSD_SETTLE_MS + 50.0f);
    }
    else {
        dwellOver = timeSinceLastSwitch >= (std::max)(scanIntervalMs, SEEK_SETTLE_MS + 20.0f);
        if (dwellOver && signalLevelCount > 0) {
            detected = (signalLevelSum / signalLevelCount) > (noiseFloor + signalMarginDb);
        }
    }

    if (detected) {
        beginReceive();
        return;
    }
    if (!dwellOver) { return; }

    if (hold) {
        // Segurando: fica no canal esperando a próxima transmissão
        signalLevelSum = 0.0f;
        signalLevelCount = 0;
        timeSinceLastSwitch = SEEK_SETTLE_MS;
        return;
    }

    if (onPriority) {
        // Prioritário quieto: segue a varredura de onde parou
        nextStation();
        return;
    }

    // Hora de dar uma olhada no prioritário?
    timeSincePriority += timeSinceLastSwitch;
    if (!priorityStation.empty() && !lockedOut.count(priorityStation) &&
        timeSincePriority >= priorityIntervalSec * 1000.0f) {
        int p = indexOf(priorityStation);
        timeSincePriority = 0.0f;
        if (p >= 0 && (size_t)p != currentStationIndex) {
            returnIndex = currentStationIndex;
            tuneTo((size_t)p, true);
            return;
        }
    }

    nextStation();
}

void Scanner::onPlayStateChange(bool playing) {
    if (!playing && scanning) {
        stop();
    }
}

// ---------------------------------------------------------------- interface

void Scanner::render() {
    if (!ImGui::CollapsingHeader("Scanner", ImGuiTreeNodeFlags_DefaultOpen)) { return; }

    float w = ImGui::GetContentRegionAvail().x;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float bw = (w - 3 * spacing) / 4.0f;

    // Linha de botões
    if (scanning) {
        if (ImGui::Button("Parar##fm_scan", ImVec2(bw, 0))) { stop(); }
    }
    else {
        if (ImGui::Button("Iniciar##fm_scan", ImVec2(bw, 0))) { start(); }
    }
    ImGui::SameLine();
    // Guardamos o estado antes: os botões abaixo podem mudar 'scanning' e o
    // begin/endDisabled precisa ficar equilibrado.
    const bool ctrlDisabled = !scanning;
    if (ctrlDisabled) { style::beginDisabled(); }
    if (ImGui::Button(hold ? "Soltar##fm_scan" : "Segurar##fm_scan", ImVec2(bw, 0))) {
        hold = !hold;
    }
    ImGui::SameLine();
    if (ImGui::Button("Pular##fm_scan", ImVec2(bw, 0))) {
        if (state == State::RECEIVE) { endReceive(); }
        hold = false;
        nextStation();
    }
    ImGui::SameLine();
    if (ImGui::Button("Bloquear##fm_scan", ImVec2(bw, 0))) {
        if (!currentStation.empty()) {
            if (state == State::RECEIVE) { endReceive(); }
            setLockedOut(currentStation, true);
            hold = false;
            nextStation();
        }
    }
    if (ctrlDisabled) { style::endDisabled(); }

    // Status
    if (scanning) {
        char freqBuf[32];
        snprintf(freqBuf, sizeof(freqBuf), "%.4f MHz", currentBookmark.frequency / 1e6);
        const char* tag = onPriority ? " [P]" : "";
        if (state == State::RECEIVE) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%s %s%s  %s",
                               hold ? "SEGURANDO" : "RECEBENDO", currentStation.c_str(), tag, freqBuf);
        }
        else if (hold) {
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "SEGURANDO %s%s  %s (aguardando)",
                               currentStation.c_str(), tag, freqBuf);
        }
        else {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Procurando... %s%s  %s",
                               currentStation.c_str(), tag, freqBuf);
        }
        if (currentIsDsd) {
            if (lastDsd.available) {
                if (lastDsd.sync) {
                    ImGui::Text("%s  CC %d  S0: %s  S1: %s", lastDsd.dmr ? "DMR" : "Digital",
                                lastDsd.colorCode, lastDsd.slot0.c_str(), lastDsd.slot1.c_str());
                }
                else {
                    ImGui::TextDisabled("Digital: sem sync");
                }
            }
            else if (timeSinceLastSwitch >= DSD_STARTUP_MS) {
                ImGui::TextDisabled("DSD indisponível, usando SNR");
            }
            else {
                ImGui::TextDisabled("Digital: iniciando...");
            }
        }
        else {
            // Sempre uma segunda linha: o painel não "pula" ao alternar AM/DMR
            float avg = signalHistory.empty() ? 0.0f : signalHistorySum / signalHistory.size();
            ImGui::TextDisabled("Analógico  |  SNR %.1f dB  |  limiar %.1f dB", avg, noiseFloor + signalMarginDb);
        }
    }
    else {
        float avg = signalHistory.empty() ? 0.0f : signalHistorySum / signalHistory.size();
        ImGui::Text("Parado");
        ImGui::TextDisabled("SNR %.1f dB  |  limiar %.1f dB", avg, noiseFloor + signalMarginDb);
    }

    // Prioritário
    ImGui::LeftLabel("Prioritário");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    std::string prioLabel = priorityStation.empty() ? "(nenhum)" : priorityStation;
    if (ImGui::BeginCombo("##fm_scan_prio", prioLabel.c_str())) {
        if (ImGui::Selectable("(nenhum)", priorityStation.empty())) {
            priorityStation.clear();
            saveListState();
        }
        for (auto& n : bookmarks) {
            if (ImGui::Selectable(n.c_str(), n == priorityStation)) {
                priorityStation = n;
                saveListState();
            }
        }
        ImGui::EndCombo();
    }

    // Bloqueios
    int selectedCount = 0;
    for (auto& [n, bm] : bookmarksMap) { if (bm.selected) { selectedCount++; } }
    ImGui::Text("Bloqueados: %d de %d", (int)lockedOut.size(), (int)bookmarks.size());
    float hw = (w - spacing) / 2.0f;
    const bool selDisabled = (selectedCount == 0);
    if (selDisabled) { style::beginDisabled(); }
    if (ImGui::Button("Bloquear sel.##fm_scan", ImVec2(hw, 0))) {
        for (auto& [n, bm] : bookmarksMap) {
            if (!bm.selected) { continue; }
            if (lockedOut.count(n)) { lockedOut.erase(n); }
            else { lockedOut.insert(n); }
        }
        saveListState();
    }
    if (selDisabled) { style::endDisabled(); }
    ImGui::SameLine();
    const bool unlockDisabled = lockedOut.empty();
    if (unlockDisabled) { style::beginDisabled(); }
    if (ImGui::Button("Desbloq. todos##fm_scan", ImVec2(hw, 0))) {
        lockedOut.clear();
        saveListState();
    }
    if (unlockDisabled) { style::endDisabled(); }

    // Ajustes
    if (ImGui::TreeNode("Ajustes do scanner##fm_scan")) {
        bool changed = false;

        ImGui::LeftLabel("Tempo/canal (ms)");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputFloat("##fm_scan_interval", &scanIntervalMs, 10.0f, 50.0f, "%.0f")) {
            scanIntervalMs = std::clamp(scanIntervalMs, 30.0f, 5000.0f);
            changed = true;
        }
        ImGui::LeftLabel("Tempo/canal DMR (ms)");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputFloat("##fm_scan_dmr_dwell", &dmrDwellMs, 50.0f, 100.0f, "%.0f")) {
            dmrDwellMs = std::clamp(dmrDwellMs, 200.0f, 5000.0f);
            changed = true;
        }
        ImGui::LeftLabel("Espera após sinal (s)");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputFloat("##fm_scan_hang", &hangTimeSec, 0.5f, 1.0f, "%.1f")) {
            hangTimeSec = std::clamp(hangTimeSec, 0.0f, 60.0f);
            changed = true;
        }
        ImGui::LeftLabel("Máx./canal (s, 0=livre)");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputFloat("##fm_scan_listen", &listenTimeSec, 1.0f, 10.0f, "%.0f")) {
            listenTimeSec = std::clamp(listenTimeSec, 0.0f, 3600.0f);
            changed = true;
        }
        ImGui::LeftLabel("Base de ruído (dB)");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputFloat("##fm_scan_noise", &noiseFloor, 1.0f, 5.0f, "%.1f")) {
            noiseFloor = std::clamp(noiseFloor, 0.0f, 40.0f);
            changed = true;
        }
        if (ImGui::Button("Medir ruído agora (num canal vazio)##fm_scan", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            if (!signalHistory.empty()) {
                noiseFloor = signalHistorySum / signalHistory.size();
                changed = true;
            }
        }
        ImGui::LeftLabel("Margem (dB)");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputFloat("##fm_scan_margin", &signalMarginDb, 0.5f, 1.0f, "%.1f")) {
            signalMarginDb = std::clamp(signalMarginDb, 0.0f, 40.0f);
            changed = true;
        }
        ImGui::LeftLabel("Prioritário a cada (s)");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::InputFloat("##fm_scan_prio_iv", &priorityIntervalSec, 0.5f, 1.0f, "%.1f")) {
            priorityIntervalSec = std::clamp(priorityIntervalSec, 0.5f, 120.0f);
            changed = true;
        }
        if (ImGui::Checkbox("Mudo enquanto procura##fm_scan", &squelchEnabled)) {
            if (!squelchEnabled) { sigpath::sinkManager.setAllMuted(false); }
            changed = true;
        }
        if (ImGui::Checkbox("Gravar log em scanner_log.csv##fm_scan", &logToFile)) {
            changed = true;
        }
        if (changed) { saveSettings(); }
        ImGui::TreePop();
    }

    // Atividade
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "Atividade (%d)###fm_scan_log", (int)logEntries.size());
    if (ImGui::TreeNode(hdr)) {
        if (ImGui::Button("Limpar##fm_scan_log")) { logEntries.clear(); }
        ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
        float h = (std::min<float>)(220.0f * style::uiScale, (logEntries.size() + 1.5f) * ImGui::GetTextLineHeightWithSpacing());
        if (!logEntries.empty() && ImGui::BeginTable("##fm_scan_log_tbl", 4, flags, ImVec2(0, h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Hora");
            ImGui::TableSetupColumn("Canal", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("s");
            ImGui::TableSetupColumn("Info");
            ImGui::TableHeadersRow();
            for (auto& e : logEntries) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(e.time.c_str());
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(e.name.c_str());
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%.4f MHz", e.frequency / 1e6); }
                ImGui::TableSetColumnIndex(2); ImGui::Text("%.1f", e.seconds);
                ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(e.info.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
}
