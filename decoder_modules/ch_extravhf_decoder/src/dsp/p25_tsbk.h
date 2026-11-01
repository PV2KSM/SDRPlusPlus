#pragma once
// P25 fase 1: decodificação do canal de controle (TSBK) de sistemas troncalizados.
//
// Cada TSBK são 98 dibits: entrelaçados em 49 símbolos de 4 bits, codificados
// em treliça de taxa 1/2 (4 estados) e protegidos por CRC-CCITT (TIA-102.AABB).
// Daqui saem: identificação do sistema (WACN, SysID, RFSS, site), a tabela de
// frequências (IDEN_UP) e as concessões de canal de voz (talkgroup -> frequência).
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <cstdio>

namespace dsp::p25 {

    // Palavra de 4 bits enviada para cada (estado = dibit anterior, entrada = dibit atual)
    static const uint8_t TRELLIS_1_2[4][4] = {
        { 0x2, 0xC, 0x1, 0xF },
        { 0xE, 0x0, 0xD, 0x3 },
        { 0x9, 0x7, 0xA, 0x4 },
        { 0x5, 0xB, 0x6, 0x8 },
    };

    // Ordem de transmissão dos 49 símbolos: 0,13,25,37, 1,14,26,38, ... 12
    inline int interleaveSource(int t) {
        int col = t % 4, row = t / 4;
        static const int base[4] = { 0, 13, 25, 37 };
        return base[col] + row;
    }

    inline int popcount4(uint8_t v) {
        v &= 0xF;
        return (v & 1) + ((v >> 1) & 1) + ((v >> 2) & 1) + ((v >> 3) & 1);
    }

    // Viterbi de 4 estados; devolve 48 dibits (o 49º é o "flush")
    inline int viterbi(const uint8_t* sym, uint8_t* outDibits) {
        const int N = 49;
        const int INF = 1 << 20;
        int metric[4] = { 0, INF, INF, INF };   // começa no estado 0
        uint8_t from[N][4];
        memset(from, 0, sizeof(from));
        for (int k = 0; k < N; k++) {
            int nm[4] = { INF, INF, INF, INF };
            for (int st = 0; st < 4; st++) {
                if (metric[st] >= INF) { continue; }
                for (int in = 0; in < 4; in++) {
                    int m = metric[st] + popcount4(TRELLIS_1_2[st][in] ^ sym[k]);
                    if (m < nm[in]) {
                        nm[in] = m;
                        from[k][in] = (uint8_t)st;
                    }
                }
            }
            memcpy(metric, nm, sizeof(metric));
        }
        // termina no estado 0 (flush)
        int st = 0, errors = metric[0];
        uint8_t path[N];
        for (int k = N - 1; k >= 0; k--) {
            path[k] = (uint8_t)st;
            st = from[k][st];
        }
        for (int k = 0; k < N - 1; k++) { outDibits[k] = path[k]; }
        return errors;
    }

    // CRC-CCITT do TSBK: divisão dos 96 bits; mensagem válida deixa resto 0xFFFF
    inline uint16_t crcRemainder(const uint8_t* bytes, int nbits) {
        uint32_t crc = 0;
        for (int i = 0; i < nbits; i++) {
            uint32_t bit = (bytes[i / 8] >> (7 - (i % 8))) & 1;
            crc = ((crc << 1) | bit) & 0x1FFFF;
            if (crc & 0x10000) { crc = (crc & 0xFFFF) ^ 0x1021; }
        }
        return (uint16_t)crc;
    }

    // Decodifica um TSBK (98 dibits já sem os dibits de status). bytes[12] na saída.
    inline bool decodeTsbk(const uint8_t* dibits, uint8_t* bytes, int* bitErrors = nullptr) {
        uint8_t rx[49], sym[49], out[48];
        for (int t = 0; t < 49; t++) { rx[t] = (uint8_t)(((dibits[2 * t] & 3) << 2) | (dibits[2 * t + 1] & 3)); }
        // as duas convenções da tabela de entrelaçamento; o CRC decide
        for (int dir = 0; dir < 2; dir++) {
            for (int t = 0; t < 49; t++) {
                if (dir == 0) { sym[interleaveSource(t)] = rx[t]; }
                else { sym[t] = rx[interleaveSource(t)]; }
            }
            int err = viterbi(sym, out);
            memset(bytes, 0, 12);
            for (int k = 0; k < 48; k++) { bytes[k / 4] |= (uint8_t)(out[k] << (6 - 2 * (k % 4))); }
            if (crcRemainder(bytes, 96) == 0xFFFF) {
                if (bitErrors) { *bitErrors = err; }
                return true;
            }
        }
        return false;
    }

