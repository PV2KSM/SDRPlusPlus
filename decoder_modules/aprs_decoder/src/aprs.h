#pragma once
// APRS (VHF 1200 bit/s): áudio FM -> AFSK Bell 202 (1200/2200 Hz) -> HDLC/AX.25 -> APRS.
// O demodulador roda três "fatiadores" com pesos diferentes entre os tons de marca e
// espaço (rádios com e sem pré-ênfase chegam com os tons em volumes bem diferentes) e
// entrega todo quadro com FCS correto, sem repetir o mesmo quadro.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aprs {

    static const double SAMPLE_RATE = 24000.0;   // taxa do canal (IQ) e do áudio FM

    struct Frame {
        std::string src, dst;
        std::vector<std::string> path;   // digipeaters, com '*' nos que já repetiram
        std::string info;
        bool direct = true;              // nenhum digipeater repetiu: ouvido direto
        std::string tnc2() const;        // "SRC>DST,PATH:info"
    };

    // Dados extraídos do campo de informação
    struct Report {
        enum Type { OTHER, POSITION, MICE, OBJECT, ITEM, MESSAGE, STATUS, WEATHER, TELEMETRY };
        Type type = OTHER;
        std::string name;        // objeto/item: nome; demais: indicativo de origem
        bool hasPos = false;
        double lat = 0, lon = 0;
        char symTable = '/', symCode = ' ';
        int speedKmh = -1, course = -1;
        double altM = -1e9;
        std::string comment;
        std::string msgTo, msgText, msgId;   // mensagens
        bool killed = false;                 // objeto apagado
    };

    bool parse(const Frame& f, Report& r);
    const char* symbolName(char table, char code);
    const char* typeName(Report::Type t);

    // AX.25 -> Frame (bytes sem FCS)
    bool decodeAx25(const uint8_t* b, int n, Frame& f);
    // Frame -> bytes AX.25 com FCS (testes)
    std::vector<uint8_t> encodeAx25(const Frame& f);
    uint16_t fcs(const uint8_t* b, int n);

    class Demodulator {
    public:
        Demodulator();
        void reset();
        // áudio FM (desvio normalizado, qualquer escala) a SAMPLE_RATE
        void process(const float* audio, int count);
        std::function<void(const Frame&)> onFrame;
        uint64_t frames = 0, badFcs = 0;

    private:
        struct Slicer {
            float gain = 1.0f;
            int32_t pll = 0;
            float prevD = 0;
            int prevBit = 0;
            uint8_t flagReg = 0;
            int ones = 0;
            std::vector<uint8_t> bits;
            bool inFrame = false;
        };
        static const int N = 20;   // amostras por bit (24000 / 1200)
        float histM[2][N] = {}, histS[2][N] = {};
        double sumM[2] = {}, sumS[2] = {};
        int pos = 0;
        double phM = 0, phS = 0;
        float dcAvg = 0;
        float mPeak = 0, mValley = 0, sPeak = 0, sValley = 0;
        int resum = 0;
        Slicer sl[3];
        std::vector<uint8_t> lastFrame;
        int sinceLast = 1 << 30;

        void bit(Slicer& s, int b);
        void endFrame(Slicer& s);
    };

    // Gera o áudio AFSK de um quadro (testes): devolve amostras a SAMPLE_RATE
    std::vector<float> synthesize(const Frame& f, int preambleFlags = 30);
}
