#pragma once
#include "../demod.h"

#include <dsp/demod/fm.h>
#include <dsp/correction/dc_blocker.h>
#include <dsp/buffer/packer.h>
#include <dsp/loop/agc.h>
#include <dsp/multirate/rational_resampler.h>
#include <dsp/demod/psk.h>
#include <dsp/buffer/reshaper.h>
#include <dsp/routing/splitter.h>
#include <dsp/stream.h>
#include <dsp/sink/handler_sink.h>
#include <gui/widgets/constellation_diagram.h>
#include "../dsp/dsd.h"
#include "../dsp/slicer.h"
#include "../dsp/afc_quadrature.h"
#include "gui/style.h"
#include <dsp/clock_recovery/fd.h>
#include <dsp/clock_recovery/mm.h>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

// 12 amostras por símbolo (57,6 kHz): cabe um VFO de até 50 kHz, para achar
// sinais bem fora do centro (erro de ppm grande); o filtro do canal fica no AFC
#define INSR                   (4800.0f * 12)
#define CLOCK_RECOVERY_BW      0.1f
#define CLOCK_RECOVERY_DAMPN_F 1.0f
#define CLOCK_RECOVERY_REL_LIM 0.001f
#define RRC_TAP_COUNT          157
#define RRC_ALPHA              0.23f
#ifndef AFC_CUTOFF
#define AFC_CUTOFF             4200.0
#endif

namespace demod {
    // P25p1 = 13 kHz
    // DStar = 7 kHz
    // NXDN48 = 7 kHz
    // NXDN96 = 13 kHz
    // ProVoice = ???(assumed 13 kHz)
    // DMR = 13 kHz
    // X2-TDMA = 13 kHz
    // DPMR = 7 kHz
    // YSF = 17 kHz
    class DSD : public Demodulator {
    public:
        struct DebugStatus {
            bool available = false;
            bool sync = false;
            bool dmr = false;
            bool voice = false;
            bool mbe_decoding = false;
            uint8_t color_code = 0;
            uint8_t slot0_burst = 0;
            uint8_t slot1_burst = 0;
            std::string slot0_type = "";
            std::string slot1_type = "";
            std::string mbe_errorbar = "";
            float offset_hz = 0;      // erro de frequência corrigido pelo AFC
            uint32_t src = 0, dst = 0;  // última chamada (LC)
            bool group = true, emergency = false, privacy = false;
            int lc_slot = -1;
            uint32_t lc_count = 0;
            // P25 fase 1
            bool p25 = false;
            int p25_nac = 0;
            uint32_t p25_tg = 0, p25_src = 0;
            int p25_algid = 0x80;
            bool p25_emergency = false;
            std::string p25_type;
            int p25_kid = 0;
            int p25_mfid = 0;
        };

        DSD() {}

        DSD(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            init(name, config, input, bandwidth, audioSR);
        }

        ~DSD() {
            stop();
            unregisterInstance();
            dsp::taps::free(rrcTaps);
            dsp::taps::free(passTaps);
        }

