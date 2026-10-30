#include "modes.h"
#include <cmath>
#include <cstring>
#include <climits>
#include <algorithm>
#include <cstdio>

namespace modes {

    static const double PI = 3.14159265358979323846;

    // ------------------------------------------------------------ CRC / util

    uint32_t crc24(const uint8_t* msg, int bits) {
        int n = bits / 8;
        uint32_t crc = 0;
        for (int i = 0; i < n - 3; i++) {
            crc ^= ((uint32_t)msg[i]) << 16;
            for (int b = 0; b < 8; b++) {
                crc = (crc & 0x800000) ? ((crc << 1) ^ 0xFFF409) : (crc << 1);
                crc &= 0xFFFFFF;
            }
        }
        uint32_t parity = ((uint32_t)msg[n - 3] << 16) | ((uint32_t)msg[n - 2] << 8) | msg[n - 1];
        return crc ^ parity;
    }

    int messageBits(int df) {
        return (df & 0x10) ? LONG_MSG_BITS : SHORT_MSG_BITS;
    }

    bool hexToBytes(const std::string& hex, uint8_t* out, int& bits) {
        if (hex.size() != 14 && hex.size() != 28) { return false; }
        for (size_t i = 0; i < hex.size(); i += 2) {
            unsigned v;
            if (sscanf(hex.substr(i, 2).c_str(), "%2x", &v) != 1) { return false; }
            out[i / 2] = (uint8_t)v;
        }
        bits = (int)hex.size() * 4;
        return true;
    }

    // ------------------------------------------------------------ demodulador

    void Demodulator::setSampleRate(double sampleRate) {
        spu = sampleRate / 1e6;
        fullLen = (int)ceil((8.0 + LONG_MSG_BITS) * spu) + 4;
        tail.clear();
    }

    // Energia integrada entre as posições a e b (em amostras, fracionárias)
    double Demodulator::energy(double a, double b) const {
        auto at = [this](double x) {
            if (x <= 0) { return csum[0]; }
            size_t i = (size_t)x;
            if (i + 1 >= csum.size()) { return csum.back(); }
            double f = x - (double)i;
            return csum[i] + (csum[i + 1] - csum[i]) * f;
        };
        return at(b) - at(a);
    }

    // Preâmbulo: pulsos de 0,5 µs em 0; 1,0; 3,5 e 4,5 µs, silêncio no resto até 8 µs
    double Demodulator::preambleScore(double t, double noise, bool& ok) const {
        double h = 0.5 * spu;
        double p0 = energy(t, t + h);
        double p1 = energy(t + 1.0 * spu, t + 1.0 * spu + h);
        double p2 = energy(t + 3.5 * spu, t + 3.5 * spu + h);
        double p3 = energy(t + 4.5 * spu, t + 4.5 * spu + h);
        double q0 = energy(t + h, t + 1.0 * spu);
        double q1 = energy(t + 1.5 * spu, t + 3.5 * spu);
        double q2 = energy(t + 5.0 * spu, t + 8.0 * spu);
        double hp = (p0 + p1 + p2 + p3) / (4.0 * h);          // nível médio dos pulsos
        double lq = (q0 + q1 + q2) / (5.5 * spu);              // nível médio do silêncio
        double pmin = (std::min)((std::min)(p0, p1), (std::min)(p2, p3)) / h;
        ok = hp > 2.0 * lq && pmin > 1.4 * lq && hp > 2.2 * noise;
        return hp - lq;
    }