    // Monta um TSBK (bytes[0..9]; o CRC é calculado) em 98 dibits — usado nos testes
    inline void encodeTsbk(const uint8_t* bytes10, uint8_t* dibits) {
        uint8_t b[12];
        memcpy(b, bytes10, 10);
        b[10] = b[11] = 0;
        uint16_t c = (uint16_t)(crcRemainder(b, 96) ^ 0xFFFF);
        b[10] = (uint8_t)(c >> 8);
        b[11] = (uint8_t)(c & 0xFF);
        uint8_t in[49], sym[49];
        for (int k = 0; k < 48; k++) { in[k] = (b[k / 4] >> (6 - 2 * (k % 4))) & 3; }
        in[48] = 0;
        int st = 0;
        for (int k = 0; k < 49; k++) {
            sym[k] = TRELLIS_1_2[st][in[k]];
            st = in[k];
        }
        for (int t = 0; t < 49; t++) {
            uint8_t v = sym[interleaveSource(t)];
            dibits[2 * t] = (v >> 2) & 3;
            dibits[2 * t + 1] = v & 3;
        }
    }

    // ------------------------------------------------------------------ sistema
    struct Grant {
        uint32_t group = 0;
        uint32_t source = 0;      // 0 se o TSBK não traz
        uint16_t channel = 0;
        double freq = 0;          // Hz (0 se ainda não temos a tabela IDEN)
        bool encrypted = false;
        bool emergency = false;
        double time = 0;          // relógio do sistema (s)
    };

    struct Iden {
        bool valid = false;
        double baseHz = 0;
        double spacingHz = 0;
    };

    class System {
    public:
        void reset() {
            std::lock_guard<std::mutex> lck(mtx);
            *this->dataPtr() = Data();
        }

        void handle(const uint8_t* b, double now) {
            std::lock_guard<std::mutex> lck(mtx);
            Data& d = *dataPtr();
            d.tsbks++;
            d.lastTsbk = now;
            uint8_t opcode = b[0] & 0x3F;
            uint8_t mfid = b[1];
            if (mfid != 0x00 && mfid != 0x01) { return; }   // só o padrão
            auto ch = [&](int i) { return (uint16_t)((b[i] << 8) | b[i + 1]); };
            switch (opcode) {
            case 0x00: {   // GRP_V_CH_GRANT
                Grant g;
                g.encrypted = (b[2] & 0x40) != 0;
                g.emergency = (b[2] & 0x80) != 0;
                g.channel = ch(3);
                g.group = ch(5);
                g.source = ((uint32_t)b[7] << 16) | ((uint32_t)b[8] << 8) | b[9];
                addGrant(d, g, now);
                break;
            }
            case 0x02: {   // GRP_V_CH_GRANT_UPDT (duas concessões)
                Grant g1, g2;
                g1.channel = ch(2);
                g1.group = ch(4);
                g2.channel = ch(6);
                g2.group = ch(8);
                addGrant(d, g1, now);
                if (g2.channel != g1.channel || g2.group != g1.group) { addGrant(d, g2, now); }
                break;
            }
            case 0x34: {   // IDEN_UP_VU
                int id = b[2] >> 4;
                uint32_t spacing = ((b[4] & 0x03) << 8) | b[5];
                uint32_t base = ((uint32_t)b[6] << 24) | ((uint32_t)b[7] << 16) | ((uint32_t)b[8] << 8) | b[9];
                d.iden[id].valid = true;
                d.iden[id].spacingHz = spacing * 125.0;
                d.iden[id].baseHz = base * 5.0;
                break;
            }
            case 0x3D: {   // IDEN_UP
                int id = b[2] >> 4;
                uint32_t spacing = ((b[4] & 0x03) << 8) | b[5];
                uint32_t base = ((uint32_t)b[6] << 24) | ((uint32_t)b[7] << 16) | ((uint32_t)b[8] << 8) | b[9];
                d.iden[id].valid = true;
                d.iden[id].spacingHz = spacing * 125.0;
                d.iden[id].baseHz = base * 5.0;
                break;
            }
            case 0x3B: {   // NET_STS_BCST
                d.wacn = ((uint32_t)b[3] << 12) | ((uint32_t)b[4] << 4) | (b[5] >> 4);
                d.sysid = ((b[5] & 0x0F) << 8) | b[6];
                d.ccChannel = ch(7);
                d.hasNet = true;
                break;
            }
            case 0x3A: {   // RFSS_STS_BCST
                d.sysid = ((b[3] & 0x0F) << 8) | b[4];
                d.rfss = b[5];
                d.site = b[6];
                d.ccChannel = ch(7);
                d.hasSite = true;
                break;
            }
            default:
                break;
            }
            // frequências que ficaram conhecidas depois
            for (auto& g : d.grants) {
                if (g.freq == 0) { g.freq = channelFreq(d, g.channel); }
            }
        }