        void init(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            this->name = name;
            registerInstance();

            // Define structure

            float recov_bandwidth = CLOCK_RECOVERY_BW;
            float recov_dampningFactor = CLOCK_RECOVERY_DAMPN_F;
            float recov_denominator = (1.0f + 2.0 * recov_dampningFactor * recov_bandwidth + recov_bandwidth * recov_bandwidth);
            float recov_mu = (4.0f * recov_dampningFactor * recov_bandwidth) / recov_denominator;
            float recov_omega = (4.0f * recov_bandwidth * recov_bandwidth) / recov_denominator;
            // FM com AFC: segue até 7 kHz de erro do RTL-SDR e filtra só o canal 4FSK
            if (bandwidth > 0) { bw = (float)bandwidth; }
            quadDemod.init(input, 1944.0f, INSR, AFC_CUTOFF, afcMaxOffset(bw));
            dcBlock.init(&quadDemod.out, 1.0f / INSR);
            rrcTaps = dsp::taps::rootRaisedCosine<float>(RRC_TAP_COUNT, RRC_ALPHA, 4800.0f, INSR);
            rrcFilt.init(&dcBlock.out, rrcTaps);
            passTaps = dsp::taps::alloc<float>(1);
            passTaps.taps[0] = 1.0f;
            if (cqpskMode) { applyCqpsk(true); }
            clockRecov.init(&rrcFilt.out, INSR / 4800.0f, recov_omega, recov_mu, CLOCK_RECOVERY_REL_LIM, 32, 8);
            constDiagSplitter.init(&clockRecov.out);
            constDiagSplitter.bindStream(&constDiagStream);
            constDiagSplitter.bindStream(&demodStream);

            constDiagReshaper.init(&constDiagStream, 1024, 0);
            constDiagSink.init(&constDiagReshaper.out, _constDiagSinkHandler, this);

            slicer.init(&demodStream);

            decoder.init(&slicer.out);
            outputConv.init(&decoder.out);
            outputMts.init(&outputConv.out);
        }

        DebugStatus getDebugStatus() {
            DebugStatus st;
            st.available = true;
            dsp::NewDSD::Frame_status fr_st = decoder.getFrameSyncStatus();
            dsp::NewDSD::MBE_status mbe_st = decoder.getMBEStatus();
            dsp::NewDSD::DMR_status dmr_st = decoder.getDMRStatus();
            st.sync = fr_st.sync;
            st.dmr = (fr_st.lasttype == dsp::NewDSD::Frame_status::LAST_DMR);
            st.mbe_decoding = mbe_st.mbe_status_decoding;
            st.voice = st.dmr && st.sync && st.mbe_decoding;
            st.color_code = dmr_st.dmr_status_cc;
            st.slot0_burst = dmr_st.dmr_status_s0_lastburstt;
            st.slot1_burst = dmr_st.dmr_status_s1_lastburstt;
            st.slot0_type = dmr_st.dmr_status_s0_lasttype;
            st.slot1_type = dmr_st.dmr_status_s1_lasttype;
            st.mbe_errorbar = mbe_st.mbe_status_errorbar;
            st.offset_hz = quadDemod.offsetHz;
            st.src = dmr_st.dmr_status_src;
            st.dst = dmr_st.dmr_status_dst;
            st.group = dmr_st.dmr_status_group;
            st.emergency = dmr_st.dmr_status_emergency;
            st.privacy = dmr_st.dmr_status_privacy;
            st.lc_slot = dmr_st.dmr_status_lc_slot;
            st.lc_count = dmr_st.dmr_status_lc_count;
            dsp::NewDSD::P25_status p25_st = decoder.getP25Status();
            st.p25 = (fr_st.lasttype == dsp::NewDSD::Frame_status::LAST_P25);
            st.p25_nac = p25_st.p25_status_nac;
            st.p25_tg = (uint32_t)p25_st.p25_status_tg;
            st.p25_src = (uint32_t)p25_st.p25_status_src;
            st.p25_algid = p25_st.p25_status_algid;
            st.p25_emergency = p25_st.p25_status_emr;
            st.p25_type = p25_st.p25_status_lasttype;
            st.p25_kid = p25_st.p25_status_kid;
            st.p25_mfid = p25_st.p25_status_mfid;
            return st;
        }

        static bool getStatusForRadio(const std::string& radioName, DebugStatus& out) {
            std::lock_guard<std::mutex> lock(registryMtx);
            auto it = instances.find(radioName);
            if (it == instances.end() || !it->second) {
                return false;
            }
            out = it->second->getDebugStatus();
            return true;
        }

        static bool getP25ViewForRadio(const std::string& radioName, dsp::p25::System::View& out) {
            std::lock_guard<std::mutex> lock(registryMtx);
            auto it = instances.find(radioName);
            if (it == instances.end() || !it->second) {
                return false;
            }
            double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            out = it->second->decoder.p25sys.view(now);
            return true;
        }

