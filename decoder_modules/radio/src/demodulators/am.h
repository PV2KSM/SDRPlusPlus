#pragma once
#include "../demod.h"
#include <dsp/demod/am.h>

namespace demod {
    class AM : public Demodulator {
    public:
        AM() {}

        AM(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            init(name, config, input, bandwidth, audioSR);
        }

        ~AM() { stop(); }

        void init(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            this->name = name;
            _config = config;

            // Load config
            config->acquire();
            if (config->conf[name][getName()].contains("agcAttack")) {
                agcAttack = config->conf[name][getName()]["agcAttack"];
            }
            if (config->conf[name][getName()].contains("agcDecay")) {
                agcDecay = config->conf[name][getName()]["agcDecay"];
            }
            if (config->conf[name][getName()].contains("carrierAgc")) {
                carrierAgc = config->conf[name][getName()]["carrierAgc"];
            }
            config->release();

            // Define structure
            demod.init(input, carrierAgc ? dsp::demod::AM<dsp::stereo_t>::AGCMode::CARRIER : dsp::demod::AM<dsp::stereo_t>::AGCMode::AUDIO, bandwidth, agcAttack / getIFSampleRate(), agcDecay / getIFSampleRate(), 100.0 / getIFSampleRate(), getIFSampleRate());
        }

        void start() { demod.start(); }

        void stop() { demod.stop(); }

        // Gráfico de modulação do AM: profundidade (%) e envoltória
        void drawModulation() {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float w = ImGui::GetContentRegionAvail().x;
            float d = demod.depth;
            _depthShown += (d - _depthShown) * 0.1f;
            float sh = 40.0f * style::uiScale;
            ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + sh), IM_COL32(10, 16, 28, 255), 6.0f * style::uiScale);
            dl->AddLine(ImVec2(p.x, p.y + sh / 2), ImVec2(p.x + w, p.y + sh / 2), IM_COL32(31, 43, 66, 255));
            const int n = dsp::demod::AM<dsp::stereo_t>::SCOPE_LEN;
            int pos = demod.scopePos;
            ImVec2 top[n], bot[n];
            for (int i = 0; i < n; i++) {
                float v = std::clamp<float>(demod.scope[(pos + i) % n], -1.0f, 1.0f);
                float a = (sh / 2 - 2) * (0.5f + 0.5f * v);
                top[i] = ImVec2(p.x + w * i / (n - 1), p.y + sh / 2 - a);
                bot[i] = ImVec2(p.x + w * i / (n - 1), p.y + sh / 2 + a);
            }
            dl->AddPolyline(top, n, IM_COL32(127, 212, 255, 255), 0, 1.3f * style::uiScale);
            dl->AddPolyline(bot, n, IM_COL32(127, 212, 255, 255), 0, 1.3f * style::uiScale);
            ImGui::Dummy(ImVec2(w, sh));
            float h = 8.0f * style::uiScale;
            p = ImGui::GetCursorScreenPos();
            float bw = w * 0.5f;
            dl->AddRectFilled(p, ImVec2(p.x + bw, p.y + h), IM_COL32(26, 36, 54, 255), h / 2);
            float f = std::clamp<float>(_depthShown, 0.0f, 1.0f);
            dl->AddRectFilled(p, ImVec2(p.x + bw * f, p.y + h), _depthShown > 1.0f ? IM_COL32(239, 107, 91, 255) : IM_COL32(70, 210, 122, 255), h / 2);
            ImGui::Dummy(ImVec2(bw, h));
            ImGui::SameLine();
            ImGui::TextDisabled("Modulação %.0f %%", _depthShown * 100.0f);
            // indicativo em Morse (VOR, ILS, NDB): linha sempre presente para o menu não saltar
            // só o indicativo confirmado (lido igual duas vezes); letras soltas do ruído não aparecem
            std::string id = demod.morse.ident();
            if (!id.empty()) { ImGui::TextColored(ImVec4(0.96f, 0.69f, 0.25f, 1.0f), "Indicativo (Morse): %s", id.c_str()); }
            else { ImGui::TextDisabled("Indicativo (Morse): -"); }
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("VOR, ILS e NDB transmitem o nome da estação em Morse (tom de 1020 Hz) a cada ~10 s."); }
        }

        void showMenu() {
            drawModulation();
            float menuWidth = ImGui::GetContentRegionAvail().x;
            ImGui::LeftLabel("AGC Attack");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderFloat(("##_radio_am_agc_attack_" + name).c_str(), &agcAttack, 1.0f, 200.0f)) {
                demod.setAGCAttack(agcAttack / getIFSampleRate());
                _config->acquire();
                _config->conf[name][getName()]["agcAttack"] = agcAttack;
                _config->release(true);
            }
            ImGui::LeftLabel("AGC Decay");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderFloat(("##_radio_am_agc_decay_" + name).c_str(), &agcDecay, 1.0f, 20.0f)) {
                demod.setAGCDecay(agcDecay / getIFSampleRate());
                _config->acquire();
                _config->conf[name][getName()]["agcDecay"] = agcDecay;
                _config->release(true);
            }
            if (ImGui::Checkbox(("Carrier AGC##_radio_am_carrier_agc_" + name).c_str(), &carrierAgc)) {
                demod.setAGCMode(carrierAgc ? dsp::demod::AM<dsp::stereo_t>::AGCMode::CARRIER : dsp::demod::AM<dsp::stereo_t>::AGCMode::AUDIO);
                _config->acquire();
                _config->conf[name][getName()]["carrierAgc"] = carrierAgc;
                _config->release(true);
            }
        }

        void setBandwidth(double bandwidth) { demod.setBandwidth(bandwidth); }

        void setInput(dsp::stream<dsp::complex_t>* input) { demod.setInput(input); }

        void AFSampRateChanged(double newSR) {}

        // ============= INFO =============

        const char* getName() { return "AM"; }
        double getIFSampleRate() { return 15000.0; }
        double getAFSampleRate() { return getIFSampleRate(); }
        double getDefaultBandwidth() { return 10000.0; }
        double getMinBandwidth() { return 1000.0; }
        double getMaxBandwidth() { return getIFSampleRate(); }
        bool getBandwidthLocked() { return false; }
        double getDefaultSnapInterval() { return 1000.0; }
        int getVFOReference() { return ImGui::WaterfallVFO::REF_CENTER; }
        bool getDeempAllowed() { return false; }
        bool getPostProcEnabled() { return true; }
        int getDefaultDeemphasisMode() { return DEEMP_MODE_NONE; }
        bool getFMIFNRAllowed() { return false; }
        virtual void setAGCFrozen(bool frozen) { demod.setAGCFrozen(frozen); };
        bool getNBAllowed() { return false; }
        dsp::stream<dsp::stereo_t>* getOutput() { return &demod.out; }

    private:
        float _depthShown = 0;
        dsp::demod::AM<dsp::stereo_t> demod;

        ConfigManager* _config = NULL;

        float agcAttack = 50.0f;
        float agcDecay = 5.0f;
        bool carrierAgc = false;

        std::string name;
    };
}