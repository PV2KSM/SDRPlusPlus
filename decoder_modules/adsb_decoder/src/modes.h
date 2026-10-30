#pragma once
// Decodificador Mode S / ADS-B (1090 MHz) independente do SDR++.
// Entrada: amostras de magnitude a 2 MS/s (1 bit = 2 amostras).
// Baseado no algoritmo clássico do dump1090 (Salvatore Sanfilippo, BSD).

#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <functional>

namespace modes {

    constexpr int LONG_MSG_BITS = 112;
    constexpr int SHORT_MSG_BITS = 56;

    uint32_t crc24(const uint8_t* msg, int bits);   // resto da divisão (0 = íntegra)
    int messageBits(int df);

    struct Message {
        uint8_t data[14];
        int bits = 0;
        int df = 0;
        uint32_t icao = 0;
        bool crcOk = false;
        bool corrected = false;
        float signalDb = 0.0f;   // dBFS
    };

    // ------------------------------------------------------------ demodulador
    // Funciona em qualquer taxa >= 2 MS/s (o RTL-SDR usa 2,4 MS/s). Em vez de
    // depender de amostras alinhadas com os pulsos, integra a energia de cada
    // meio-bit (0,5 µs) e testa alguns alinhamentos finos até o CRC bater.
    class Demodulator {
    public:
        explicit Demodulator(double sampleRate = 2400000.0) { setSampleRate(sampleRate); }
        void setSampleRate(double sampleRate);

        // Chamado para cada mensagem com preâmbulo válido e CRC aceito.
        std::function<void(const Message&)> onMessage;
        // ICAOs conhecidos (para validar DF4/5/20/21, cujo CRC traz o endereço)
        std::function<bool(uint32_t)> isKnownIcao;

        void process(const float* mag, int count);
        void reset() { tail.clear(); }

        uint64_t preambles = 0;
        uint64_t accepted = 0;
        uint64_t rejected = 0;

    private:
        double spu = 2.4;          // amostras por microssegundo
        int fullLen = 0;           // amostras de preâmbulo + mensagem longa
        std::vector<float> buf;
        std::vector<float> tail;
        std::vector<double> csum;  // soma acumulada da magnitude
        std::vector<float> noiseSample;

        double energy(double a, double b) const;
        double preambleScore(double t, double noise, bool& ok) const;
        bool decodeAt(double t, Message& out);
        bool validate(uint8_t* data, Message& out);
    };

    // ------------------------------------------------------------ aviões
    struct TrailPoint {
        double lat, lon;
        int alt;
        double t;
    };

    struct Aircraft {
        uint32_t icao = 0;
        std::string callsign;
        int altitude = 0;        bool hasAltitude = false;
        bool altitudeGnss = false;
        double groundSpeed = 0;  double track = 0;  bool hasVelocity = false;
        int verticalRate = 0;    bool hasVerticalRate = false;
        int squawk = -1;
        double lat = 0, lon = 0; bool hasPosition = false;
        double lastSeen = 0, lastPos = 0;
        uint64_t messages = 0;
        float signalDb = -100.0f;
        // CPR
        int cprLat[2] = { 0, 0 }, cprLon[2] = { 0, 0 };
        double cprTime[2] = { 0, 0 };
        std::deque<TrailPoint> trail;
    };

    class AircraftDB {
    public:
        // Posição da estação (para decodificação local e filtro de distância)
        void setReceiver(double lat, double lon, bool valid);
        bool receiverValid() const { return rxValid; }
        double receiverLat() const { return rxLat; }
        double receiverLon() const { return rxLon; }

        void handle(const Message& m, double now);
        bool isKnown(uint32_t icao);
        void expire(double now, double maxAge = 60.0);
        void clear();

        // Cópia segura (UI / HTTP)
        std::vector<Aircraft> snapshot();
        std::string toJson(double now, bool withTrails = true);

        uint64_t totalMessages = 0;
        double maxRangeKm = 450.0;   // posições mais longe que isso são descartadas

    private:
        std::mutex mtx;
        std::map<uint32_t, Aircraft> aircraft;
        std::map<uint32_t, double> knownIcao;   // DF11/DF17 recentes
        double rxLat = 0, rxLon = 0;
        bool rxValid = false;

        void decodeExtendedSquitter(Aircraft& a, const uint8_t* me, double now);
        void updatePosition(Aircraft& a, int fflag, int latCpr, int lonCpr, double now);
    };

    // Utilidades (expostas para teste)
    int cprNL(double lat);
    bool cprGlobal(int latE, int lonE, int latO, int lonO, bool oddIsNewer, double& lat, double& lon);
    void cprLocal(double refLat, double refLon, int latCpr, int lonCpr, int fflag, double& lat, double& lon);
    int decodeAc13(uint32_t ac13);      // altitude DF0/4/16/20 (pés) ou INT32_MIN
    int decodeAc12(uint32_t ac12);      // altitude ADS-B (pés) ou INT32_MIN
    int decodeId13(uint32_t id13);      // squawk (octal como inteiro decimal, ex.: 7700)
    double distanceKm(double lat1, double lon1, double lat2, double lon2);
    double bearingDeg(double lat1, double lon1, double lat2, double lon2);
    bool hexToBytes(const std::string& hex, uint8_t* out, int& bits);
}