        // Liga/desliga o modo CQPSK/LSM (simulcast) em todos os demoduladores DSD
        inline static std::atomic<bool> cqpskMode { false };
        // Zera o AFC (ao pular para outra frequência)
        static void resetAfcForRadio(const std::string& radioName) {
            std::lock_guard<std::mutex> lock(registryMtx);
            auto it = instances.find(radioName);
            if (it != instances.end() && it->second) {
                it->second->quadDemod.setEnabled(false);
                it->second->quadDemod.setEnabled(true);
            }
        }

        static void setCqpskAll(bool on) {
            cqpskMode = on;
            std::lock_guard<std::mutex> lock(registryMtx);
            for (auto& [n, d] : instances) {
                if (d) { d->applyCqpsk(on); }
            }
        }
        void applyCqpsk(bool on) {
            quadDemod.setCqpsk(on);
            // no CQPSK o filtro casado já foi aplicado no sinal complexo
            rrcFilt.setTaps(on ? passTaps : rrcTaps);
        }

        // Canal de controle P25 (sistema, frequências, concessões) em JSON
        static bool getP25SystemForRadio(const std::string& radioName, std::string& out) {
            std::lock_guard<std::mutex> lock(registryMtx);
            auto it = instances.find(radioName);
            if (it == instances.end() || !it->second) {
                return false;
            }
            double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            out = it->second->decoder.p25sys.json(now);
            return true;
        }

