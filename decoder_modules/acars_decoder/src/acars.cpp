#include "acars.h"
#include <cmath>
#include <map>
#include <random>
#include <algorithm>
#include <array>

namespace acars {

    static const double PI = 3.14159265358979323846;

    // ------------------------------------------------------------------ utilidades
    uint16_t crc16(const uint8_t* data, size_t len, uint16_t crc) {
        for (size_t i = 0; i < len; i++) {
            crc ^= data[i];
            for (int b = 0; b < 8; b++) { crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : (crc >> 1); }
        }
        return crc;
    }

    static uint8_t withParity(uint8_t c) {
        c &= 0x7F;
        int ones = 0;
        for (int i = 0; i < 7; i++) { ones += (c >> i) & 1; }
        return (ones % 2 == 0) ? (c | 0x80) : c;   // paridade ímpar
    }

    static bool parityOk(uint8_t c) {
        int ones = 0;
        for (int i = 0; i < 8; i++) { ones += (c >> i) & 1; }
        return ones % 2 == 1;
    }

    std::string sanitize(const std::string& s) {
        std::string o;
        o.reserve(s.size());
        for (unsigned char c : s) {
            if (c == '\r') { continue; }
            if (c == '\n') { o += '\n'; continue; }
            o += (c >= 32 && c < 127) ? (char)c : ' ';
        }
        return o;
    }

    std::string labelDescription(const std::string& l) {
        static const std::map<std::string, std::string> L = {
            { "_\x7f", "Sinal de vida (sem texto)" }, { "_d", "Sinal de vida (sem texto)" },
            { "Q0", "Teste de enlace" }, { "SQ", "Beacon da estação de solo" },
            { "H1", "Mensagem de sistema de bordo (texto livre/FANS)" }, { "H2", "Meteorologia (sensor)" },
            { "AA", "CPDLC / ATC (FANS-1/A)" }, { "BA", "CPDLC / ATC (FANS-1/A)" },
            { "A6", "ADS-C (pedido)" }, { "B6", "ADS-C (relatório de posição)" },
            { "A0", "FANS (aplicação)" }, { "AB", "Pedido de tempo/ATIS" },
            { "B9", "Pedido de ATIS" }, { "A9", "ATIS" }, { "A3", "Autorização de partida (DCL)" },
            { "B3", "Pedido de autorização (DCL)" }, { "A4", "Confirmação de autorização" },
            { "B1", "Pedido de autorização oceânica" }, { "A1", "Autorização oceânica" },
            { "5Z", "Mensagem da companhia" }, { "5U", "Meteorologia (pedido)" },
            { "C1", "Mensagem para a cabine" }, { "10", "Saída (OUT)" }, { "11", "Partida (OFF)" },
            { "12", "Pouso (ON)" }, { "13", "Chegada (IN)" }, { "14", "Previsão de chegada (ETA)" },
            { "15", "Relatório de voo" }, { "16", "Posição" }, { "20", "Relatório de atraso" },
            { "21", "Relatório de atraso" }, { "22", "Posição/estimado" }, { "2Z", "Posição/estimado" },
            { "30", "Combustível" }, { "33", "Combustível/ETA" }, { "44", "Posição/tempo" },
            { "4A", "Mensagem genérica" }, { "4N", "Mensagem genérica" }, { "54", "Voz pedida (SELCAL)" },
            { "57", "Relatório de posição" }, { "80", "Relatório OOOI" }, { "8D", "Manutenção" },
            { "8E", "Estimativa de chegada" }, { "QA", "Saída (OUT) + combustível" }, { "QB", "Partida (OFF)" },
            { "QC", "Pouso (ON)" }, { "QD", "Chegada (IN)" }, { "QE", "Saída/combustível" },
            { "QF", "Partida/destino" }, { "QG", "Retorno ao portão" }, { "QH", "Saída (OUT)" },
            { "QK", "Pouso" }, { "QL", "Chegada" }, { "QM", "Chegada + combustível" },
            { "QN", "Desvio" }, { "QP", "Saída (OUT)" }, { "QQ", "Partida (OFF)" }, { "QR", "Pouso (ON)" },
            { "QS", "Chegada (IN)" }, { "QT", "Saída/retorno" }, { "RA", "Comando para impressora" },
            { "SA", "Status de mídia" }, { "MA", "MIAM (mensagem comprimida)" },
        };
        auto it = L.find(l);
        if (it != L.end()) { return it->second; }
        if (l.size() == 2 && l[0] == 'H' && l[1] == '1') { return "Sistema de bordo"; }
        return "";
    }

