#pragma once
// DMR troncalizado: lê os CSBK do canal de controle/descanso e monta a lista de
// conversas (talkgroup -> canal). Formatos (campos conforme ETSI TS 102 361-4 e
// a análise do DSD-FME, licença ISC):
//   - Tier III (FID 0x00): PV/TV/BTV_GRANT (opcodes 48, 49, 50, 53) com LPCN + slot
//   - Capacity Plus (FID 0x10, opcode 0x3E): canais ativos (LSN) e seus TGs, canal de descanso
//   - Connect Plus (FID 0x06, opcode 0x03): concessão de voz com LCN + slot
// A frequência de cada canal vem do mapa de canais do usuário (número -> MHz),
// porque esses sistemas anunciam números lógicos, não frequências.
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>

namespace dsp::dmr {

    struct TrunkGrant {
        uint32_t group = 0;
        uint32_t source = 0;
        int channel = 0;        // número do canal no mapa (LPCN, LCN ou repetidora Cap+)
        int slot = -1;          // 0/1
        bool privateCall = false;
        double freq = 0;        // Hz, se o canal estiver no mapa
        double time = 0;
    };

    class TrunkSystem {
    public:
        enum Kind { NONE = 0, TIER3, CAPPLUS, CONPLUS };

        struct View {
            Kind kind = NONE;
            uint64_t csbks = 0;
            bool active = false;
            int restLsn = 0;           // Cap+: canal de descanso (LSN)
            double restFreq = 0;
            std::deque<TrunkGrant> grants;
        };

        void setChannelMap(const std::map<int, double>& m) {
            std::lock_guard<std::mutex> lck(mtx);
            chanMap = m;
            for (auto& g : grants) { g.freq = freqOf(g.channel); }
        }

        static const char* kindName(Kind k) {
            switch (k) {
            case TIER3: return "DMR Tier III";
            case CAPPLUS: return "Capacity Plus";
            case CONPLUS: return "Connect Plus";
            default: return "";
            }
        }

        // b: 12 bytes do CSBK já com CRC conferido; slot do quadro (0/1)
        void handle(const uint8_t* b, int slot, double now) {
            std::lock_guard<std::mutex> lck(mtx);
            csbks++;
            last = now;
            uint8_t opcode = b[0] & 0x3F;
            uint8_t fid = b[1];
            if (fid == 0x00 && (opcode == 48 || opcode == 49 || opcode == 50 || opcode == 53)) {
                kind = TIER3;
                TrunkGrant g;
                int lpcn = (b[2] << 4) | (b[3] >> 4);
                if (lpcn == 0 || lpcn == 0xFFF) { return; }   // inválido ou absoluto (MBC, não tratado)
                g.channel = lpcn;
                g.slot = (b[3] >> 3) & 1;
                g.group = ((uint32_t)b[4] << 16) | ((uint32_t)b[5] << 8) | b[6];
                g.source = ((uint32_t)b[7] << 16) | ((uint32_t)b[8] << 8) | b[9];
                g.privateCall = (opcode == 48 || opcode == 53);
                addGrant(g, now);
            }
            else if (fid == 0x00 && opcode == 28) {
                if (kind == NONE) { kind = TIER3; }   // C_ALOHA: canal de controle Tier III
            }
            else if (fid == 0x06 && opcode == 0x03) {
                kind = CONPLUS;
                TrunkGrant g;
                g.source = ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) | b[4];
                g.group = ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 8) | b[7];
                g.channel = (b[8] & 0xF0) >> 4;
                g.slot = (b[8] & 0x08) >> 3;
                g.privateCall = (b[9] == 3);
                addGrant(g, now);
            }
            else if (fid == 0x06) {
                if (kind == NONE) { kind = CONPLUS; }
            }
            else if (fid == 0x10 && opcode == 0x3E) {
                kind = CAPPLUS;
                capPlus(b, slot, now);
            }
        }

        View view(double now) {
            std::lock_guard<std::mutex> lck(mtx);
            View v;
            v.kind = kind;
            v.csbks = csbks;
            v.active = (now - last) < 3.0;
            v.restLsn = restLsn;
            v.restFreq = restLsn ? freqOf((restLsn + 1) / 2) : 0;
            v.grants = grants;
            return v;
        }

    private:
        std::mutex mtx;
        Kind kind = NONE;
        uint64_t csbks = 0;
        double last = -100;
        int restLsn = 0;
        std::deque<TrunkGrant> grants;
        std::map<int, double> chanMap;
        // Cap+: blocos encadeados por slot
        uint8_t capBits[2][80 + 7 * 8 * 7] = {};
        int capBlock[2] = { 0, 0 };

        double freqOf(int ch) const {
            auto it = chanMap.find(ch);
            return it == chanMap.end() ? 0 : it->second;
        }

        void addGrant(TrunkGrant g, double now) {
            g.time = now;
            g.freq = freqOf(g.channel);
            for (auto it = grants.begin(); it != grants.end(); ++it) {
                if (it->group == g.group && it->privateCall == g.privateCall) {
                    if (g.source == 0) { g.source = it->source; }
                    grants.erase(it);
                    break;
                }
            }
            grants.push_front(g);
            while (grants.size() > 30) { grants.pop_back(); }
        }

        static int bitsToInt(const uint8_t* bits, int n) {
            int v = 0;
            for (int i = 0; i < n; i++) { v = (v << 1) | (bits[i] & 1); }
            return v;
        }

        void capPlus(const uint8_t* b, int slotIn, double now) {
            uint8_t pdu[96];
            for (int i = 0; i < 96; i++) { pdu[i] = (b[i / 8] >> (7 - (i % 8))) & 1; }
            int fl = bitsToInt(&pdu[16], 2);
            int ts = pdu[18] & 1;
            (void)slotIn;
            restLsn = bitsToInt(&pdu[20], 4);
            uint8_t* cb = capBits[ts];
            if (fl == 2 || fl == 3) {
                memset(cb, 0, sizeof(capBits[ts]));
                for (int i = 0; i < 80; i++) { cb[i] = pdu[i]; }
                capBlock[ts] = 0;
            }
            else {
                int blk = capBlock[ts];
                if (blk > 6) { blk = 6; }
                for (int i = 0; i < 56; i++) { cb[80 + 56 * blk + i] = pdu[24 + i]; }
                capBlock[ts] = blk + 1;
            }
            if (!(fl == 1 || fl == 3)) { return; }   // espera o último bloco
            // canais (LSN) com chamada de grupo e o TG de 8 bits de cada um
            uint8_t ch[16] = {};
            int tally = 0;
            for (int i = 0; i < 8; i++) {
                ch[i] = cb[24 + i];
                tally += ch[i];
            }
            int bankTwo = bitsToInt(&cb[32 + tally * 8], 8);
            int b2start = tally;
            if (bankTwo) {
                for (int i = 0; i < 8; i++) {
                    ch[8 + i] = cb[32 + b2start * 8 + i];
                    tally += ch[8 + i];
                }
            }
            int k = 0;
            for (int i = 0; i < 16; i++) {
                if (i == 8) { k++; }
                if (!ch[i]) { continue; }
                int tg = bitsToInt(&cb[k * 8 + 32], 8);
                if (tg != 0) { k++; }
                TrunkGrant g;
                g.group = (uint32_t)tg;
                // LSN (i+1) -> repetidora (i/2 + 1) e slot (i % 2)
                g.channel = i / 2 + 1;
                g.slot = i % 2;
                addGrant(g, now);
            }
        }
    };
}
