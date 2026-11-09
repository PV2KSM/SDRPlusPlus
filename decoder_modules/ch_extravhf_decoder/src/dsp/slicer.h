#pragma once
#include <dsp/processor.h>
#include <dsp/multirate/polyphase_bank.h>
#include <dsp/loop/phase_control_loop.h>
#include <dsp/taps/windowed_sinc.h>
#include <dsp/filter/fir.h>
#include <dsp/loop/agc.h>
#include <dsp/loop/costas.h>
#include <dsp/loop/fast_agc.h>
#include <dsp/taps/root_raised_cosine.h>
#include <atomic>
#include <cmath>

namespace dsp {

    #define LVL_BUFF 1024

    class FourFSKExtractor : public Processor<float, uint8_t> {
        using base_type = Processor<float, uint8_t>;
    public:
        FourFSKExtractor() {}

        FourFSKExtractor(stream<float>* in) {init(in);}

        // Limiares adaptativos: num 4FSK com dados aleatórios a média de |símbolo|
        // cai exatamente no meio entre o nível interno e o externo (2/3 do externo).
        // Rádios reais desviam 10-20% a mais ou a menos que o nominal; com limiar fixo
        // isso multiplica os erros de símbolo. Só aprende enquanto há portadora.
        inline int process(int count, const float* in, uint8_t* out) {
            bool learn = !carrier || carrier->load();
            for(int i = 0; i < count; i++) {
                float sym_c = in[i];
                if(sym_c >= umid) {
                    out[i] = 0b01;
                } else if(sym_c >= center) {
                    out[i] = 0b00;
                } else if(sym_c >= lmid) {
                    out[i] = 0b10;
                } else {
                    out[i] = 0b11;
                }
                float a = fabsf(sym_c);
                if (learn && a < 2.0f) {
                    absAvg += (a - absAvg) * (1.0f / 256.0f);
                }
            }
            float th = absAvg;
            if (th < 0.4f) { th = 0.4f; }
            if (th > 0.95f) { th = 0.95f; }
            umid = center + th;
            lmid = center - th;
            max = center + th * 1.5f;
            min = center - th * 1.5f;
            return count;
        }

        // porta do aprendizado (portadora presente); nullptr = sempre aprende
        std::atomic<bool>* carrier = nullptr;

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            int outCount = process(count, base_type::_in->readBuf, base_type::out.writeBuf);

            // Swap if some data was generated
            base_type::_in->flush();
            if (outCount) {
                if (!base_type::out.swap(outCount)) { return -1; }
            }
            return outCount;
        }

        float max = 1.0f;
        float min = -1.0f;
        float center = 0.0f;
        float umid = 0.625f;
        float lmid = -0.625f;
        float mid = 0.6f;

    private:
        float absAvg = 0.625f;

    };
}
