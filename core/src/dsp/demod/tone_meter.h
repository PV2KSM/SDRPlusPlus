#pragma once
// Medidor de tom subaudível (CTCSS) na saída do discriminador FM, antes dos filtros.
// Decima por média para ~1 kHz e mede os 50 tons padrão com Goertzel numa janela
// de 1 s (Hann). Um tom só é anunciado quando domina os outros em duas janelas seguidas.
#include <atomic>
#include <cmath>
#include <vector>
#include <algorithm>

namespace dsp::demod {
    class ToneMeter {
    public:
        static constexpr int COUNT = 50;
        static const float* tones() {
            static const float T[COUNT] = {
                67.0f, 69.3f, 71.9f, 74.4f, 77.0f, 79.7f, 82.5f, 85.4f, 88.5f, 91.5f,
                94.8f, 97.4f, 100.0f, 103.5f, 107.2f, 110.9f, 114.8f, 118.8f, 123.0f, 127.3f,
                131.8f, 136.5f, 141.3f, 146.2f, 150.0f, 151.4f, 156.7f, 159.8f, 162.2f, 165.5f,
                167.9f, 171.3f, 173.8f, 177.3f, 179.9f, 183.5f, 186.2f, 189.9f, 192.8f, 196.6f,
                199.5f, 203.5f, 206.5f, 210.7f, 218.1f, 225.7f, 229.1f, 233.6f, 241.8f, 250.3f
            };
            return T;
        }

        void setSamplerate(double sr) {
            _dec = (std::max)(1, (int)std::lround(sr / 1000.0));
            _rate = sr / _dec;
            _win = (int)std::lround(_rate);   // 1 s
            _buf.assign(_win, 0.0f);
            _hann.resize(_win);
            for (int i = 0; i < _win; i++) { _hann[i] = 0.5f - 0.5f * (float)std::cos(2.0 * 3.14159265358979 * i / (_win - 1)); }
            _pos = 0;
            _acc = 0;
            _n = 0;
            // passa-baixas Butterworth de 2ª ordem em 300 Hz, aplicado duas vezes (4ª ordem):
            // sem ele, a voz entre 700 e 1300 Hz dobra para a faixa dos tons ao decimar
            double w0 = 2.0 * 3.14159265358979 * 300.0 / sr, al = std::sin(w0) / (2.0 * 0.7071), cw = std::cos(w0), a0 = 1 + al;
            _b0 = (float)((1 - cw) / 2 / a0);
            _b1 = (float)((1 - cw) / a0);
            _b2 = _b0;
            _a1 = (float)(-2 * cw / a0);
            _a2 = (float)((1 - al) / a0);
            for (auto& z : _z) { z = 0; }
        }

        // amostras do discriminador (qualquer escala)
        void feed(const float* x, int count) {
            if (_buf.empty()) { return; }
            for (int i = 0; i < count; i++) {
                float v = x[i];
                for (int st = 0; st < 2; st++) {
                    float* z = &_z[st * 2];
                    float y = _b0 * v + z[0];
                    z[0] = _b1 * v - _a1 * y + z[1];
                    z[1] = _b2 * v - _a2 * y;
                    v = y;
                }
                _acc += v;
                if (++_n < _dec) { continue; }
                _buf[_pos++] = _acc / _dec;
                _acc = 0;
                _n = 0;
                if (_pos >= _win) {
                    analyze();
                    _pos = 0;
                }
            }
        }

        // Tom presente (Hz) ou 0
        std::atomic<float> toneHz { 0.0f };

    private:
        void analyze() {
            const float* T = tones();
            float mean = 0;
            for (float v : _buf) { mean += v; }
            mean /= _win;
            float p[COUNT];
            float best = 0, sum = 0;
            int bi = -1;
            for (int k = 0; k < COUNT; k++) {
                double w = 2.0 * 3.14159265358979 * T[k] / _rate;
                double c = 2.0 * std::cos(w), s1 = 0, s2 = 0;
                for (int i = 0; i < _win; i++) {
                    double s0 = (_buf[i] - mean) * _hann[i] + c * s1 - s2;
                    s2 = s1;
                    s1 = s0;
                }
                p[k] = (float)(s1 * s1 + s2 * s2 - c * s1 * s2);
                sum += p[k];
                if (p[k] > best) { best = p[k]; bi = k; }
            }
            // domina se for muito maior que a média dos outros tons
            float others = (sum - best) / (COUNT - 1);
            int cand = (bi >= 0 && others > 0 && best > 25.0f * others) ? bi : -1;
            if (cand >= 0 && cand == _last) { toneHz = T[cand]; }
            else if (cand < 0 && _last < 0) { toneHz = 0.0f; }
            _last = cand;
        }

        int _dec = 48;
        double _rate = 1000;
        int _win = 1000;
        std::vector<float> _buf, _hann;
        int _pos = 0;
        float _acc = 0;
        int _n = 0;
        int _last = -1;
        float _b0 = 1, _b1 = 0, _b2 = 0, _a1 = 0, _a2 = 0;
        float _z[4] = { 0, 0, 0, 0 };
    };
}