        static bool waitForDMRSyncVoice(const std::string& radioName, int stableMs, int timeoutMs, DebugStatus& out, int& waitedMs) {
            auto start = std::chrono::steady_clock::now();
            auto lastLoop = start;
            int accumulatedVoiceMs = 0;
            waitedMs = 0;

            while (true) {
                DebugStatus cur;
                if (!getStatusForRadio(radioName, cur)) {
                    out = DebugStatus{};
                    waitedMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                    return false;
                }

                out = cur;
                auto now = std::chrono::steady_clock::now();
                int stepMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLoop).count();
                lastLoop = now;
                if (cur.voice) {
                    accumulatedVoiceMs += stepMs;
                    if (accumulatedVoiceMs >= stableMs) {
                        waitedMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                        return true;
                    }
                }

                waitedMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                if (waitedMs >= timeoutMs) {
                    if (accumulatedVoiceMs >= stableMs) { return true; }
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }

        void start() {
            constDiagSplitter.start();
            quadDemod.start();
            dcBlock.start();
            rrcFilt.start();
            clockRecov.start();
            constDiagReshaper.start();
            constDiagSink.start();
            slicer.start();
            decoder.start();
            outputConv.start();
            outputMts.start();
        }

        void stop() {
            constDiagSplitter.stop();
            quadDemod.stop();
            dcBlock.stop();
            rrcFilt.stop();
            clockRecov.stop();
            constDiagReshaper.stop();
            constDiagSink.stop();
            slicer.stop();
            decoder.stop();
            outputConv.stop();
            outputMts.stop();
        }

        void showMenu() {
            float menuWidth = ImGui::GetContentRegionAvail().x;
            ImGui::Text("Signal constellation+slicer levels: ");
            ImGui::SetNextItemWidth(menuWidth);
            constDiag.draw(ImVec2(0, 20));

            dsp::NewDSD::Frame_status fr_st = decoder.getFrameSyncStatus();
            ImVec4 color = fr_st.sync ? (ImVec4(0.4f, 1.0f, 0.4f, 1.0f)) : (ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            switch (fr_st.lasttype) {
            case dsp::NewDSD::Frame_status::LAST_P25: {
                ImGui::TextColored(color, "Mode: P25p1");
                if (!fr_st.sync) {
                    style::beginDisabled();
                }
                dsp::NewDSD::P25_status p25_st = decoder.getP25Status();
                ImGui::Text("NAC     : 0x%03x", p25_st.p25_status_nac);
                ImGui::Text("DUID    : %u%u %s", p25_st.p25_status_lastduid[0], p25_st.p25_status_lastduid[1], p25_st.p25_status_lasttype.c_str());
                if (p25_st.p25_status_irr_err) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), " IRRECOV ERROR!");
                }
                ImGui::Text("SRC     : %u", p25_st.p25_status_src);
                ImGui::Text("TG       : (id %u) %u (others %u %u %u)", p25_st.p25_status_tgid, p25_st.p25_status_tg, p25_st.p25_status_othertg1, p25_st.p25_status_othertg2, p25_st.p25_status_othertg3);
                ImGui::Text("ALGID : 0x%02x", p25_st.p25_status_algid);
                // ALGID 0 aparece em quadros sem voz (ex.: TSDU do canal de controle): não é criptografia
                if (p25_st.p25_status_algid != 0x80 && p25_st.p25_status_algid != 0x00) {
                    // Encrypted
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "ENCR? KID: 0x%04x", p25_st.p25_status_kid);
                }
                else {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "UNENCR");
                }
                ImGui::Text("MFID    : 0x%02x", p25_st.p25_status_mfid);
                ImGui::TextColored((p25_st.p25_status_emr ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f)), "EMR");
                ImGui::Text("LCFORMAT: 0x%02x", p25_st.p25_status_lcformat);
                ImGui::Text("LCINFO: 0x%016lx", p25_st.p25_status_lcinfo);
                ImGui::Text("MI(INV): 0x%016lx %04x", p25_st.p25_status_mi_0, p25_st.p25_status_mi_1);
                if (!fr_st.sync) {
                    style::endDisabled();
                }
                break;
            }
            case dsp::NewDSD::Frame_status::LAST_DMR: {
                ImGui::TextColored(color, "Mode: DMR");
                if (!fr_st.sync) {
                    style::beginDisabled();
                }
                dsp::NewDSD::DMR_status dmr_st = decoder.getDMRStatus();
                ImGui::Text("SLOT0: (%02d) %s", dmr_st.dmr_status_s0_lastburstt, dmr_st.dmr_status_s0_lasttype.c_str());
                ImGui::Text("SLOT1: (%02d) %s", dmr_st.dmr_status_s1_lastburstt, dmr_st.dmr_status_s1_lasttype.c_str());
                ImGui::Text("CC: 0x%02x %02x", dmr_st.dmr_status_s1_lastburstt, dmr_st.dmr_status_cc);
                if (!fr_st.sync) {
                    style::endDisabled();
                }
                break;
            }
            default:
                ImGui::TextColored(color, "Mode: -");
                break;
            }
            if (!fr_st.sync) {
                style::beginDisabled();
            }
            dsp::NewDSD::MBE_status mbe_st = decoder.getMBEStatus();
            ImGui::TextColored((mbe_st.mbe_status_decoding ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f)), "MBE ERR: %s", mbe_st.mbe_status_errorbar.c_str());
            {
                dsp::NewDSD::DMR_status lc = decoder.getDMRStatus();
                if (lc.dmr_status_lc_count > 0) {
                    ImVec4 amber(0.96f, 0.69f, 0.25f, 1.0f);
                    ImGui::TextColored(amber, "Última chamada (slot %d):", lc.dmr_status_lc_slot + 1);
                    ImGui::TextColored(amber, "%s %u  <-  ID %u", lc.dmr_status_group ? "TG" : "Privada", lc.dmr_status_dst, lc.dmr_status_src);
                    if (lc.dmr_status_emergency || lc.dmr_status_privacy) {
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s%s", lc.dmr_status_emergency ? "EMERGÊNCIA  " : "",
                                           lc.dmr_status_privacy ? "criptografada (sem áudio)" : "");
                    }
                }
                ImGui::Text("AFC: %+.0f Hz", (float)quadDemod.offsetHz);
            }
            if (!fr_st.sync) {
                style::endDisabled();
            }
        }

        void setBandwidth(double bandwidth) {
            bw = bandwidth;
            quadDemod.setMaxOffset(afcMaxOffset(bw));
        }

        // O AFC alcança até a borda da faixa do VFO (menos meio canal), no mínimo 6 kHz
        static double afcMaxOffset(double bandwidth) { return (std::max)(6000.0, bandwidth / 2.0 - 4000.0); }

        void setInput(dsp::stream<dsp::complex_t>* input) {
            quadDemod.setInput(input);
        }

        void AFSampRateChanged(double newSR) {}

        // ============= INFO =============

        const char* getName() { return "DSD"; }
        double getIFSampleRate() { return INSR; }
        double getAFSampleRate() { return 8000.0; }
        double getDefaultBandwidth() { return bw; }
        double getMinBandwidth() { return 5000.0; }
        double getMaxBandwidth() { return 50000.0; }
        bool getBandwidthLocked() { return false; }
        double getMaxAFBandwidth() { return 4000.0; }
        double getDefaultSnapInterval() { return 500.0; }
        int getVFOReference() { return ImGui::WaterfallVFO::REF_CENTER; }
        bool getDeempAllowed() { return false; }
        bool getPostProcEnabled() { return true; }
        int getDefaultDeemphasisMode() { return DEEMP_MODE_NONE; }
        double getAFBandwidth(double bandwidth) { return 4000.0; }
        bool getDynamicAFBandwidth() { return false; }
        bool getFMIFNRAllowed() { return false; }
        bool getNBAllowed() { return false; }
        dsp::stream<dsp::stereo_t>* getOutput() { return &outputMts.out; }

    private:
        void registerInstance() {
            std::lock_guard<std::mutex> lock(registryMtx);
            instances[name] = this;
        }

        void unregisterInstance() {
            std::lock_guard<std::mutex> lock(registryMtx);
            auto it = instances.find(name);
            if (it != instances.end() && it->second == this) {
                instances.erase(it);
            }
        }

        float bw = 16000.0;   // folga para o erro de ppm; o filtro do canal fica no AFC

        inline static std::mutex registryMtx;
        inline static std::unordered_map<std::string, DSD*> instances;

        dsp::AFCQuadrature quadDemod;
        dsp::correction::DCBlocker<float> dcBlock;
        dsp::filter::FIR<float, float> rrcFilt;
        dsp::tap<float> rrcTaps;
        dsp::tap<float> passTaps;
        dsp::clock_recovery::FD clockRecov;
        dsp::routing::Splitter<float> constDiagSplitter;
        dsp::stream<float> constDiagStream;
        dsp::buffer::Reshaper<float> constDiagReshaper;
        dsp::sink::Handler<float> constDiagSink;
        ImGui::ConstellationDiagram constDiag;

        dsp::stream<float> demodStream;
        dsp::FourFSKExtractor slicer;
        dsp::NewDSD decoder;
        dsp::Int16ToFloat outputConv;
        dsp::convert::MonoToStereo outputMts;

        std::string name;

        static void _constDiagSinkHandler(float* data, int count, void* ctx) {
            DSD* _this = (DSD*)ctx;
            dsp::complex_t* cdBuff = _this->constDiag.acquireBuffer();
            if (count == 1024) {
                for (int i = 0; i < 1021; i++) {
                    cdBuff[i].re = data[i];
                    cdBuff[i].im = 0;
                }
                //                cdBuff[1019].re = _this->slicer.max;
                //                cdBuff[1019].im = 0.5f;
                //                cdBuff[1020].re = _this->slicer.min;
                //                cdBuff[1020].im = 0.5f;
                // Display slicer ranges too
                cdBuff[1021].re = _this->slicer.center;
                cdBuff[1021].im = 1.0f;
                cdBuff[1022].re = _this->slicer.umid;
                cdBuff[1022].im = 1.0f;
                cdBuff[1023].re = _this->slicer.lmid;
                cdBuff[1023].im = 1.0f;
            }
            _this->constDiag.releaseBuffer();
        }
    };
}
