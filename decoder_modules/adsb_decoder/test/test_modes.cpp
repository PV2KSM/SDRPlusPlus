// Teste do decodificador Mode S/ADS-B com mensagens de referência conhecidas.
// Compilar: g++ -std=c++17 -O2 -I../src test_modes.cpp ../src/modes.cpp -o test_modes
#include "modes.h"
#include <cstdio>
#include <cmath>
#include <random>
#include <climits>

static int fails = 0;
#define CHECK(cond, ...) do { if (cond) { printf("  ok   "); } else { printf("  FALHA "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

// Gera magnitudes PPM na taxa fs: cada amostra integra o pulso no seu intervalo
// (como um ADC real), começando num instante fracionário aleatório.
static void modulate(const uint8_t* data, int bits, float amp, std::vector<float>& out, std::mt19937& rng, float noise, double fs) {
    std::normal_distribution<float> n(0.0f, noise);
    std::uniform_real_distribution<double> ph(0.0, 1.0);
    std::uniform_real_distribution<double> phase(0.0, 6.2831853);
    std::vector<int> chips = { 1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < bits; i++) {
        bool b = (data[i / 8] >> (7 - i % 8)) & 1;
        chips.push_back(b ? 1 : 0);
        chips.push_back(b ? 0 : 1);
    }
    double chip = 0.5e-6, dur = chips.size() * chip, t0 = -ph(rng) / fs;
    double rot = phase(rng);
    auto cum = [&](double t) {   // integral do pulso de 0 até t
        if (t <= 0) { return 0.0; }
        double s = 0;
        int k = (int)(t / chip);
        for (int i = 0; i < k && i < (int)chips.size(); i++) { s += chips[i] * chip; }
        if (k < (int)chips.size()) { s += chips[k] * (t - k * chip); }
        return s;
    };
    int ns = (int)ceil((dur - t0) * fs) + 1;
    for (int k = 0; k < ns; k++) {
        double a = t0 + k / fs, b = a + 1 / fs;
        float v = (float)((cum(b) - cum(a)) * fs) * amp;
        float i = v * cosf((float)rot) + n(rng), q = v * sinf((float)rot) + n(rng);
        out.push_back(sqrtf(i * i + q * q));
    }
}

static int runDemodTest(double fs, int reps, float amp, float noise, int& sentOut, size_t& planesOut) {
    std::mt19937 rng(42);
    std::vector<float> stream;
    const char* msgs[] = { "8D4840D6202CC371C32CE0576098", "8D485020994409940838175B284F",
                           "8D40621D58C386435CC412692AD6", "8D40621D58C382D690C8AC2863A7" };
    std::normal_distribution<float> nz(0.0f, noise);
    auto gap = [&](int n) { for (int i = 0; i < n; i++) { float a = nz(rng), b = nz(rng); stream.push_back(sqrtf(a * a + b * b)); } };
    uint8_t d[14];
    int bits, sent = 0;
    for (int rep = 0; rep < reps; rep++) {
        for (auto h : msgs) {
            modes::hexToBytes(h, d, bits);
            gap(300 + rng() % 500);
            modulate(d, bits, amp, stream, rng, noise, fs);
            sent++;
        }
    }
    gap(2000);
    modes::Demodulator demod(fs);
    modes::AircraftDB db2;
    int got = 0;
    demod.onMessage = [&](const modes::Message& msg) { got++; db2.handle(msg, 1.0); };
    demod.isKnownIcao = [&](uint32_t i) { return db2.isKnown(i); };
    size_t pos = 0;
    while (pos < stream.size()) {
        size_t n = (std::min)((size_t)(500 + rng() % 4000), stream.size() - pos);
        demod.process(&stream[pos], (int)n);
        pos += n;
    }
    sentOut = sent;
    planesOut = db2.snapshot().size();
    return got;
}