    // ------------------------------------------------------------------ quadro
    // Bytes no ar: SOH | modo | matrícula(7) | ack | label(2) | bloco | STX texto | ETX/ETB | CRC(2) | DEL
    std::vector<uint8_t> buildFrame(const Message& m) {
        std::vector<uint8_t> f;
        f.push_back(withParity(m.mode ? m.mode : '2'));
        std::string reg = m.reg;
        while (reg.size() < 7) { reg = "." + reg; }
        for (int i = 0; i < 7; i++) { f.push_back(withParity(reg[i])); }
        f.push_back(withParity(m.ack ? m.ack : 0x15));
        std::string lbl = m.label;
        while (lbl.size() < 2) { lbl += ' '; }
        f.push_back(withParity(lbl[0]));
        f.push_back(withParity(lbl[1]));
        f.push_back(withParity(m.blockId ? m.blockId : (m.downlink ? '1' : 'A')));
        std::string text = m.text;
        if (m.downlink) {
            std::string no = m.msgNo, fl = m.flight;
            while (no.size() < 4) { no += ' '; }
            while (fl.size() < 6) { fl += ' '; }
            text = no.substr(0, 4) + fl.substr(0, 6) + text;
        }
        f.push_back(withParity(0x02));
        for (char c : text) { f.push_back(withParity(c)); }
        f.push_back(withParity(m.more ? 0x17 : 0x03));
        uint16_t crc = crc16(f.data(), f.size());
        f.push_back(crc & 0xFF);
        f.push_back(crc >> 8);
        return f;
    }

    bool parseFrame(const std::vector<uint8_t>& raw, Message& out) {
        if (raw.size() < 13) { return false; }
        Message m;
        m.mode = raw[0] & 0x7F;
        std::string reg;
        for (int i = 1; i <= 7; i++) { reg += (char)(raw[i] & 0x7F); }
        size_t p = reg.find_first_not_of('.');
        m.reg = (p == std::string::npos) ? "" : reg.substr(p);
        m.ack = raw[8] & 0x7F;
        m.label = std::string(1, (char)(raw[9] & 0x7F)) + (char)(raw[10] & 0x7F);
        if (m.label[1] == 0x7F) { m.label[1] = 'd'; }
        m.blockId = raw[11] & 0x7F;
        m.downlink = (m.blockId >= '0' && m.blockId <= '9');
        size_t i = 12;
        std::string text;
        uint8_t term = 0;
        if ((raw[i] & 0x7F) == 0x02) {
            for (i = 13; i < raw.size(); i++) {
                uint8_t c = raw[i] & 0x7F;
                if (c == 0x03 || c == 0x17) { term = c; break; }
                text += (char)c;
            }
        }
        else {
            term = raw[i] & 0x7F;
        }
        if (term != 0x03 && term != 0x17) { return false; }
        m.more = (term == 0x17);
        if (m.downlink && text.size() >= 10) {
            m.msgNo = text.substr(0, 4);
            m.flight = text.substr(4, 6);
            size_t a = m.flight.find_first_not_of(' '), b = m.flight.find_last_not_of(' ');
            m.flight = (a == std::string::npos) ? "" : m.flight.substr(a, b - a + 1);
            text = text.substr(10);
        }
        m.text = text;
        out = m;
        return true;
    }

    // Procura o fim (ETX/ETB) e valida o CRC. Caracteres com paridade errada são
    // corrigidos trocando os bits menos confiáveis (decisão suave) até o CRC fechar.
    static bool validate(std::vector<uint8_t>& bytes, const std::vector<std::array<uint8_t, 3>>& weak, int& corrected) {
        corrected = 0;
        for (size_t end = 12; end + 2 < bytes.size(); end++) {
            uint8_t c = bytes[end] & 0x7F;
            if (c != 0x03 && c != 0x17) { continue; }
            size_t n = end + 3;   // até o CRC
            if (crc16(bytes.data(), n) == 0) {
                bytes.resize(n);
                return true;
            }
            std::vector<size_t> bad;
            for (size_t k = 0; k <= end; k++) {
                if (!parityOk(bytes[k])) { bad.push_back(k); }
            }
            if (bad.empty() || bad.size() > 7) {
                if (bad.size() > 7) { return false; }
                continue;
            }
            // cada caractere ruim: tenta o 1º, 2º e 3º bit menos confiável (3^k combinações)
            int k = (int)bad.size();
            int combos = 1;
            for (int i = 0; i < k; i++) { combos *= 3; }
            std::vector<uint8_t> orig(k);
            for (int i = 0; i < k; i++) { orig[i] = bytes[bad[i]]; }
            for (int cmb = 0; cmb < combos; cmb++) {
                int x = cmb;
                for (int i = 0; i < k; i++) {
                    int pick = x % 3;
                    x /= 3;
                    uint8_t bit = (bad[i] < weak.size()) ? weak[bad[i]][pick] : (uint8_t)pick;
                    bytes[bad[i]] = orig[i] ^ (1 << bit);
                }
                if (crc16(bytes.data(), n) == 0) {
                    bytes.resize(n);
                    corrected = k;
                    return true;
                }
            }
            for (int i = 0; i < k; i++) { bytes[bad[i]] = orig[i]; }
        }
        return false;
    }

