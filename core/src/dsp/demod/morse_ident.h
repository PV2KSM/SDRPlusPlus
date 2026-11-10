#pragma once
// Leitor do indicativo em Morse dos radiofaróis (VOR, ILS, NDB): o tom de 1020 Hz
// (NDB: 400 ou 1020 Hz) ligado e desligado no ritmo do Morse, dentro do áudio AM.
// Mede o tom em blocos de 10 ms (Goertzel) contra frequências vizinhas, separa
// ponto/traço pela duração (velocidade aprendida) e monta as letras.
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>

namespace dsp::demod {
    class MorseIdent {
    public:
        void setSamplerate(double sr) {
            _sr = sr;
            _block = (std::max)(1, (int)std::lround(sr * 0.010));
            _n = 0;
            for (auto& g : _g) { g.s1 = g.s2 = 0; }
            const float F[3] = { 1020.0f, 400.0f, 1600.0f };   // tons do ident e uma referência
            for (int k = 0; k < 3; k++) { _g[k].c = 2.0f * (float)std::cos(2.0 * 3.14159265358979 * F[k] / sr); }
        }

        void feed(const float* x, int count) {
            if (_sr <= 0) { return; }
            for (int i = 0; i < count; i++) {
                for (auto& g : _g) {
                    float s0 = x[i] + g.c * g.s1 - g.s2;
                    g.s2 = g.s1;
                    g.s1 = s0;
                }
                if (++_n >= _block) { endBlock(); }
            }
        }

        // indicativo lido por último (ex.: "SBT") e o que está sendo lido agora
        std::string ident() {
            std::lock_guard<std::mutex> lck(_mtx);
            return _ident;
        }
        std::string partial() {
            std::lock_guard<std::mutex> lck(_mtx);
            return _word + _symbols;
        }

    private:
        struct G {
            float c = 0, s1 = 0, s2 = 0;
            float power() const { return s1 * s1 + s2 * s2 - c * s1 * s2; }
        };

        void endBlock() {
            float p1020 = _g[0].power(), p400 = _g[1].power(), pref = _g[2].power();
            for (auto& g : _g) { g.s1 = g.s2 = 0; }
            _n = 0;
            // usa o tom mais forte entre 1020 e 400 Hz (NDB às vezes usa 400)
            float tone = (std::max)(p1020, p400);
            _floor = _floor <= 0 ? tone : (tone < _floor ? _floor + 0.2f * (tone - _floor) : _floor + 0.002f * (tone - _floor));
            bool on = tone > 8.0f * pref && tone > 6.0f * _floor;
            step(on);
        }

        // ritmo: cada bloco tem 10 ms
        void step(bool on) {
            std::lock_guard<std::mutex> lck(_mtx);
            if (on == _on) {
                _run++;
                if (!on) { checkGap(); }
                return;
            }
            if (_on) {
                if (_run < 3) {
                    // pico de ruído (< 30 ms): não é ponto; o silêncio continua de onde estava
                    _on = false;
                    _run = _offRun + _run + 1;
                    _gapDone = _offGapDone;
                    checkGap();
                    return;
                }
                mark(_run);
            }
            else if (_run < 2 && _markRun > 0) {
                // falha de 10 ms no meio do tom: continua o mesmo ponto/traço
                _symbols = _symbolsBefore;
                _on = true;
                _run = _markRun + _run + 1;
                return;
            }
            else {
                _offRun = _run;
                _offGapDone = _gapDone;
            }
            if (on) { _gapDone = 0; }
            _on = on;
            _run = 1;
        }

        void mark(int len) {
            _symbolsBefore = _symbols;
            _markRun = len;
            // ponto ≈ 1 unidade, traço ≈ 3: separa em 2 unidades e reaprende a unidade
            if (len < 2 * _unit) {
                _symbols += '.';
                _unit = (int)std::lround(0.8 * _unit + 0.2 * len);
            }
            else {
                _symbols += '-';
                _unit = (int)std::lround(0.8 * _unit + 0.2 * (len / 3.0));
            }
            _unit = (std::max)(4, (std::min)(40, _unit));
            if (_symbols.size() > 7) { _symbols.clear(); }   // não é Morse
        }

        void checkGap() {
            if (_gapDone == 0 && _run > 2.5 * _unit && !_symbols.empty()) {
                char c = decode(_symbols);
                _symbols.clear();
                if (c) { _word += c; }
                _gapDone = 1;
            }
            if (_gapDone <= 1 && _run > 6 * _unit) {
                // indicativo de radiofarol: 2 a 4 letras/números
                // só vale quando o mesmo nome é lido duas vezes seguidas (o radiofarol repete)
                if (_word.size() >= 2 && _word.size() <= 4) {
                    if (_word == _cand) { _ident = _word; }
                    _cand = _word;
                }
                _word.clear();
                _gapDone = 2;
            }
        }

        static char decode(const std::string& s) {
            static const char* T[] = { ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--",
                                       "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--.." };
            static const char* D[] = { "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----." };
            for (int i = 0; i < 26; i++) { if (s == T[i]) { return (char)('A' + i); } }
            for (int i = 0; i < 10; i++) { if (s == D[i]) { return (char)('0' + i); } }
            return 0;
        }

        double _sr = 0;
        int _block = 100, _n = 0;
        G _g[3];
        float _floor = 0;
        bool _on = false;
        int _run = 0, _gapDone = 2;
        int _offRun = 0, _offGapDone = 2, _markRun = 0;
        std::string _symbolsBefore, _cand;
        int _unit = 12;   // 120 ms (~10 palavras por minuto)
        std::string _symbols, _word, _ident;
        std::mutex _mtx;
    };
}