    bool Demodulator::validate(uint8_t* data, Message& out) {
        int df = data[0] >> 3;
        int bits = messageBits(df);
        uint32_t rem = crc24(data, bits);
        bool ok = false, corrected = false;
        uint32_t icao = 0;

        if (df == 11) {
            // Os 7 bits baixos podem trazer o IID do interrogador
            if ((rem & 0xFFFF80) == 0) {
                ok = true;
                icao = ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
            }
        }
        else if (df == 17 || df == 18) {
            if (rem == 0) { ok = true; }
            else {
                // Corrige 1 bit errado (fora do campo DF)
                for (int i = 5; i < bits && !ok; i++) {
                    data[i / 8] ^= (uint8_t)(1 << (7 - (i % 8)));
                    if (crc24(data, bits) == 0) { ok = true; corrected = true; }
                    else { data[i / 8] ^= (uint8_t)(1 << (7 - (i % 8))); }
                }
            }
            if (ok) { icao = ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3]; }
        }
        else if (df == 0 || df == 4 || df == 5 || df == 16 || df == 20 || df == 21) {
            // Endereço vem "misturado" no CRC: só aceita se já conhecemos o avião
            icao = rem;
            ok = isKnownIcao && isKnownIcao(icao);
        }
        if (!ok) { return false; }
        memcpy(out.data, data, 14);
        out.bits = bits;
        out.df = df;
        out.icao = icao;
        out.crcOk = true;
        out.corrected = corrected;
        return true;
    }

    bool Demodulator::decodeAt(double t, Message& out) {
        uint8_t data[14] = { 0 };
        float conf[LONG_MSG_BITS];
        double h = 0.5 * spu;
        double power = 0;
        double start = t + 8.0 * spu;
        int nbits = LONG_MSG_BITS;
        for (int i = 0; i < LONG_MSG_BITS; i++) {
            double b0 = start + i * spu;
            double a = energy(b0, b0 + h), b = energy(b0 + h, b0 + 2 * h);
            if (a > b) { data[i / 8] |= (uint8_t)(1 << (7 - (i % 8))); }
            conf[i] = (float)(fabs(a - b) / (a + b + 1e-12));
            double hi = (std::max)(a, b) / h;
            power += hi * hi;
            // Mensagem curta: não precisamos dos 56 bits seguintes
            if (i == SHORT_MSG_BITS - 1 && messageBits(data[0] >> 3) == SHORT_MSG_BITS) {
                power *= (double)LONG_MSG_BITS / SHORT_MSG_BITS;
                nbits = SHORT_MSG_BITS;
                break;
            }
        }
        bool ok = validate(data, out);

        // Sinal fraco: tenta inverter combinações dos bits menos confiáveis.
        // Só para DF17 de aviões já confirmados com CRC limpo (evita aviões falsos).
        if (!ok && (data[0] >> 3) == 17 && nbits == LONG_MSG_BITS && isKnownIcao) {
            uint32_t icao = ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
            int idx[LONG_MSG_BITS];
            for (int i = 0; i < LONG_MSG_BITS; i++) { idx[i] = i; }
            const int N = 10;
            std::partial_sort(idx + 5, idx + 5 + N, idx + LONG_MSG_BITS, [&](int x, int y) { return conf[x] < conf[y]; });
            auto flip = [&](int i) { data[i / 8] ^= (uint8_t)(1 << (7 - (i % 8))); };
            for (int x = 5; x < 5 + N && !ok; x++) {
                flip(idx[x]);
                for (int y = x + 1; y < 5 + N && !ok; y++) {
                    flip(idx[y]);
                    if (crc24(data, LONG_MSG_BITS) == 0) {
                        uint32_t ic = ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
                        if (ic == icao || isKnownIcao(ic)) { ok = validate(data, out); }
                    }
                    if (!ok) { flip(idx[y]); }
                }
                if (!ok) { flip(idx[x]); }
            }
            if (ok) { out.corrected = true; }
        }
        if (!ok) { return false; }
        out.signalDb = (float)(10.0 * log10(power / LONG_MSG_BITS + 1e-12));
        return true;
    }

    void Demodulator::process(const float* mag, int count) {
        buf.resize(tail.size() + count);
        if (!tail.empty()) { memcpy(buf.data(), tail.data(), tail.size() * sizeof(float)); }
        memcpy(buf.data() + tail.size(), mag, count * sizeof(float));

        int size = (int)buf.size();
        csum.resize(size + 1);
        csum[0] = 0;
        double total = 0;
        for (int i = 0; i < size; i++) {
            total += buf[i];
            csum[i + 1] = total;
        }
        // Ruído: mediana de uma amostragem do bloco (não é puxada pelas mensagens)
        double noise = 0;
        if (size > 0) {
            noiseSample.clear();
            for (int i = 0; i < size; i += 7) { noiseSample.push_back(buf[i]); }
            std::nth_element(noiseSample.begin(), noiseSample.begin() + noiseSample.size() / 2, noiseSample.end());
            noise = noiseSample[noiseSample.size() / 2];
        }

        int last = size - fullLen;
        int j = 0;
        Message msg;
        int refine = (int)ceil(spu);
        while (j <= last) {
            bool ok;
            preambleScore(j, noise, ok);
            if (!ok) { j++; continue; }
            preambles++;

            // Acha o alinhamento fracionário com o preâmbulo mais nítido
            double best = j;
            double bestScore = -1e30;
            bool bok;
            for (double t = j - 1.0; t <= j + refine; t += 0.25) {
                if (t < 0 || t > last) { continue; }
                double sc = preambleScore(t, noise, bok);
                if (sc > bestScore) { bestScore = sc; best = t; }
            }
            bool found = false;
            static const double offs[] = { 0.0, -0.25, 0.25, -0.5, 0.5 };
            for (double o : offs) {
                double t = best + o;
                if (t < 0 || t > last) { continue; }
                if (decodeAt(t, msg)) {
                    found = true;
                    accepted++;
                    if (onMessage) { onMessage(msg); }
                    j = (int)ceil(t + (8.0 + msg.bits) * spu);
                    break;
                }
            }
            if (!found) {
                rejected++;
                j = (std::max)(j + 1, (int)best + 1);
            }
        }
        // Guarda o que ainda não pôde ser analisado
        int keepFrom = (std::max)(j, 0);
        if (keepFrom < size) { tail.assign(buf.begin() + keepFrom, buf.end()); }
        else { tail.clear(); }
    }

    // ------------------------------------------------------------ campos

    int decodeAc12(uint32_t ac12) {
        if (ac12 == 0) { return INT_MIN; }
        if (!((ac12 >> 4) & 1)) { return INT_MIN; }   // Gillham (raro): não suportado
        int n = (int)(((ac12 & 0xFE0) >> 1) | (ac12 & 0x0F));
        return n * 25 - 1000;
    }

    int decodeAc13(uint32_t ac13) {
        if (ac13 == 0) { return INT_MIN; }
        bool mBit = (ac13 >> 6) & 1;
        bool qBit = (ac13 >> 4) & 1;
        if (mBit || !qBit) { return INT_MIN; }        // metros / Gillham: não suportado
        int n = (int)(((ac13 & 0x1F80) >> 2) | ((ac13 & 0x0020) >> 1) | (ac13 & 0x000F));
        return n * 25 - 1000;
    }

    int decodeId13(uint32_t id) {
        auto bit = [id](int pos) { return (int)((id >> pos) & 1); };
        int a = bit(7) * 4 + bit(9) * 2 + bit(11);
        int b = bit(1) * 4 + bit(3) * 2 + bit(5);
        int c = bit(8) * 4 + bit(10) * 2 + bit(12);
        int d = bit(0) * 4 + bit(2) * 2 + bit(4);
        return a * 1000 + b * 100 + c * 10 + d;
    }

    static std::string decodeCallsign(const uint8_t* p) {
        static const char* cs = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";
        uint64_t v = 0;
        for (int i = 0; i < 6; i++) { v = (v << 8) | p[i]; }
        std::string s;
        for (int i = 0; i < 8; i++) {
            char c = cs[(v >> (42 - 6 * i)) & 0x3F];
            if (c == '#') { return ""; }   // caractere inválido
            s += c;
        }
        while (!s.empty() && s.back() == ' ') { s.pop_back(); }
        size_t st = s.find_first_not_of(' ');
        return st == std::string::npos ? "" : s.substr(st);
    }

    // ------------------------------------------------------------ CPR

    int cprNL(double lat) {
        lat = fabs(lat);
        if (lat < 1e-9) { return 59; }
        if (fabs(lat - 87.0) < 1e-9) { return 2; }
        if (lat > 87.0) { return 1; }
        const double nz = 15.0;
        double a = 1.0 - cos(PI / (2.0 * nz));
        double b = cos(PI / 180.0 * lat);
        return (int)floor(2.0 * PI / acos(1.0 - a / (b * b)));
    }

    static double pmod(double a, double b) {
        double r = fmod(a, b);
        return r < 0 ? r + b : r;
    }

    bool cprGlobal(int latE, int lonE, int latO, int lonO, bool oddIsNewer, double& lat, double& lon) {
        const double s = 131072.0;
        double le = latE / s, lo = latO / s, ge = lonE / s, go = lonO / s;
        double j = floor(59.0 * le - 60.0 * lo + 0.5);
        double rlatE = (360.0 / 60.0) * (pmod(j, 60.0) + le);
        double rlatO = (360.0 / 59.0) * (pmod(j, 59.0) + lo);
        if (rlatE >= 270.0) { rlatE -= 360.0; }
        if (rlatO >= 270.0) { rlatO -= 360.0; }
        if (cprNL(rlatE) != cprNL(rlatO)) { return false; }
        lat = oddIsNewer ? rlatO : rlatE;
        int nl = cprNL(lat);
        int ni = (std::max)(nl - (oddIsNewer ? 1 : 0), 1);
        double m = floor(ge * (nl - 1) - go * nl + 0.5);
        lon = (360.0 / ni) * (pmod(m, ni) + (oddIsNewer ? go : ge));
        if (lon >= 180.0) { lon -= 360.0; }
        return lat >= -90.0 && lat <= 90.0;
    }

    void cprLocal(double refLat, double refLon, int latCpr, int lonCpr, int fflag, double& lat, double& lon) {
        const double s = 131072.0;
        double dLat = 360.0 / (60.0 - fflag);
        double j = floor(refLat / dLat) + floor(0.5 + pmod(refLat, dLat) / dLat - latCpr / s);
        lat = dLat * (j + latCpr / s);
        double dLon = 360.0 / (std::max)(cprNL(lat) - fflag, 1);
        double m = floor(refLon / dLon) + floor(0.5 + pmod(refLon, dLon) / dLon - lonCpr / s);
        lon = dLon * (m + lonCpr / s);
    }

    double distanceKm(double lat1, double lon1, double lat2, double lon2) {
        double r = PI / 180.0;
        double dlat = (lat2 - lat1) * r, dlon = (lon2 - lon1) * r;
        double a = sin(dlat / 2) * sin(dlat / 2) + cos(lat1 * r) * cos(lat2 * r) * sin(dlon / 2) * sin(dlon / 2);
        return 6371.0 * 2.0 * atan2(sqrt(a), sqrt(1 - a));
    }

    double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
        double r = PI / 180.0;
        double y = sin((lon2 - lon1) * r) * cos(lat2 * r);
        double x = cos(lat1 * r) * sin(lat2 * r) - sin(lat1 * r) * cos(lat2 * r) * cos((lon2 - lon1) * r);
        return pmod(atan2(y, x) / r, 360.0);
    }

    // ------------------------------------------------------------ banco de aviões

    void AircraftDB::setReceiver(double lat, double lon, bool valid) {
        std::lock_guard<std::mutex> lck(mtx);
        rxLat = lat;
        rxLon = lon;
        rxValid = valid;
    }

    bool AircraftDB::isKnown(uint32_t icao) {
        std::lock_guard<std::mutex> lck(mtx);
        return knownIcao.count(icao) > 0;
    }

    void AircraftDB::clear() {
        std::lock_guard<std::mutex> lck(mtx);
        aircraft.clear();
        knownIcao.clear();
        totalMessages = 0;
    }

    void AircraftDB::updatePosition(Aircraft& a, int f, int latCpr, int lonCpr, double now) {
        a.cprLat[f] = latCpr;
        a.cprLon[f] = lonCpr;
        a.cprTime[f] = now;

        double lat = 0, lon = 0;
        bool ok = false;

        // 1) Avião já localizado há pouco: decodificação local relativa a ele
        if (a.hasPosition && now - a.lastPos < 30.0) {
            cprLocal(a.lat, a.lon, latCpr, lonCpr, f, lat, lon);
            ok = distanceKm(a.lat, a.lon, lat, lon) < 30.0;
        }
        // 2) Par par/ímpar recente: decodificação global (sem ambiguidade)
        if (!ok && a.cprTime[0] > 0 && a.cprTime[1] > 0 && fabs(a.cprTime[0] - a.cprTime[1]) < 10.0) {
            ok = cprGlobal(a.cprLat[0], a.cprLon[0], a.cprLat[1], a.cprLon[1], f == 1, lat, lon);
            if (ok && a.hasPosition && now - a.lastPos < 30.0 && distanceKm(a.lat, a.lon, lat, lon) > 30.0) {
                ok = false;   // salto impossível: provavelmente par de mensagens inconsistente
            }
        }
        if (!ok) { return; }
        if (rxValid && distanceKm(rxLat, rxLon, lat, lon) > maxRangeKm) { return; }

        a.lat = lat;
        a.lon = lon;
        a.hasPosition = true;
        a.lastPos = now;
        if (a.trail.empty() || now - a.trail.back().t >= 3.0) {
            a.trail.push_back({ lat, lon, a.hasAltitude ? a.altitude : 0, now });
            while (a.trail.size() > 300) { a.trail.pop_front(); }
        }
    }

    void AircraftDB::decodeExtendedSquitter(Aircraft& a, const uint8_t* me, double now) {
        int tc = me[0] >> 3;
        if (tc >= 1 && tc <= 4) {
            std::string cs = decodeCallsign(me + 1);
            if (!cs.empty()) { a.callsign = cs; }
        }
        else if ((tc >= 9 && tc <= 18) || (tc >= 20 && tc <= 22)) {
            if (tc <= 18) {
                int alt = decodeAc12(((uint32_t)me[1] << 4) | (me[2] >> 4));
                if (alt != INT_MIN) {
                    a.altitude = alt;
                    a.hasAltitude = true;
                    a.altitudeGnss = false;
                }
            }
            int f = (me[2] >> 2) & 1;
            int latCpr = ((me[2] & 3) << 15) | (me[3] << 7) | (me[4] >> 1);
            int lonCpr = ((me[4] & 1) << 16) | (me[5] << 8) | me[6];
            updatePosition(a, f, latCpr, lonCpr, now);
        }
        else if (tc == 19) {
            int st = me[0] & 7;
            if (st == 1 || st == 2) {
                int dew = (me[1] >> 2) & 1;
                int vew = ((me[1] & 3) << 8) | me[2];
                int dns = (me[3] >> 7) & 1;
                int vns = ((me[3] & 0x7F) << 3) | (me[4] >> 5);
                if (vew && vns) {
                    double vx = (vew - 1) * (dew ? -1.0 : 1.0);
                    double vy = (vns - 1) * (dns ? -1.0 : 1.0);
                    if (st == 2) { vx *= 4; vy *= 4; }
                    a.groundSpeed = sqrt(vx * vx + vy * vy);
                    a.track = pmod(atan2(vx, vy) * 180.0 / PI, 360.0);
                    a.hasVelocity = true;
                }
            }
            else if (st == 3 || st == 4) {
                if ((me[1] >> 2) & 1) {   // rumo magnético disponível
                    int h = ((me[1] & 3) << 8) | me[2];
                    if (!a.hasVelocity) { a.track = h * 360.0 / 1024.0; }
                }
            }
            if (st >= 1 && st <= 4) {
                int svr = (me[4] >> 3) & 1;
                int vr = ((me[4] & 7) << 6) | (me[5] >> 2);
                if (vr) {
                    a.verticalRate = (vr - 1) * 64 * (svr ? -1 : 1);
                    a.hasVerticalRate = true;
                }
            }
        }
    }

    void AircraftDB::handle(const Message& m, double now) {
        std::lock_guard<std::mutex> lck(mtx);
        totalMessages++;
        const uint8_t* d = m.data;

        if (m.df == 11 || m.df == 17 || m.df == 18) { knownIcao[m.icao] = now; }
        if (m.df == 18) {
            int cf = d[0] & 7;
            if (cf != 0 && cf != 1 && cf != 6) { return; }   // TIS-B/ADS-R de outros formatos
        }

        Aircraft& a = aircraft[m.icao];
        a.icao = m.icao;
        a.messages++;
        a.lastSeen = now;
        a.signalDb = (a.signalDb < -99.0f) ? m.signalDb : (a.signalDb * 0.8f + m.signalDb * 0.2f);

        switch (m.df) {
        case 0: case 4: case 16: case 20: {
            int alt = decodeAc13(((uint32_t)(d[2] & 0x1F) << 8) | d[3]);
            if (alt != INT_MIN) {
                a.altitude = alt;
                a.hasAltitude = true;
            }
            break;
        }
        case 5: case 21:
            a.squawk = decodeId13(((uint32_t)(d[2] & 0x1F) << 8) | d[3]);
            break;
        case 17: case 18:
            decodeExtendedSquitter(a, d + 4, now);
            break;
        default:
            break;
        }
        // Comm-B BDS 2,0 (identificação) nas respostas DF20/21
        if ((m.df == 20 || m.df == 21) && d[4] == 0x20) {
            std::string cs = decodeCallsign(d + 5);
            if (!cs.empty()) { a.callsign = cs; }
        }
    }

    void AircraftDB::expire(double now, double maxAge) {
        std::lock_guard<std::mutex> lck(mtx);
        for (auto it = aircraft.begin(); it != aircraft.end();) {
            if (now - it->second.lastSeen > maxAge) { it = aircraft.erase(it); }
            else {
                auto& tr = it->second.trail;
                while (!tr.empty() && now - tr.front().t > 900.0) { tr.pop_front(); }
                ++it;
            }
        }
        for (auto it = knownIcao.begin(); it != knownIcao.end();) {
            if (now - it->second > maxAge) { it = knownIcao.erase(it); }
            else { ++it; }
        }
    }

    std::vector<Aircraft> AircraftDB::snapshot() {
        std::lock_guard<std::mutex> lck(mtx);
        std::vector<Aircraft> out;
        out.reserve(aircraft.size());
        for (auto& [k, a] : aircraft) { out.push_back(a); }
        return out;
    }

    // Formato compatível com o aircraft.json do dump1090 (+ rastro e posição da estação)
    std::string AircraftDB::toJson(double now, bool withTrails) {
        std::lock_guard<std::mutex> lck(mtx);
        std::string s;
        s.reserve(4096 + aircraft.size() * (withTrails ? 4096 : 256));
        char b[512];
        snprintf(b, sizeof(b), "{\"now\":%.1f,\"messages\":%llu,", now, (unsigned long long)totalMessages);
        s += b;
        if (rxValid) {
            snprintf(b, sizeof(b), "\"receiver\":{\"lat\":%.5f,\"lon\":%.5f},", rxLat, rxLon);
            s += b;
        }
        s += "\"aircraft\":[";
        bool first = true;
        for (auto& [k, a] : aircraft) {
            if (!first) { s += ","; }
            first = false;
            snprintf(b, sizeof(b), "{\"hex\":\"%06x\",\"messages\":%llu,\"seen\":%.1f,\"rssi\":%.1f",
                     a.icao, (unsigned long long)a.messages, now - a.lastSeen, a.signalDb);
            s += b;
            if (!a.callsign.empty()) { s += ",\"flight\":\"" + a.callsign + "\""; }
            if (a.hasAltitude) { snprintf(b, sizeof(b), ",\"alt_baro\":%d", a.altitude); s += b; }
            if (a.hasVelocity) { snprintf(b, sizeof(b), ",\"gs\":%.1f,\"track\":%.1f", a.groundSpeed, a.track); s += b; }
            else if (a.track != 0) { snprintf(b, sizeof(b), ",\"track\":%.1f", a.track); s += b; }
            if (a.hasVerticalRate) { snprintf(b, sizeof(b), ",\"baro_rate\":%d", a.verticalRate); s += b; }
            if (a.squawk >= 0) { snprintf(b, sizeof(b), ",\"squawk\":\"%04d\"", a.squawk); s += b; }
            if (a.hasPosition) {
                snprintf(b, sizeof(b), ",\"lat\":%.5f,\"lon\":%.5f,\"seen_pos\":%.1f", a.lat, a.lon, now - a.lastPos);
                s += b;
                if (rxValid) {
                    snprintf(b, sizeof(b), ",\"dist_km\":%.1f", distanceKm(rxLat, rxLon, a.lat, a.lon));
                    s += b;
                }
                if (withTrails && !a.trail.empty()) {
                    s += ",\"trail\":[";
                    for (size_t i = 0; i < a.trail.size(); i++) {
                        snprintf(b, sizeof(b), "%s[%.5f,%.5f,%d]", i ? "," : "", a.trail[i].lat, a.trail[i].lon, a.trail[i].alt);
                        s += b;
                    }
                    s += "]";
                }
            }
            s += "}";
        }
        s += "]}";
        return s;
    }
}
