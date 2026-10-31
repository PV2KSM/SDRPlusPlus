#pragma once
// Demodulador FM com AFC (correção automática de frequência) para os modos digitais.
//
// O RTL-SDR costuma ter erro de alguns ppm: em 400 MHz isso põe o sinal DMR
// 2-6 kHz fora do centro, e o DSD perde o sincronismo. Este bloco:
//   1. desloca o sinal pelo erro estimado (NCO);
//   2. filtra o canal (passa-baixa complexo, só o necessário para 4FSK);
//   3. demodula FM (diferença de fase);
//   4. mede o "DC" da saída enquanto há portadora e corrige o NCO aos poucos.
// Em 4FSK com dados aleatórios a média da frequência é zero; o que sobra é o erro.
#include <dsp/processor.h>
#include <dsp/math/hz_to_rads.h>
#include <dsp/math/normalize_phase.h>
#include <atomic>
#include <cmath>
#include <vector>

namespace dsp {
    class AFCQuadrature : public Processor<complex_t, float> {
        using base_type = Processor<complex_t, float>;
    public:
        AFCQuadrature() {}

        void init(stream<complex_t>* in, double deviation, double samplerate, double cutoff, double maxOffset) {
            _sr = samplerate;
            _invDeviation = 1.0f / (float)math::hzToRads(deviation, samplerate);
            _maxOffset = maxOffset;
            makeTaps(cutoff);
            _blockLen = (int)(samplerate * 0.04);   // estimativa a cada 40 ms
            base_type::init(in);
        }

        void setCutoff(double cutoff) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            makeTaps(cutoff);
        }

        void setEnabled(bool en) { _afcOn = en; if (!en) { _freq = 0; offsetHz = 0; } }

        // Erro de frequência corrigido no momento (Hz). Positivo: sinal acima da sintonia.
        std::atomic<float> offsetHz { 0.0f };
        std::atomic<bool> locked { false };

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }
            complex_t* in = base_type::_in->readBuf;
            float* out = base_type::out.writeBuf;
            int T = (int)_taps.size();
            const double TWO_PI = 6.283185307179586;
            for (int i = 0; i < count; i++) {
                // NCO
                _ncoPhase += TWO_PI * _freq / _sr;
                if (_ncoPhase > TWO_PI) { _ncoPhase -= TWO_PI; }
                else if (_ncoPhase < -TWO_PI) { _ncoPhase += TWO_PI; }
                float c = (float)std::cos(_ncoPhase), s = (float)std::sin(_ncoPhase);
                complex_t x = { in[i].re * c + in[i].im * s, in[i].im * c - in[i].re * s };
                // filtro de canal (linha de atraso circular duplicada para não testar índice)
                _hist[_hpos] = x;
                _hist[_hpos + T] = x;
                _hpos = (_hpos + 1) % T;
                complex_t y = { 0, 0 };
                const complex_t* h = &_hist[_hpos];
                for (int k = 0; k < T; k++) {
                    y.re += h[k].re * _taps[k];
                    y.im += h[k].im * _taps[k];
                }
                float ph = y.phase();
                float d = math::normalizePhase(ph - _lastPhase);
                _lastPhase = ph;
                out[i] = d * _invDeviation;

                // estatística para o AFC: média da frequência e quantas amostras
                // fogem muito dela (ruído espalha por +-pi; 4FSK fica em +-0,6 rad)
                _accD += d;
                if (std::fabs(d - _lastMean) > 1.2f) { _wild++; }
                if (++_n >= _blockLen) { updateAfc(); }
            }
            base_type::_in->flush();
            if (!base_type::out.swap(count)) { return -1; }
            return count;
        }

    private:
        void makeTaps(double cutoff) {
            const int N = 49;
            _taps.assign(N, 0.0f);
            double fc = cutoff / _sr, sum = 0;
            for (int i = 0; i < N; i++) {
                double x = i - (N - 1) / 2.0;
                double v = (x == 0) ? 2 * fc : std::sin(2 * 3.14159265358979 * fc * x) / (3.14159265358979 * x);
                double w = 0.42 - 0.5 * std::cos(2 * 3.14159265358979 * i / (N - 1)) + 0.08 * std::cos(4 * 3.14159265358979 * i / (N - 1));
                _taps[i] = (float)(v * w);
                sum += v * w;
            }
            for (auto& t : _taps) { t /= (float)sum; }
            _hist.assign(2 * N, complex_t{ 0, 0 });
            _hpos = 0;
        }

        void updateAfc() {
            float meanD = _accD / _n;
            float wildFrac = (float)_wild / (float)_n;
            _accD = 0;
            _wild = 0;
            _n = 0;
            _lastMean = meanD;
            bool carrier = wildFrac < 0.12f;
            double err = meanD * _sr / 6.283185307179586;   // Hz
            locked = carrier && std::fabs(err) < 300.0;
            if (!_afcOn || !carrier) { return; }
            _freq += 0.35 * err;
            if (_freq > _maxOffset) { _freq = _maxOffset; }
            if (_freq < -_maxOffset) { _freq = -_maxOffset; }
            offsetHz = (float)_freq;
        }

        double _sr = 24000;
        float _invDeviation = 1.0f;
        double _maxOffset = 6000;
        std::vector<float> _taps;
        std::vector<complex_t> _hist;
        int _hpos = 0;
        double _ncoPhase = 0, _freq = 0;
        float _lastPhase = 0;
        bool _afcOn = true;
        int _blockLen = 960, _n = 0;
        float _accD = 0, _lastMean = 0;
        int _wild = 0;
    };
}