    // ------------------------------------------------------------------ demodulação
    static std::vector<float> lowpassTaps() {
        static std::vector<float> taps;
        if (!taps.empty()) { return taps; }
        const int N = 33;
        const double fc = 1500.0 / SAMPLE_RATE;
        double sum = 0;
        for (int i = 0; i < N; i++) {
            double x = i - (N - 1) / 2.0;
            double s = (x == 0) ? 2 * fc : std::sin(2 * PI * fc * x) / (PI * x);
            double w = 0.42 - 0.5 * std::cos(2 * PI * i / (N - 1)) + 0.08 * std::cos(4 * PI * i / (N - 1));
            taps.push_back((float)(s * w));
            sum += s * w;
        }
        for (auto& t : taps) { t /= (float)sum; }
        return taps;
    }

    // Áudio AM (tons de 1200/2400 Hz) levado para banda base em torno de 1800 Hz
    static std::vector<std::complex<float>> baseband(const std::vector<float>& env) {
        int n = (int)env.size();
        std::vector<float> audio(n);
        // tira a portadora: média móvel de 20 amostras anula 1200 e 2400 Hz
        const int MA = 20;
        std::vector<float> pref(n + 1, 0);
        for (int i = 0; i < n; i++) { pref[i + 1] = pref[i] + env[i]; }
        for (int i = 0; i < n; i++) {
            int a = (std::max)(0, i - MA / 2), b = (std::min)(n, i + MA / 2);
            float mean = (pref[b] - pref[a]) / (float)(b - a);
            audio[i] = env[i] - mean;
        }
        std::vector<std::complex<float>> z(n);
        for (int i = 0; i < n; i++) {
            double ph = -2 * PI * 1800.0 * i / SAMPLE_RATE;
            z[i] = std::complex<float>((float)std::cos(ph), (float)std::sin(ph)) * audio[i];
        }
        const std::vector<float>& t = lowpassTaps();
        int T = (int)t.size(), h = T / 2;
        std::vector<std::complex<float>> y(n);
        for (int i = 0; i < n; i++) {
            std::complex<float> s(0, 0);
            for (int k = 0; k < T; k++) {
                int j = i + k - h;
                if (j >= 0 && j < n) { s += z[j] * t[k]; }
            }
            y[i] = s;
        }
        return y;
    }

    static const uint32_t SYNC = 0x16 | (0x16 << 8) | (0x01 << 16);   // SYN SYN SOH (LSB primeiro)

