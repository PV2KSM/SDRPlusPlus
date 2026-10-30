#pragma once
// Decodificador ACARS (VHF, AM-MSK 2400 bit/s) independente do SDR++.
// Entrada: amostras complexas de um canal (24 kHz). Saída: mensagens já
// separadas em campos (modo, matrícula, label, voo, texto...).
//
// Como funciona:
//  1. detector de portadora pela envoltória AM (rajada = mensagem);
//  2. a rajada é demodulada em AM, o áudio (tons de 1200/2400 Hz) é levado
//     para banda base em 1800 Hz e passa por um discriminador de frequência;
//  3. os bits são fatiados testando as 10 fases de amostragem e as quatro
//     convenções possíveis (tom direto/invertido, diferencial ou não) até
//     achar SYN SYN SOH e um CRC válido;
//  4. caracteres com paridade errada são corrigidos usando o CRC.

#include <complex>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace acars {

    constexpr int SAMPLE_RATE = 24000;
    constexpr int BAUD = 2400;
    constexpr int SPB = SAMPLE_RATE / BAUD;   // amostras por bit

    struct Message {
        double freq = 0;          // Hz (preenchido pelo módulo)
        double time = 0;          // unix (s)
        char mode = 0;
        std::string reg;          // matrícula (sem os pontos à esquerda)
        char ack = 0;
        std::string label;        // 2 caracteres
        char blockId = 0;
        bool downlink = false;    // avião -> solo
        std::string msgNo;        // só descida
        std::string flight;       // só descida
        std::string text;
        bool more = false;        // terminou com ETB (continua na próxima)
        int corrected = 0;        // caracteres corrigidos pelo CRC
        float levelDb = 0;        // nível da rajada
        std::string decoded;      // CPDLC/ADS-C/... decodificado (libacars)
        std::string app;          // "CPDLC", "ADS-C", "MIAM"... ou vazio
        std::string summary;      // resumo em português (posição, autorização...)
    };

    uint16_t crc16(const uint8_t* data, size_t len, uint16_t crc = 0);   // CRC-16/KERMIT
    std::string labelDescription(const std::string& label);
    std::string sanitize(const std::string& s);                           // troca controles por espaço

    // Analisa os bytes de uma mensagem a partir do SOH (exclusive) até o CRC.
    // Usado internamente e pelos testes.
    bool parseFrame(const std::vector<uint8_t>& raw, Message& out);

    // Decodifica uma rajada (envoltória AM já calculada a SAMPLE_RATE)
    bool decodeBurst(const std::vector<float>& envelope, Message& out);

    // Gera uma rajada sintética (para testes): mensagem completa em envoltória AM
    std::vector<float> synthesize(const Message& m, float snrDb = 40, unsigned seed = 1, int convention = 0, double clockPpm = 0);
    std::vector<uint8_t> buildFrame(const Message& m);   // SOH..DEL com CRC

    class ChannelDecoder {
    public:
        std::function<void(Message&)> onMessage;
        void process(const std::complex<float>* in, int count);
        void reset();
        float noiseDb() const;
        bool inBurst() const { return burst; }
        uint64_t bursts = 0, decoded = 0;

    private:
        static constexpr int WIN = SAMPLE_RATE / 200;   // janela de 5 ms
        std::vector<float> win;
        std::vector<float> pre;        // últimas janelas antes da rajada
        std::vector<float> buf;
        float floorPow = -1;
        float winAcc = 0;
        int winN = 0;
        bool burst = false;
        int quiet = 0, loud = 0;
        float peakPow = 0;
        void endBurst();
    };
}