        std::string json(double now) {
            std::lock_guard<std::mutex> lck(mtx);
            Data& d = *dataPtr();
            std::string s = "{\"tsbks\":" + std::to_string(d.tsbks);
            s += ",\"active\":" + std::string((now - d.lastTsbk) < 3.0 ? "true" : "false");
            s += ",\"wacn\":" + std::to_string(d.wacn) + ",\"sysid\":" + std::to_string(d.sysid);
            s += ",\"rfss\":" + std::to_string(d.rfss) + ",\"site\":" + std::to_string(d.site);
            s += ",\"cc_freq\":" + std::to_string((long long)channelFreq(d, d.ccChannel));
            s += ",\"grants\":[";
            bool first = true;
            for (auto& g : d.grants) {
                if (!first) { s += ","; }
                first = false;
                s += "{\"group\":" + std::to_string(g.group) + ",\"source\":" + std::to_string(g.source) +
                     ",\"channel\":" + std::to_string(g.channel) + ",\"freq\":" + std::to_string((long long)g.freq) +
                     ",\"encrypted\":" + (g.encrypted ? "true" : "false") + ",\"emergency\":" + (g.emergency ? "true" : "false") +
                     ",\"age\":" + std::to_string(now - g.time) + "}";
            }
            s += "]}";
            return s;
        }

        // cópia para a interface
        struct View {
            uint64_t tsbks = 0;
            bool active = false;
            uint32_t wacn = 0, sysid = 0;
            int rfss = 0, site = 0;
            double ccFreq = 0;
            std::deque<Grant> grants;
        };
        View view(double now) {
            std::lock_guard<std::mutex> lck(mtx);
            Data& d = *dataPtr();
            View v;
            v.tsbks = d.tsbks;
            v.active = (now - d.lastTsbk) < 3.0;
            v.wacn = d.wacn;
            v.sysid = d.sysid;
            v.rfss = d.rfss;
            v.site = d.site;
            v.ccFreq = channelFreq(d, d.ccChannel);
            v.grants = d.grants;
            return v;
        }

    private:
        struct Data {
            uint64_t tsbks = 0;
            double lastTsbk = -100;
            uint32_t wacn = 0, sysid = 0;
            int rfss = 0, site = 0;
            uint16_t ccChannel = 0;
            bool hasNet = false, hasSite = false;
            Iden iden[16];
            std::deque<Grant> grants;   // mais recente primeiro, um por talkgroup
        };
        Data data;
        std::mutex mtx;
        Data* dataPtr() { return &data; }

        static double channelFreq(const Data& d, uint16_t channel) {
            int id = channel >> 12;
            int n = channel & 0x0FFF;
            if (!d.iden[id].valid) { return 0; }
            return d.iden[id].baseHz + n * d.iden[id].spacingHz;
        }

        static void addGrant(Data& d, Grant g, double now) {
            g.time = now;
            g.freq = channelFreq(d, g.channel);
            for (auto it = d.grants.begin(); it != d.grants.end(); ++it) {
                if (it->group == g.group) {
                    if (g.source == 0) { g.source = it->source; }
                    d.grants.erase(it);
                    break;
                }
            }
            d.grants.push_front(g);
            while (d.grants.size() > 30) { d.grants.pop_back(); }
        }
    };
}
