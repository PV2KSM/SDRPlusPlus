#pragma once

#include <string>
#include <vector>
#include <deque>
#include <set>
#include <map>
#include <chrono>
#include <functional>
#include <imgui.h>
#include <module.h>
#include "frequency_manager.h"
#include <gui/widgets/snr_meter.h>

class FrequencyManagerModule; // Forward declaration

// Scanner por lista de canais (bookmarks do Frequency Manager).
//
// - Canais analógicos (AM/NFM...): detecção por SNR, como antes.
// - Canais em modo DSD (DMR/P25/NXDN...): detecção pelo sincronismo do
//   decodificador DSD embutido (ch_extravhf_decoder). Para só quando há
//   sinal digital de verdade, não em qualquer portadora. Sem cabo virtual.
// - Fica no canal enquanto houver sinal e sai após o "tempo de espera"
//   (hang time) sem sinal, em vez de um tempo fixo.
// - Segurar, pular, bloquear (lockout persistente) e canal prioritário.
// - Log de atividade na tela e em CSV.
class Scanner {
public:
    Scanner(FrequencyManagerModule* module);
    ~Scanner();

    bool isScanning() const { return scanning; }
    std::string getCurrentStation() const { return currentStation; }
    void startScanner() { start(); }
    void stopScanner() { stop(); }

    void onPlayStateChange(bool playing);

    void setBookmarks(const std::string& listName, const std::vector<std::string>& bookmarks, const std::map<std::string, FrequencyBookmark>& bookmarksMap);
    void setCurrentFrequency(double freq);
    void setNoiseFloor(float noiseFloor);

    void render();
    void update(float deltaTime);

    // Estado para automação/testes (comando get_scanner_status)
    bool isReceiving() const { return scanning && state == State::RECEIVE; }
    bool isHolding() const { return hold; }
    std::string getLogJson() const;

    // Bloqueio (lockout) de canais da lista atual
    bool isLockedOut(const std::string& name) const { return lockedOut.count(name) > 0; }
    void setLockedOut(const std::string& name, bool locked);

private:
    enum class State { SEEK, RECEIVE };

    struct DsdStatus {
        bool available = false; // há um demodulador DSD ativo neste VFO
        bool sync = false;
        bool dmr = false;
        bool voice = false;
        int colorCode = 0;
        std::string slot0;
        std::string slot1;
    };

    struct LogEntry {
        std::string time;   // HH:MM:SS
        std::string name;
        double frequency;
        float seconds;
        std::string info;   // "AM 12 dB" / "DMR CC1"
    };

    FrequencyManagerModule* module;
    bool scanning = false;
    State state = State::SEEK;
    bool hold = false;

    // Ajustes (persistidos em config["scanner"])
    float scanIntervalMs = 100.0f;  // tempo em cada canal analógico procurando sinal
    float dmrDwellMs = 450.0f;      // tempo em cada canal DSD (o sync demora mais)
    float hangTimeSec = 2.0f;       // continua no canal X s depois que o sinal some
    float listenTimeSec = 0.0f;     // tempo máximo num canal (0 = sem limite)
    float noiseFloor = 3.0f;        // SNR base (dB)
    float signalMarginDb = 4.0f;    // margem acima da base para considerar sinal
    bool squelchEnabled = false;    // mudo enquanto procura
    float priorityIntervalSec = 3.0f;
    bool logToFile = true;

    // Lista atual
    std::string listName;
    std::vector<std::string> bookmarks;  // ordenados por frequência
    std::map<std::string, FrequencyBookmark> bookmarksMap;
    std::set<std::string> lockedOut;
    std::string priorityStation;

    // Canal atual
    size_t currentStationIndex = 0;
    std::string currentStation;
    FrequencyBookmark currentBookmark;
    bool currentIsDsd = false;
    bool onPriority = false;         // visitando o prioritário
    size_t returnIndex = 0;          // para onde voltar depois do prioritário
    float timeSinceLastSwitch = 0.0f; // ms
    float timeSinceSignal = 0.0f;     // ms
    float timeSincePriority = 0.0f;   // ms
    float receiveTime = 0.0f;         // ms
    float peakSnr = 0.0f;
    DsdStatus lastDsd;

    // SNR
    void onSNRMeterExtPoint(ImGui::SNRMeterExtPoint point);
    EventHandler<ImGui::SNRMeterExtPoint> snrMeterHandler;
    EventHandler<bool> playStateHandler;
    float signalLevelSum = 0.0f;
    int signalLevelCount = 0;
    struct SignalSample {
        float level;
        float time;
    };
    std::deque<SignalSample> signalHistory;
    float signalHistorySum = 0.0f;

    // Log
    std::deque<LogEntry> logEntries;

    void start();
    void stop();
    void nextStation();
    void tuneTo(size_t index, bool priority);
    bool findNextIndex(size_t from, size_t& out) const;
    int indexOf(const std::string& name) const;
    void resetSignal();
    bool analogActive() const;
    bool bookmarkIsDsd(const FrequencyBookmark& bm) const;
    bool queryDsd(DsdStatus& out) const;
    void beginReceive();
    void endReceive();
    void addLog(const std::string& name, double freq, float seconds, const std::string& info);
    void saveSettings();
    void saveListState();
    void loadListState();
    std::string targetVfo() const;
};
