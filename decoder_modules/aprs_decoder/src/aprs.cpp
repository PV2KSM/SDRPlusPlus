#include "aprs.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace aprs {

    // ================================================================== AX.25
    uint16_t fcs(const uint8_t* b, int n) {
        uint16_t crc = 0xFFFF;
        for (int i = 0; i < n; i++) {
            crc ^= b[i];
            for (int k = 0; k < 8; k++) { crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1); }
        }
        return (uint16_t)~crc;
    }

    static std::string addrText(const uint8_t* a, bool& hbit) {
        std::string s;
        for (int i = 0; i < 6; i++) {
            char c = (char)(a[i] >> 1);
            if (c != ' ') { s += c; }
        }
        int ssid = (a[6] >> 1) & 0x0F;
        if (ssid) { s += "-" + std::to_string(ssid); }
        hbit = (a[6] & 0x80) != 0;
        return s;
    }

    static bool validCall(const std::string& s) {
        if (s.empty() || s.size() > 9) { return false; }
        for (char c : s) {
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) { return false; }
        }
        return true;
    }

    bool decodeAx25(const uint8_t* b, int n, Frame& f) {
        if (n < 16) { return false; }
        int p = 0;
        std::vector<std::string> addrs;
        bool last = false;
        while (!last) {
            if (p + 7 > n || addrs.size() >= 10) { return false; }
            bool h;
            std::string a = addrText(b + p, h);
            if (!validCall(a)) { return false; }
            last = b[p + 6] & 1;
            if (addrs.size() >= 2 && h) { a += "*"; }
            addrs.push_back(a);
            p += 7;
        }
        if (addrs.size() < 2 || p + 2 > n) { return false; }
        if (b[p] != 0x03 || b[p + 1] != 0xF0) { return false; }   // só quadros UI de APRS
        f.dst = addrs[0];
        f.src = addrs[1];
        f.path.assign(addrs.begin() + 2, addrs.end());
        f.direct = true;
        for (auto& d : f.path) {
            if (!d.empty() && d.back() == '*') { f.direct = false; }
        }
        f.info.assign((const char*)b + p + 2, n - p - 2);
        return true;
    }

    static void encodeAddr(std::vector<uint8_t>& out, std::string a, bool lastAddr) {
        bool h = !a.empty() && a.back() == '*';
        if (h) { a.pop_back(); }
        int ssid = 0;
        size_t d = a.find('-');
        if (d != std::string::npos) {
            ssid = atoi(a.c_str() + d + 1);
            a = a.substr(0, d);
        }
        for (int i = 0; i < 6; i++) { out.push_back((uint8_t)((i < (int)a.size() ? a[i] : ' ') << 1)); }
        out.push_back((uint8_t)(0x60 | ((ssid & 0x0F) << 1) | (h ? 0x80 : 0) | (lastAddr ? 1 : 0)));
    }

    std::vector<uint8_t> encodeAx25(const Frame& f) {
        std::vector<uint8_t> o;
        encodeAddr(o, f.dst, false);
        encodeAddr(o, f.src, f.path.empty());
        for (size_t i = 0; i < f.path.size(); i++) { encodeAddr(o, f.path[i], i + 1 == f.path.size()); }
        o.push_back(0x03);
        o.push_back(0xF0);
        o.insert(o.end(), f.info.begin(), f.info.end());
        uint16_t c = fcs(o.data(), (int)o.size());
        o.push_back((uint8_t)(c & 0xFF));
        o.push_back((uint8_t)(c >> 8));
        return o;
    }

    std::string Frame::tnc2() const {
        std::string s = src + ">" + dst;
        for (auto& p : path) { s += "," + p; }
        return s + ":" + info;
    }

    // ================================================================== demodulador AFSK
    Demodulator::Demodulator() {
        sl[0].gain = 1.0f;
        sl[1].gain = 0.55f;
        sl[2].gain = 1.8f;
        reset();
    }

    void Demodulator::reset() {
        memset(histM, 0, sizeof(histM));
        memset(histS, 0, sizeof(histS));
        sumM[0] = sumM[1] = sumS[0] = sumS[1] = 0;
        pos = 0;
        phM = phS = 0;
        dcAvg = 0;
        mPeak = sPeak = 1e-6f;
        for (auto& s : sl) {
            s.pll = 0;
            s.prevD = 0;
            s.prevBit = 0;
            s.flagReg = 0;
            s.ones = 0;
            s.bits.clear();
            s.inFrame = false;
        }
    }

    void Demodulator::process(const float* audio, int count) {
        const double wM = 2.0 * M_PI * 1200.0 / SAMPLE_RATE, wS = 2.0 * M_PI * 2200.0 / SAMPLE_RATE;
        const int64_t STEP = (int64_t)(4294967296.0 / N);
        for (int i = 0; i < count; i++) {
            // tira o nível DC (desvio do centro do canal)
            float x = audio[i];
            dcAvg += 0.002f * (x - dcAvg);
            x -= dcAvg;
            // correlação com 1200 e 2200 Hz na janela de um bit (soma deslizante)
            float iM = x * (float)cos(phM), qM = x * (float)sin(phM);
            float iS = x * (float)cos(phS), qS = x * (float)sin(phS);
            phM += wM;
            phS += wS;
            if (phM > 2 * M_PI) { phM -= 2 * M_PI; }
            if (phS > 2 * M_PI) { phS -= 2 * M_PI; }
            sumM[0] += iM - histM[0][pos];
            sumM[1] += qM - histM[1][pos];
            sumS[0] += iS - histS[0][pos];
            sumS[1] += qS - histS[1][pos];
            histM[0][pos] = iM;
            histM[1][pos] = qM;
            histS[0][pos] = iS;
            histS[1][pos] = qS;
            pos = (pos + 1) % N;
            if (++resum >= 4800) {
                // refaz as somas de vez em quando (erro de arredondamento acumulado)
                resum = 0;
                for (int k = 0; k < 2; k++) {
                    sumM[k] = sumS[k] = 0;
                    for (int j = 0; j < N; j++) {
                        sumM[k] += histM[k][j];
                        sumS[k] += histS[k][j];
                    }
                }
            }
            float m = (float)std::sqrt(sumM[0] * sumM[0] + sumM[1] * sumM[1]);
            float s = (float)std::sqrt(sumS[0] * sumS[0] + sumS[1] * sumS[1]);
            // AGC separado por tom: o rádio pode chegar com o 2200 bem mais fraco (pré-ênfase)
            mPeak = m > mPeak ? mPeak + 0.3f * (m - mPeak) : mPeak - 0.0002f * mPeak;
            sPeak = s > sPeak ? sPeak + 0.3f * (s - sPeak) : sPeak - 0.0002f * sPeak;
            float mn = m / (mPeak + 1e-9f), sn = s / (sPeak + 1e-9f);
            sinceLast++;

            for (auto& sv : sl) {
                float d = mn - sv.gain * sn;
                // relógio: PLL digital que estoura uma vez por bit; nas transições puxa a fase
                int32_t prev = sv.pll;
                sv.pll = (int32_t)((uint32_t)sv.pll + (uint32_t)STEP);
                if (prev > 0 && sv.pll < 0) {
                    int raw = d > 0 ? 1 : 0;   // 1 = marca (1200 Hz)
                    int b = (raw == sv.prevBit) ? 1 : 0;   // NRZI
                    sv.prevBit = raw;
                    bit(sv, b);
                }
                if ((d > 0) != (sv.prevD > 0)) { sv.pll = (int32_t)((double)sv.pll * (sv.inFrame ? 0.74 : 0.5)); }
                sv.prevD = d;
            }
        }
    }

    void Demodulator::bit(Slicer& s, int b) {
        s.flagReg = (uint8_t)((s.flagReg >> 1) | (b << 7));
        if (s.flagReg == 0x7E) {
            if (s.inFrame && s.bits.size() >= 7) {
                s.bits.resize(s.bits.size() - 7);   // tira o começo da própria flag
                endFrame(s);
            }
            s.bits.clear();
            s.inFrame = true;
            s.ones = 0;
            return;
        }
        if (!s.inFrame) { return; }
        if (b) {
            if (++s.ones >= 7) {   // sete 1 seguidos: aborto/ruído
                s.inFrame = false;
                s.bits.clear();
                return;
            }
            s.bits.push_back(1);
        }
        else {
            if (s.ones == 5) {   // 0 de enchimento
                s.ones = 0;
                return;
            }
            s.ones = 0;
            s.bits.push_back(0);
        }
        if (s.bits.size() > 8 * 400) {
            s.inFrame = false;
            s.bits.clear();
        }
    }

    void Demodulator::endFrame(Slicer& s) {
        size_t nb = s.bits.size();
        if (nb < 8 * 18 || nb % 8 != 0) { return; }
        std::vector<uint8_t> by(nb / 8);
        for (size_t i = 0; i < by.size(); i++) {
            uint8_t v = 0;
            for (int k = 0; k < 8; k++) { v |= (uint8_t)(s.bits[i * 8 + k] << k); }   // LSB primeiro
            by[i] = v;
        }
        int n = (int)by.size();
        uint16_t got = (uint16_t)(by[n - 2] | (by[n - 1] << 8));
        if (fcs(by.data(), n - 2) != got) {
            badFcs++;
            return;
        }
        by.resize(n - 2);
        // os três fatiadores costumam pegar o mesmo quadro: entrega uma vez só
        if (by == lastFrame && sinceLast < (int)SAMPLE_RATE) { return; }
        lastFrame = by;
        sinceLast = 0;
        Frame f;
        if (!decodeAx25(by.data(), (int)by.size(), f)) { return; }
        frames++;
        if (onFrame) { onFrame(f); }
    }

    std::vector<float> synthesize(const Frame& f, int preambleFlags) {
        std::vector<uint8_t> by = encodeAx25(f);
        std::vector<int> bits;
        for (int i = 0; i < preambleFlags; i++) {
            for (int k = 0; k < 8; k++) { bits.push_back((0x7E >> k) & 1); }
        }
        int ones = 0;
        for (uint8_t v : by) {
            for (int k = 0; k < 8; k++) {
                int b = (v >> k) & 1;
                bits.push_back(b);
                if (b) {
                    if (++ones == 5) {
                        bits.push_back(0);
                        ones = 0;
                    }
                }
                else { ones = 0; }
            }
        }
        for (int i = 0; i < 4; i++) {
            for (int k = 0; k < 8; k++) { bits.push_back((0x7E >> k) & 1); }
        }
        std::vector<float> out;
        double ph = 0;
        bool mark = true;
        const int sps = (int)(SAMPLE_RATE / 1200.0);
        for (int b : bits) {
            if (!b) { mark = !mark; }   // NRZI: 0 troca o tom
            double w = 2.0 * M_PI * (mark ? 1200.0 : 2200.0) / SAMPLE_RATE;
            for (int i = 0; i < sps; i++) {
                out.push_back((float)std::sin(ph));
                ph += w;
            }
        }
        return out;
    }

    // ================================================================== APRS
    static bool digitsOrSpace(const std::string& s, size_t a, size_t n) {
        if (a + n > s.size()) { return false; }
        for (size_t i = a; i < a + n; i++) {
            if (!(isdigit((unsigned char)s[i]) || s[i] == ' ')) { return false; }
        }
        return true;
    }

    static double num(std::string t) {
        for (auto& c : t) {
            if (c == ' ') { c = '0'; }   // ambiguidade de posição
        }
        return atof(t.c_str());
    }

    static void takeExtras(Report& r) {
        // "ccc/sss" no início do comentário: rumo e velocidade (nós)
        std::string& c = r.comment;
        if (c.size() >= 7 && isdigit((unsigned char)c[0]) && isdigit((unsigned char)c[1]) && isdigit((unsigned char)c[2]) && c[3] == '/' &&
            isdigit((unsigned char)c[4]) && isdigit((unsigned char)c[5]) && isdigit((unsigned char)c[6])) {
            r.course = atoi(c.substr(0, 3).c_str());
            r.speedKmh = (int)std::lround(atoi(c.substr(4, 3).c_str()) * 1.852);
            c = c.substr(7);
        }
        // "/A=001234": altitude em pés
        size_t a = c.find("/A=");
        if (a != std::string::npos && a + 9 <= c.size()) {
            r.altM = atof(c.substr(a + 3, 6).c_str()) * 0.3048;
            c.erase(a, 9);
        }
    }

    // Posição a partir de p: normal ("ddmm.mmN/dddmm.mmW>") ou comprimida (base 91)
    static bool parsePos(const std::string& s, size_t p, Report& r) {
        if (p + 13 > s.size()) { return false; }
        if (digitsOrSpace(s, p, 4) && s[p + 4] == '.' && digitsOrSpace(s, p + 5, 2) && p + 19 <= s.size()) {
            char ns = s[p + 7], ew = s[p + 17];
            if ((ns != 'N' && ns != 'S') || (ew != 'E' && ew != 'W')) { return false; }
            double lat = num(s.substr(p, 2)) + num(s.substr(p + 2, 5)) / 60.0;
            double lon = num(s.substr(p + 9, 3)) + num(s.substr(p + 12, 5)) / 60.0;
            if (ns == 'S') { lat = -lat; }
            if (ew == 'W') { lon = -lon; }
            r.symTable = s[p + 8];
            r.symCode = s[p + 18];
            r.lat = lat;
            r.lon = lon;
            r.hasPos = std::fabs(lat) <= 90 && std::fabs(lon) <= 180;
            r.comment = s.substr(p + 19);
            takeExtras(r);
            return r.hasPos;
        }
        char t = s[p];
        if (t == '/' || t == '\\' || (t >= 'A' && t <= 'Z') || (t >= 'a' && t <= 'j')) {
            long y = 0, x = 0;
            for (int i = 0; i < 4; i++) {
                int cy = (unsigned char)s[p + 1 + i] - 33, cx = (unsigned char)s[p + 5 + i] - 33;
                if (cy < 0 || cy > 90 || cx < 0 || cx > 90) { return false; }
                y = y * 91 + cy;
                x = x * 91 + cx;
            }
            r.symTable = t;
            r.symCode = s[p + 9];
            r.lat = 90.0 - y / 380926.0;
            r.lon = -180.0 + x / 190463.0;
            char c = s[p + 10], sp = s[p + 11], ty = s[p + 12];
            if (c != ' ' && ((ty - 33) & 0x18) != 0x10 && c >= '!' && c <= 'z') {
                r.course = (c - 33) * 4;
                r.speedKmh = (int)std::lround((std::pow(1.08, sp - 33) - 1.0) * 1.852);
            }
            r.hasPos = true;
            r.comment = s.substr(p + 13);
            takeExtras(r);
            return true;
        }
        return false;
    }

    // Mic-E: latitude no destino, longitude/velocidade nos bytes seguintes (HTs Kenwood/Yaesu)
    static bool parseMicE(const Frame& f, Report& r) {
        std::string d = f.dst;
        size_t dash = d.find('-');
        if (dash != std::string::npos) { d = d.substr(0, dash); }
        const std::string& s = f.info;
        if (d.size() < 6 || s.size() < 9) { return false; }
        int dig[6];
        for (int i = 0; i < 6; i++) {
            char c = d[i];
            if (c >= '0' && c <= '9') { dig[i] = c - '0'; }
            else if (c >= 'A' && c <= 'J') { dig[i] = c - 'A'; }
            else if (c >= 'P' && c <= 'Y') { dig[i] = c - 'P'; }
            else if (c == 'K' || c == 'L' || c == 'Z') { dig[i] = 0; }
            else { return false; }
        }
        double lat = dig[0] * 10 + dig[1] + (dig[2] * 10 + dig[3] + (dig[4] * 10 + dig[5]) / 100.0) / 60.0;
        bool north = d[3] >= 'P' && d[3] <= 'Z';
        bool off100 = d[4] >= 'P' && d[4] <= 'Z';
        bool west = d[5] >= 'P' && d[5] <= 'Z';
        int ld = (unsigned char)s[1] - 28;
        if (off100) { ld += 100; }
        if (ld >= 180 && ld <= 189) { ld -= 80; }
        else if (ld >= 190 && ld <= 199) { ld -= 190; }
        int lm = (unsigned char)s[2] - 28;
        if (lm >= 60) { lm -= 60; }
        int lh = (unsigned char)s[3] - 28;
        double lon = ld + (lm + lh / 100.0) / 60.0;
        if (lat > 90 || lon > 180 || lm < 0 || lh < 0) { return false; }
        r.lat = north ? lat : -lat;
        r.lon = west ? -lon : lon;
        int sp = ((unsigned char)s[4] - 28) * 10 + ((unsigned char)s[5] - 28) / 10;
        int dc = (((unsigned char)s[5] - 28) % 10) * 100 + ((unsigned char)s[6] - 28);
        if (sp >= 800) { sp -= 800; }
        if (dc >= 400) { dc -= 400; }
        r.speedKmh = (int)std::lround(sp * 1.852);
        r.course = dc;
        r.symCode = s[7];
        r.symTable = s[8];
        r.hasPos = true;
        std::string c = s.substr(9);
        if (!c.empty() && (c[0] == '>' || c[0] == ']' || c[0] == '`' || c[0] == '\'')) { c = c.substr(1); }
        // altitude: "xxx}" em base 91, metros acima de -10 km
        if (c.size() >= 4 && c[3] == '}') {
            r.altM = ((c[0] - 33) * 91 * 91 + (c[1] - 33) * 91 + (c[2] - 33)) - 10000.0;
            c = c.substr(4);
        }
        // código do modelo do rádio no fim (ex.: "_%" Yaesu, "=" TH-D72)
        if (c.size() >= 2 && (c[c.size() - 2] == '_' || c[c.size() - 2] == '`' || c[c.size() - 2] == '\'')) { c.resize(c.size() - 2); }
        else if (!c.empty() && (c.back() == '=' || c.back() == '^')) { c.pop_back(); }
        r.comment = c;
        return true;
    }

    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
        return a == std::string::npos ? "" : s.substr(a, b - a + 1);
    }

    bool parse(const Frame& f, Report& r) {
        r = Report();
        r.name = f.src;
        const std::string& s = f.info;
        if (s.empty()) { return false; }
        char t = s[0];
        switch (t) {
        case '!':
        case '=':
            r.type = Report::POSITION;
            parsePos(s, 1, r);
            break;
        case '/':
        case '@':
            r.type = Report::POSITION;
            parsePos(s, 8, r);
            break;
        case '`':
        case '\'':
        case 0x1c:
        case 0x1d:
            r.type = Report::MICE;
            parseMicE(f, r);
            break;
        case ';':
            r.type = Report::OBJECT;
            if (s.size() >= 18) {
                r.name = trim(s.substr(1, 9));
                r.killed = s[10] == '_';
                parsePos(s, 18, r);
            }
            break;
        case ')': {
            r.type = Report::ITEM;
            size_t e = s.find_first_of("!_", 1);
            if (e != std::string::npos && e <= 10) {
                r.name = s.substr(1, e - 1);
                r.killed = s[e] == '_';
                parsePos(s, e + 1, r);
            }
            break;
        }
        case ':':
            r.type = Report::MESSAGE;
            if (s.size() >= 11 && s[10] == ':') {
                r.msgTo = trim(s.substr(1, 9));
                r.msgText = s.substr(11);
                size_t b = r.msgText.rfind('{');
                if (b != std::string::npos) {
                    r.msgId = r.msgText.substr(b + 1);
                    r.msgText = r.msgText.substr(0, b);
                }
            }
            break;
        case '>':
            r.type = Report::STATUS;
            r.comment = s.substr(1);
            break;
        case '_':
            r.type = Report::WEATHER;
            r.comment = s.substr(1);
            break;
        case 'T':
            r.type = Report::TELEMETRY;
            r.comment = s.substr(1);
            break;
        case '}': {
            // terceiro (ex.: vindo da internet por um igate): "SRC>DST,PATH:info"
            std::string in = s.substr(1);
            size_t gt = in.find('>'), co = in.find(':');
            if (gt == std::string::npos || co == std::string::npos || gt > co) { break; }
            Frame g;
            g.src = in.substr(0, gt);
            std::string hdr = in.substr(gt + 1, co - gt - 1);
            size_t cm = hdr.find(',');
            g.dst = hdr.substr(0, cm);
            g.info = in.substr(co + 1);
            if (g.info.empty() || g.info[0] == '}') { break; }
            return parse(g, r);
        }
        default: {
            // alguns rastreadores põem '!' no meio dos primeiros 40 caracteres
            size_t b = s.find('!');
            if (b != std::string::npos && b < 40 && parsePos(s, b + 1, r)) { r.type = Report::POSITION; }
            else { r.comment = s; }
        }
        }
        // limpa caracteres de controle do texto
        for (auto* str : { &r.comment, &r.msgText }) {
            for (auto& c : *str) {
                if ((unsigned char)c < 32 || c == 127) { c = ' '; }
            }
            *str = trim(*str);
        }
        return true;
    }

    const char* typeName(Report::Type t) {
        switch (t) {
        case Report::POSITION: return "posição";
        case Report::MICE: return "posição (Mic-E)";
        case Report::OBJECT: return "objeto";
        case Report::ITEM: return "item";
        case Report::MESSAGE: return "mensagem";
        case Report::STATUS: return "status";
        case Report::WEATHER: return "meteorologia";
        case Report::TELEMETRY: return "telemetria";
        default: return "outro";
        }
    }

    const char* symbolName(char table, char code) {
        if (table == '/') {
            switch (code) {
            case '!': return "polícia";
            case '#': return "digipeater";
            case '$': return "telefone";
            case '&': return "gateway HF";
            case '\'': return "avião pequeno";
            case '-': return "casa";
            case '<': return "moto";
            case '=': return "trem";
            case '>': return "carro";
            case 'O': return "balão";
            case 'P': return "polícia";
            case 'R': return "motorhome";
            case 'U': return "ônibus";
            case 'X': return "helicóptero";
            case 'Y': return "veleiro";
            case '[': return "pessoa";
            case '^': return "avião";
            case '_': return "meteorologia";
            case 'a': return "ambulância";
            case 'b': return "bicicleta";
            case 'f': return "bombeiros";
            case 'h': return "hospital";
            case 'j': return "jipe";
            case 'k': return "caminhonete";
            case 'l': return "computador";
            case 'n': return "nó";
            case 'r': return "repetidora";
            case 's': return "barco";
            case 'u': return "caminhão";
            case 'v': return "van";
            case 'y': return "casa com antena";
            default: return "";
            }
        }
        switch (code) {
        case '#': return "digipeater";
        case '&': return table == 'I' ? "igate" : "gateway";
        case '-': return "casa";
        case '>': return "carro";
        case '_': return "meteorologia";
        case 'a': return "ARES";
        case 'k': return "SUV";
        case 'u': return "caminhão";
        case 'r': return "repetidora";
        default: return "";
        }
    }
}
