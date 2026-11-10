#pragma once
#include "../demod.h"
#include <dsp/demod/fm.h>

namespace demod {
    class NFM : public Demodulator {
    public:
        NFM() {}

        NFM(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            init(name, config, input, bandwidth, audioSR);
        }

        ~NFM() { stop(); }

        void init(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            this->name = name;
            this->_config = config;

            // Load config
            _config->acquire();
            if (config->conf[name][getName()].contains("lowPass")) {
                _lowPass = config->conf[name][getName()]["lowPass"];
            }
            if (config->conf[name][getName()].contains("highPass")) {
                _highPass = config->conf[name][getName()]["highPass"];
            }
            _config->release();


            // Define structure
            _bw = bandwidth;
            demod.init(input, getIFSampleRate(), bandwidth, _lowPass, _highPass);
        }

        void start() { demod.start(); }

        void stop() { demod.stop(); }

        void showMenu() {
            drawModulation();
            if (ImGui::Checkbox(("Low Pass##_radio_wfm_lowpass_" + name).c_str(), &_lowPass)) {
                demod.setLowPass(_lowPass);
                _config->acquire();
                _config->conf[name][getName()]["lowPass"] = _lowPass;
                _config->release(true);
            }
            if (ImGui::Checkbox(("High Pass##_radio_wfm_highpass_" + name).c_str(), &_highPass)) {
                demod.setHighPass(_highPass);
                _config->acquire();
                _config->conf[name][getName()]["highPass"] = _highPass;
                _config->release(true);
            }
        }

        void setBandwidth(double bandwidth) {
            _bw = bandwidth;
            demod.setBandwidth(bandwidth);
        }

        void setInput(dsp::stream<dsp::complex_t>* input) { demod.setInput(input); }

        void AFSampRateChanged(double newSR) {}

        // ============= INFO =============

        const char* getName() { return "FM"; }
        double getIFSampleRate() { return 50000.0; }
        double getAFSampleRate() { return getIFSampleRate(); }
        double getDefaultBandwidth() { return 12500.0; }
        double getMinBandwidth() { return 1000.0; }
        double getMaxBandwidth() { return getIFSampleRate(); }
        bool getBandwidthLocked() { return false; }
        double getDefaultSnapInterval() { return 2500.0; }
        int getVFOReference() { return ImGui::WaterfallVFO::REF_CENTER; }
        bool getDeempAllowed() { return true; }
        bool getPostProcEnabled() { return true; }
        int getDefaultDeemphasisMode() { return DEEMP_MODE_NONE; }
        bool getFMIFNRAllowed() { return true; }
        bool getNBAllowed() { return false; }
        dsp::stream<dsp::stereo_t>* getOutput() { return &demod.out; }

        // tom CTCSS do sinal recebido (Hz) ou 0
        float getToneHz() { return demod.tone.toneHz; }

        // gráfico de modulação
        float getPeakDeviation() { return demod.peakDeviation; }
        void getScope(float* out, int n) {
            int p = demod.scopePos;
            for (int i = 0; i < n; i++) { out[i] = demod.scope[(p + i) % dsp::demod::FM<dsp::stereo_t>::SCOPE_LEN]; }
        }
        double getDeviationLimit() { return _bw / 2.0; }

        // Gráfico de modulação: barra de desvio (±meia largura) e osciloscópio do áudio
        void drawModulation() {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float w = ImGui::GetContentRegionAvail().x;
            float lim = (float)getDeviationLimit();
            float dev = getPeakDeviation();
            _devShown = dev > _devShown ? dev : _devShown + (dev - _devShown) * 0.08f;
            ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
            ImU32 bg = IM_COL32(26, 36, 54, 255), blue = IM_COL32(59, 160, 230, 255);
            // barra centrada em 0
            ImVec2 p = ImGui::GetCursorScreenPos();
            float h = 12.0f * style::uiScale;
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, h / 2);
            float cx = p.x + w / 2, half = w / 2 * (std::min)(1.0f, _devShown / lim);
            dl->AddRectFilled(ImVec2(cx - half, p.y + 3), ImVec2(cx + half, p.y + h - 3), _devShown > lim * 0.9f ? IM_COL32(239, 107, 91, 255) : blue, 4);
            dl->AddLine(ImVec2(cx, p.y), ImVec2(cx, p.y + h), dim);
            ImGui::Dummy(ImVec2(w, h));
            char b[64];
            snprintf(b, sizeof(b), "Desvio ±%.1f kHz (limite ±%.1f kHz)", _devShown / 1000.0f, lim / 1000.0f);
            ImGui::TextDisabled("%s", b);
            // osciloscópio
            float sh = 46.0f * style::uiScale;
            p = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + sh), IM_COL32(10, 16, 28, 255), 6.0f * style::uiScale);
            dl->AddLine(ImVec2(p.x, p.y + sh / 2), ImVec2(p.x + w, p.y + sh / 2), IM_COL32(31, 43, 66, 255));
            float sc[dsp::demod::FM<dsp::stereo_t>::SCOPE_LEN];
            int n = dsp::demod::FM<dsp::stereo_t>::SCOPE_LEN;
            getScope(sc, n);
            ImVec2 pts[dsp::demod::FM<dsp::stereo_t>::SCOPE_LEN];
            for (int i = 0; i < n; i++) {
                float v = std::clamp<float>(sc[i], -1.0f, 1.0f);
                pts[i] = ImVec2(p.x + w * i / (n - 1), p.y + sh / 2 - v * (sh / 2 - 2));
            }
            dl->AddPolyline(pts, n, IM_COL32(92, 200, 255, 255), 0, 1.5f * style::uiScale);
            ImGui::Dummy(ImVec2(w, sh));
        }

    private:
        dsp::demod::FM<dsp::stereo_t> demod;

        ConfigManager* _config = NULL;

        bool _lowPass = true;
        bool _highPass = false;
        double _bw = 12500;
        float _devShown = 0;

        std::string name;
    };
}