int main() {
    uint8_t d[14];
    int bits;

    printf("CRC\n");
    modes::hexToBytes("8D4840D6202CC371C32CE0576098", d, bits);
    CHECK(modes::crc24(d, bits) == 0, "crc de mensagem DF17 íntegra = 0");
    d[5] ^= 0x10;
    CHECK(modes::crc24(d, bits) != 0, "crc detecta bit trocado");

    printf("Campos\n");
    CHECK(modes::decodeId13(0) == 0, "squawk 0000");
    modes::AircraftDB db;
    modes::Message m;
    auto feed = [&](const char* hex, double t) {
        modes::hexToBytes(hex, m.data, m.bits);
        m.df = m.data[0] >> 3;
        m.icao = ((uint32_t)m.data[1] << 16) | (m.data[2] << 8) | m.data[3];
        db.handle(m, t);
    };
    feed("8D4840D6202CC371C32CE0576098", 1.0);
    feed("8D485020994409940838175B284F", 1.0);
    // Par de posição (ímpar depois par: o par é o mais recente)
    feed("8D40621D58C386435CC412692AD6", 1.0);
    feed("8D40621D58C382D690C8AC2863A7", 2.0);
    for (auto& a : db.snapshot()) {
        if (a.icao == 0x4840D6) { CHECK(a.callsign == "KLM1023", "indicativo 4840D6 = %s (esperado KLM1023)", a.callsign.c_str()); }
        if (a.icao == 0x485020) {
            CHECK(fabs(a.groundSpeed - 159.2) < 0.2, "velocidade %.2f kt (esperado 159.20)", a.groundSpeed);
            CHECK(fabs(a.track - 182.88) < 0.05, "rumo %.2f° (esperado 182.88)", a.track);
            CHECK(a.verticalRate == -832, "razão vertical %d ft/min (esperado -832)", a.verticalRate);
        }
        if (a.icao == 0x40621D) {
            CHECK(a.hasAltitude && a.altitude == 38000, "altitude %d ft (esperado 38000)", a.altitude);
            CHECK(a.hasPosition && fabs(a.lat - 52.2572) < 0.0005 && fabs(a.lon - 3.9194) < 0.0005,
                  "posição %.4f, %.4f (esperado 52.2572, 3.9194)", a.lat, a.lon);
        }
    }

    printf("CPR local (referência próxima)\n");
    double lat, lon;
    modes::cprLocal(52.258, 3.918, 93000, 51372, 0, lat, lon);
    CHECK(fabs(lat - 52.2572) < 0.001 && fabs(lon - 3.9194) < 0.001, "local %.4f, %.4f", lat, lon);

    printf("Demodulador (pulsos desalinhados das amostras, blocos variados)\n");
    struct { double fs; float amp; float noise; double minRate; } cases[] = {
        { 2.4e6, 0.30f, 0.02f, 0.97 },   // RTL-SDR padrão, sinal forte (~24 dB)
        { 2.0e6, 0.30f, 0.02f, 0.80 },   // 2 MS/s (cada meio-bit = 1 amostra: perde mais)
        { 2.4e6, 0.12f, 0.02f, 0.85 },   // sinal fraco (~16 dB)
    };
    for (auto& c : cases) {
        int sent; size_t planes;
        int got = runDemodTest(c.fs, 50, c.amp, c.noise, sent, planes);
        CHECK(got >= sent * c.minRate && planes == 3, "%.1f MS/s, amp %.2f: %d de %d (%.0f%%), %zu aviões",
              c.fs / 1e6, c.amp, got, sent, 100.0 * got / sent, planes);
    }

    printf("Ruído puro não gera aviões falsos\n");
    std::mt19937 rng(7);
    std::normal_distribution<float> nz(0.0f, 0.02f);
    std::vector<float> noise;
    for (int i = 0; i < 2400000; i++) { float a = nz(rng), b = nz(rng); noise.push_back(sqrtf(a * a + b * b)); }
    modes::Demodulator d3(2.4e6);
    int fake = 0;
    d3.onMessage = [&](const modes::Message&) { fake++; };
    d3.isKnownIcao = [](uint32_t) { return false; };
    d3.process(noise.data(), (int)noise.size());
    CHECK(fake == 0, "1 s de ruído: %d mensagens falsas (%llu preâmbulos testados)", fake, (unsigned long long)d3.preambles);

    printf("\n%s (%d falhas)\n", fails ? "FALHOU" : "TUDO OK", fails);
    return fails ? 1 : 0;
}
