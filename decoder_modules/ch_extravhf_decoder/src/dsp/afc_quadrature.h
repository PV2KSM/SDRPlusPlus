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
// Quando o sinal está tão longe que nem passa pelo filtro do canal (ex.: 15 kHz),
// uma busca grossa usa a faixa inteira do VFO (centroide de potência) para trazê-lo.
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

        // Erro máximo que o AFC pode corrigir (acompanha a largura do VFO)
        void setMaxOffset(double maxOffset) {
            _maxOffset = maxOffset;
            if (_freq > _maxOffset) { _freq = _maxOffset; }
            if (_freq < -_maxOffset) { _freq = -_maxOffset; }
        }

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
                // busca grossa: correlação de atraso 1 na faixa inteira (antes do filtro)
                _accSre += x.re * _prevX.re + x.im * _prevX.im;
                _accSim += x.im * _prevX.re - x.re * _prevX.im;
                _accP += x.re * x.re + x.im * x.im;
                _prevX = x;
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

                // estatística para o AFC: média da frequência e estabilidade da
                // envoltória (4FSK tem amplitude constante; ruído não)
                _accD += d;
                double py = (double)y.re * y.re + (double)y.im * y.im;
                _accY2 += py;
                _accY4 += py * py;
                if (++_n >= _blockLen) { updateAfc(); }
            }
            base_type::_in->flush();
            if (!base_type::out.swap(count)) { return -1; }
            return count;
        }

    private:
        void makeTaps(double cutoff) {
            // ~2 ms de filtro: o mesmo corte em qualquer taxa de amostragem
            int N = ((int)(_sr / 500.0)) | 1;
            if (N < 49) { N = 49; }
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
            double m2 = _accY2 / _n, m4 = _accY4 / _n;
            // var(|y|^2)/media^2: ~1 para ruído puro, ~0 para portadora limpa (>= 3 dB de SNR: < 0,55)
            double envVar = (m2 > 0) ? (m4 - m2 * m2) / (m2 * m2) : 1.0;
            double coh = (_accP > 0) ? std::sqrt(_accSre * _accSre + _accSim * _accSim) / _accP : 0;
            double coarse = std::atan2(_accSim, _accSre) * _sr / 6.283185307179586;   // Hz
            _accD = 0;
            _accY2 = _accY4 = 0;
            _n = 0;
            _accSre = _accSim = _accP = 0;
            _lastMean = meanD;
            bool carrier = envVar < 0.55;
            double err = meanD * _sr / 6.283185307179586;   // Hz
            locked = carrier && std::fabs(err) < 300.0;
            if (!_afcOn) { return; }
            if (carrier) {
                _lost = 0;
                _freq += 0.35 * err;
            }
            else if (++_lost >= 3 && coh > 0.3) {
                // nada no filtro do canal mas há um sinal forte na faixa: vai até ele
                _freq += 0.5 * coarse;
            }
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
        int _lost = 0;
        double _accY2 = 0, _accY4 = 0;
        double _accSre = 0, _accSim = 0, _accP = 0;
        complex_t _prevX = { 0, 0 };
    };
}