    // Dada uma sequência de tons (1 = 2400 Hz) e sua confiabilidade, procura SYN SYN SOH
    // nas quatro convenções de dados e valida o quadro pelo CRC.
    static bool tryTones(const std::vector<uint8_t>& tones, const std::vector<float>& soft, Message& out) {
        int nbits = (int)tones.size();
        std::vector<uint8_t> data(nbits);
        std::vector<float> rel(nbits);
        for (int conv = 0; conv < 4; conv++) {
            for (int k = 0; k < nbits; k++) {
                uint8_t t = tones[k], prev = k ? tones[k - 1] : 0;
                switch (conv) {
                case 0: data[k] = t; break;
                case 1: data[k] = !t; break;
                case 2: data[k] = (t == prev); break;
                default: data[k] = (t != prev); break;
                }
                // confiabilidade do bit de dado (no diferencial depende dos dois tons)
                rel[k] = (conv < 2 || k == 0) ? soft[k] : (std::min)(soft[k], soft[k - 1]);
            }
            uint32_t reg = 0;
            for (int k = 0; k < nbits; k++) {
                reg = (reg >> 1) | ((uint32_t)data[k] << 23);
                if (k < 23) { continue; }
                uint32_t diff = reg ^ SYNC;
                int errs = 0;
                while (diff) { errs += diff & 1; diff >>= 1; }
                if (errs > 1) { continue; }
                // lê os bytes depois do SOH
                std::vector<uint8_t> bytes;
                std::vector<std::array<uint8_t, 3>> weak;
                for (int j = k + 1; j + 8 <= nbits && bytes.size() < 260; j += 8) {
                    uint8_t c = 0;
                    int order[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
                    for (int b = 0; b < 8; b++) { c |= data[j + b] << b; }
                    std::sort(order, order + 8, [&](int a, int b) { return rel[j + a] < rel[j + b]; });
                    bytes.push_back(c);
                    weak.push_back({ (uint8_t)order[0], (uint8_t)order[1], (uint8_t)order[2] });
                }
                int corr = 0;
                if (!validate(bytes, weak, corr)) { continue; }
                Message m;
                if (!parseFrame(bytes, m)) { continue; }
                m.corrected = corr;
                out = m;
                return true;
            }
        }
        return false;
    }

    bool decodeBurst(const std::vector<float>& envelope, Message& out) {
        if ((int)envelope.size() < SPB * 120) { return false; }
        std::vector<std::complex<float>> y = baseband(envelope);
        int n = (int)y.size();
        int nbits = n / SPB - 2;
        std::vector<uint8_t> tones(nbits);
        std::vector<float> soft(nbits);

        // 1) Discriminador de frequência (rápido; resolve sinais bons)
        std::vector<float> f(n, 0);
        for (int i = 1; i < n; i++) { f[i] = std::arg(y[i] * std::conj(y[i - 1])); }
        for (int phase = 0; phase < SPB; phase++) {
            for (int k = 0; k < nbits; k++) {
                float s = 0;
                int base = phase + k * SPB;
                for (int i = 2; i < SPB - 2; i++) { s += f[base + i]; }
                tones[k] = s > 0;
                soft[k] = std::fabs(s);
            }
            if (tryTones(tones, soft, out)) { return true; }
        }

        // 2) Demodulação coerente de MSK (uns 3 dB melhor em sinais fracos).
        //    Fase da portadora pelas raias de +-1200 Hz do sinal ao quadrado.
        std::complex<double> ap(0, 0), am(0, 0);
        for (int i = 0; i < n; i++) {
            std::complex<double> z2 = std::complex<double>(y[i]) * std::complex<double>(y[i]);
            double ph = 2 * PI * 1200.0 * i / SAMPLE_RATE;
            std::complex<double> e(std::cos(ph), std::sin(ph));
            ap += z2 * std::conj(e);
            am += z2 * e;
        }
        double theta0 = std::arg(ap * am) / 4.0;
        std::vector<float> w(2 * SPB + 1);
        for (int i = -SPB; i <= SPB; i++) { w[i + SPB] = (float)std::cos(PI * i / (2.0 * SPB)); }
        std::vector<float> x(nbits + 1);
        for (int m = 0; m < 2; m++) {
            double th = theta0 + m * PI / 2;
            std::complex<float> rot((float)std::cos(-th), (float)std::sin(-th));
            for (int tau = 0; tau < SPB; tau++) {
                for (int par = 0; par < 2; par++) {
                    for (int k = 0; k <= nbits; k++) {
                        int c = tau + SPB + k * SPB;
                        std::complex<float> acc(0, 0);
                        for (int i = -SPB; i <= SPB; i++) {
                            int j = c + i;
                            if (j >= 0 && j < n) { acc += y[j] * w[i + SPB]; }
                        }
                        acc *= rot;
                        x[k] = ((k + par) & 1) ? acc.imag() : acc.real();
                    }
                    for (int pat = 0; pat < 2; pat++) {
                        for (int k = 0; k < nbits; k++) {
                            uint8_t ck = x[k] > 0, cn = x[k + 1] > 0;
                            uint8_t p = pat ? ((k >> 1) & 1) : (k & 1);
                            tones[k] = (uint8_t)((ck != cn) ^ p);
                            soft[k] = (std::min)(std::fabs(x[k]), std::fabs(x[k + 1]));
                        }
                        if (tryTones(tones, soft, out)) { return true; }
                    }
                }
            }
        }
        return false;
    }

    // ------------------------------------------------------------------ síntese (testes)
    std::vector<float> synthesize(const Message& m, float snrDb, unsigned seed, int convention, double clockPpm) {
        std::vector<uint8_t> bytes;
        for (int i = 0; i < 16; i++) { bytes.push_back(0xFF); }
        bytes.push_back(withParity('+'));
        bytes.push_back(withParity('*'));
        bytes.push_back(0x16);
        bytes.push_back(0x16);
        bytes.push_back(0x01);
        for (uint8_t c : buildFrame(m)) { bytes.push_back(c); }
        bytes.push_back(0x7F);
        std::vector<uint8_t> bits;
        for (uint8_t c : bytes) {
            for (int b = 0; b < 8; b++) { bits.push_back((c >> b) & 1); }
        }
        // dados -> tons segundo a convenção
        std::vector<uint8_t> tones(bits.size());
        uint8_t prev = 0;
        for (size_t k = 0; k < bits.size(); k++) {
            uint8_t d = bits[k], t;
            switch (convention) {
            case 0: t = d; break;
            case 1: t = !d; break;
            case 2: t = d ? prev : !prev; break;
            default: t = d ? !prev : prev; break;
            }
            tones[k] = t;
            prev = t;
        }
        std::mt19937 rng(seed);
        std::normal_distribution<float> g(0, 1);
        float noise = std::pow(10.0f, -snrDb / 20.0f);
        std::vector<float> env;
        int lead = SAMPLE_RATE / 50;
        double phase = 0;
        for (int i = 0; i < lead; i++) { env.push_back(std::abs(std::complex<float>(1, 0) + std::complex<float>(g(rng), g(rng)) * noise)); }
        // relógio do modem fora do nominal: tons e taxa de bits escalados juntos
        double scale = 1.0 + clockPpm * 1e-6;
        double tpos = 0;
        for (size_t k = 0; k < tones.size(); k++) {
            double fr = (tones[k] ? 2400.0 : 1200.0) * scale;
            tpos += SPB / scale;
            while ((double)env.size() - lead < tpos) {
                phase += 2 * PI * fr / SAMPLE_RATE;
                float a = 1.0f + 0.5f * (float)std::sin(phase);
                env.push_back(std::abs(std::complex<float>(a, 0) + std::complex<float>(g(rng), g(rng)) * noise));
            }
        }
        for (int i = 0; i < lead; i++) { env.push_back(std::abs(std::complex<float>(1, 0) + std::complex<float>(g(rng), g(rng)) * noise)); }
        return env;
    }

    // ------------------------------------------------------------------ canal contínuo
    void ChannelDecoder::reset() {
        win.clear();
        pre.clear();
        buf.clear();
        floorPow = -1;
        winAcc = 0;
        winN = 0;
        burst = false;
        quiet = loud = 0;
    }

    float ChannelDecoder::noiseDb() const {
        return floorPow > 0 ? 10.0f * std::log10(floorPow) : -120.0f;
    }

    void ChannelDecoder::endBurst() {
        burst = false;
        bursts++;
        if ((int)buf.size() > SPB * 120) {
            Message m;
            if (decodeBurst(buf, m)) {
                decoded++;
                m.levelDb = floorPow > 0 ? 10.0f * std::log10(peakPow / floorPow) : 0;
                if (onMessage) { onMessage(m); }
            }
        }
        buf.clear();
    }

    void ChannelDecoder::process(const std::complex<float>* in, int count) {
        const int PRE_WIN = 4;
        const int MAX_LEN = SAMPLE_RATE * 3;
        for (int i = 0; i < count; i++) {
            float p = std::norm(in[i]);
            float e = std::sqrt(p);
            win.push_back(e);
            winAcc += p;
            if (++winN < WIN) { continue; }
            float pw = winAcc / WIN;
            winAcc = 0;
            winN = 0;
            if (floorPow < 0) { floorPow = pw; }
            if (!burst) {
                if (pw > floorPow * 5.0f) {
                    if (++loud >= 2) {
                        burst = true;
                        quiet = 0;
                        peakPow = pw;
                        buf = pre;
                        buf.insert(buf.end(), win.begin(), win.end());
                    }
                }
                else {
                    loud = 0;
                    if (pw < floorPow) { floorPow = 0.7f * floorPow + 0.3f * pw; }
                    else { floorPow += 0.01f * (pw - floorPow); }
                }
                pre.insert(pre.end(), win.begin(), win.end());
                if ((int)pre.size() > PRE_WIN * WIN) { pre.erase(pre.begin(), pre.end() - PRE_WIN * WIN); }
                if (burst) { pre.clear(); }
            }
            else {
                buf.insert(buf.end(), win.begin(), win.end());
                peakPow = (std::max)(peakPow, pw);
                if (pw < floorPow * 2.5f) {
                    if (++quiet >= 4) { endBurst(); loud = 0; }
                }
                else { quiet = 0; }
                if ((int)buf.size() > MAX_LEN) { endBurst(); loud = 0; }
            }
            win.clear();
        }
    }
}
