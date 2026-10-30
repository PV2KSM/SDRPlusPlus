// Testes offline do decodificador ACARS: g++ via CMake (alvo test_acars)
#include "acars.h"
#include "apps.h"
#include <cstdio>
#include <cmath>
#include <complex>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FALHOU %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static acars::Message sample(bool down) {
    acars::Message m;
    m.mode = '2';
    m.reg = "PR-GXA";
    m.label = down ? "H1" : "AA";
    m.downlink = down;
    m.blockId = down ? '3' : 'D';
    m.msgNo = "M01A";
    m.flight = "G31234";
    m.text = down ? "#DFB POS S23123W045456,FL350,ETA 1532" : "TESTE DE SUBIDA SBGR";
    return m;
}

int main() {
    // 1) quadro -> bytes -> quadro
    {
        auto m = sample(true);
        auto f = acars::buildFrame(m);
        CHECK(acars::crc16(f.data(), f.size()) == 0);
        acars::Message o;
        CHECK(acars::parseFrame(f, o));
        CHECK(o.reg == "PR-GXA");
        CHECK(o.label == "H1");
        CHECK(o.flight == "G31234");
        CHECK(o.msgNo == "M01A");
        CHECK(o.text == m.text);
    }
    // 2) as quatro convenções de tom, com ruído
    for (int conv = 0; conv < 4; conv++) {
        for (int down = 0; down < 2; down++) {
            auto m = sample(down);
            auto env = acars::synthesize(m, 20, 7 + conv, conv);
            acars::Message o;
            bool ok = acars::decodeBurst(env, o);
            CHECK(ok);
            if (ok) {
                CHECK(o.text == m.text);
                CHECK(o.reg == "PR-GXA");
            }
        }
    }
    // 3) sensibilidade: taxa de acerto por SNR
    for (float snr : { 6.f, 9.f, 12.f, 15.f }) {
        int ok = 0, n = 30;
        for (int i = 0; i < n; i++) {
            acars::Message o;
            if (acars::decodeBurst(acars::synthesize(sample(i & 1), snr, 100 + i, i % 4), o)) { ok++; }
        }
        printf("SNR %4.1f dB: %d/%d\n", snr, ok, n);
        if (snr >= 12) { CHECK(ok >= n * 8 / 10); }
    }
    // 3b) relógio do modem desviado (+-200 ppm) com sinal médio
    for (double ppm : { -200.0, 200.0 }) {
        int ok = 0, n = 10;
        for (int i = 0; i < n; i++) {
            acars::Message o;
            if (acars::decodeBurst(acars::synthesize(sample(i & 1), 11, 300 + i, i % 4, ppm), o)) { ok++; }
        }
        printf("relógio %+.0f ppm, SNR 11 dB: %d/%d\n", ppm, ok, n);
        CHECK(ok >= n / 2);
    }
    // 4) caminho contínuo: IQ com silêncio, rajada e silêncio, com deslocamento fracionário
    {
        auto m = sample(true);
        auto env = acars::synthesize(m, 30, 3, 2);
        std::vector<std::complex<float>> iq;
        for (int i = 0; i < 6000; i++) { iq.push_back(std::complex<float>(0.01f * std::cos(i * 0.3f), 0.01f * std::sin(i * 0.7f))); }
        for (float e : env) { iq.push_back(std::complex<float>(e * 0.5f, 0)); }
        for (int i = 0; i < 6000; i++) { iq.push_back(std::complex<float>(0.01f * std::cos(i * 0.3f), 0.01f * std::sin(i * 0.5f))); }
        acars::ChannelDecoder ch;
        int got = 0;
        ch.onMessage = [&](acars::Message& o) { got++; CHECK(o.text == m.text); };
        for (size_t i = 0; i < iq.size(); i += 512) { ch.process(iq.data() + i, (int)std::min<size_t>(512, iq.size() - i)); }
        CHECK(got == 1);
    }
    // 5) CPDLC / ADS-C pela libacars (exemplos da documentação da libacars)
    {
        acars::Message m;
        m.label = "AA";
        m.downlink = false;
        m.text = "/AKLCDYA.AT1.9V-SVG21D0755D84AD067448398722949A7521C8AB4A1C8EAB5CE393";
        CHECK(acars::decodeApps(m));
        CHECK(m.app == "CPDLC");
        printf("--- CPDLC ---\n%s\n", m.decoded.c_str());
        acars::Message a;
        a.label = "B6";
        a.downlink = true;
        a.text = "/BOMASAI.ADS.VT-ANB072501A070A988CA73248F0E5DC10200000F5EE1ABC000102B885E0A19F5";
        CHECK(acars::decodeApps(a));
        CHECK(a.app == "ADS-C");
        printf("--- ADS-C ---\n%s\n", a.decoded.c_str());
    }
    printf(fails ? "%d FALHAS\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
