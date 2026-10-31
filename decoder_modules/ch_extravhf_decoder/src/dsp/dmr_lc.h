#pragma once
// Decodifica o Link Control (LC) dos quadros DMR "Voice LC Header" e
// "Terminator with LC": quem fala (ID de origem), para quem (talkgroup ou ID)
// e o tipo de chamada. Caminho: 196 bits -> desentrelaçamento BPTC(196,96) ->
// 96 bits -> 9 bytes de LC + 3 de Reed-Solomon(12,9) com máscara (ETSI TS 102 361-1).
#include <cstdint>

namespace dsp::dmr {
    struct LC {
        bool valid = false;
        uint8_t flco = 0;        // 0 = chamada de grupo, 3 = chamada privada
        uint8_t fid = 0;         // fabricante (0 = padrão)
        uint8_t options = 0;     // bit 7 = emergência, bit 6 = criptografia (privacy)
        uint32_t dst = 0;        // talkgroup ou ID de destino
        uint32_t src = 0;        // ID de quem transmite
    };

    class RS129 {
    public:
        RS129() {
            int x = 1;
            for (int i = 0; i < 255; i++) {
                exp_[i] = (uint8_t)x;
                log_[x] = (uint8_t)i;
                x <<= 1;
                if (x & 0x100) { x ^= 0x11D; }
            }
            for (int i = 255; i < 512; i++) { exp_[i] = exp_[i - 255]; }
        }
        uint8_t mul(uint8_t a, uint8_t b) const { return (a == 0 || b == 0) ? 0 : exp_[log_[a] + log_[b]]; }
        uint8_t div(uint8_t a, uint8_t b) const { return (a == 0) ? 0 : exp_[(log_[a] + 255 - log_[b]) % 255]; }
        // g(x) = (x+a)(x+a^2)(x+a^3): palavra válida se anula em a, a^2, a^3.
        // Corrige 1 byte errado (o código garante isso com 3 de paridade).
        bool check(uint8_t* cw) const {
            uint8_t S[3];
            for (int r = 1; r <= 3; r++) {
                uint8_t s = 0;
                for (int i = 0; i < 12; i++) { s = mul(s, exp_[r]) ^ cw[i]; }
                S[r - 1] = s;
            }
            if (!S[0] && !S[1] && !S[2]) { return true; }
            if (!S[0] || !S[1]) { return false; }
            // erro e na posição de grau j: S1 = e.a^j, S2 = e.a^2j, S3 = e.a^3j
            uint8_t aj = div(S[1], S[0]);
            uint8_t e = div(mul(S[0], S[0]), S[1]);
            if (mul(S[1], aj) != S[2]) { return false; }
            int j = log_[aj];
            if (j > 11) { return false; }
            cw[11 - j] ^= e;
            return true;
        }
        // calcula as 3 de paridade (usado nos testes)
        void encode(const uint8_t* data, uint8_t* parity) const {
            static const uint8_t G[3] = { 14, 56, 64 };
            uint8_t reg[3] = { 0, 0, 0 };
            for (int i = 0; i < 9; i++) {
                uint8_t fb = data[i] ^ reg[0];
                reg[0] = reg[1] ^ mul(fb, G[0]);
                reg[1] = reg[2] ^ mul(fb, G[1]);
                reg[2] = mul(fb, G[2]);
            }
            parity[0] = reg[0];
            parity[1] = reg[1];
            parity[2] = reg[2];
        }

    private:
        uint8_t exp_[512];
        uint8_t log_[256];
    };

    // infoDibits: 98 dibits (49 antes e 49 depois do sync), na ordem recebida.
    // dataType: 1 = Voice LC Header (máscara 0x96), 2 = Terminator (0x99).
    inline LC decodeLC(const uint8_t* infoDibits, int dataType) {
        LC lc;
        uint8_t raw[196], deint[196];
        for (int i = 0; i < 98; i++) {
            raw[2 * i] = (infoDibits[i] >> 1) & 1;
            raw[2 * i + 1] = infoDibits[i] & 1;
        }
        for (int i = 0; i < 196; i++) { deint[i] = raw[(i * 181) % 196]; }
        // matriz 13x15 a partir do bit 1; dados = colunas 0..10 das linhas 0..8,
        // sem os 3 primeiros bits reservados
        uint8_t bits[96];
        int n = 0;
        for (int row = 0; row < 9; row++) {
            for (int col = 0; col < 11; col++) {
                if (row == 0 && col < 3) { continue; }
                bits[n++] = deint[1 + row * 15 + col];
            }
        }
        uint8_t b[12];
        for (int i = 0; i < 12; i++) {
            uint8_t v = 0;
            for (int k = 0; k < 8; k++) { v = (uint8_t)((v << 1) | bits[i * 8 + k]); }
            b[i] = v;
        }
        uint8_t mask = (dataType == 1) ? 0x96 : 0x99;
        for (int i = 9; i < 12; i++) { b[i] ^= mask; }
        static const RS129 rs;
        if (!rs.check(b)) { return lc; }
        lc.valid = true;
        lc.flco = b[0] & 0x3F;
        lc.fid = b[1];
        lc.options = b[2];
        lc.dst = ((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 8) | b[5];
        lc.src = ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 8) | b[8];
        return lc;
    }
}
