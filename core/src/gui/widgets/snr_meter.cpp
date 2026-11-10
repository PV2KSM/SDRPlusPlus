#include <gui/widgets/volume_meter.h>
#include <algorithm>
#include <gui/style.h>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>
#include "snr_meter.h"

namespace ImGui {
    Event<SNRMeterExtPoint> onSNRMeterExtPoint;

    void SNRMeter(float val, const ImVec2& size_arg) {
        ImGuiWindow* window = GetCurrentWindow();
        ImGuiStyle& style = GImGui->Style;

        ImVec2 min = window->DC.CursorPos;
        ImVec2 size = CalcItemSize(size_arg, CalcItemWidth(), 26);
        ImRect bb(min, min + size);

        ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);

        ItemSize(size, style.FramePadding.y);
        if (!ItemAdd(bb, 0)) {
            return;
        }

        // S-meter em barra segmentada (verde até S9, amarelo acima, vermelho perto de saturar),
        // com marca de pico. Escala mantida (0-90 dB de SNR na largura) para o scanner e a
        // redução de ruído continuarem desenhando suas marcas no lugar certo.
        // S estimado pela SNR: 6 dB por unidade S, S9 = 54 dB (o RTL-SDR não é calibrado).
        val = std::clamp<float>(val, 0, 100);
        float ratio = size.x / 90;
        char buf[32];
        float drawVal = (float) val * ratio;
        float barH = 10.0f * style::uiScale;
        float segW = 4.0f * style::uiScale, gap = 2.0f * style::uiScale;
        static float peak = 0;
        static double peakAt = 0;
        double t = ImGui::GetTime();
        if (val >= peak) { peak = val; peakAt = t; }
        else if (t - peakAt > 1.5) { peak = (std::max)(val, peak - (float)(ImGui::GetIO().DeltaTime * 15.0)); }

        ImU32 off = IM_COL32(40, 50, 68, 255);
        for (float x = 0; x + segW <= size.x; x += segW + gap) {
            float db = (x + segW * 0.5f) / ratio;
            ImU32 c = db < 54 ? IM_COL32(70, 210, 122, 255) : (db < 78 ? IM_COL32(240, 194, 74, 255) : IM_COL32(239, 107, 91, 255));
            bool lit = x + segW * 0.5f <= drawVal;
            window->DrawList->AddRectFilled(min + ImVec2(roundf(x), 1), min + ImVec2(roundf(x + segW), barH), lit ? c : off, 1.5f * style::uiScale);
        }
        float px = roundf(peak * ratio);
        window->DrawList->AddRectFilled(min + ImVec2(px - 1.5f * style::uiScale, -1), min + ImVec2(px + 1.5f * style::uiScale, barH + 2), IM_COL32(255, 255, 255, 230));

        // escala S1..S9 e +dB
        static const float marks[] = { 6, 18, 30, 42, 54, 64, 74, 84 };
        static const char* labels[] = { "S1", "3", "5", "7", "9", "+10", "+20", "+30" };
        for (int i = 0; i < 8; i++) {
            float x = roundf(marks[i] * ratio);
            ImVec2 sz = ImGui::CalcTextSize(labels[i]);
            ImU32 c = i > 4 ? IM_COL32(240, 162, 74, 255) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
            window->DrawList->AddText(min + ImVec2(roundf(x - sz.x / 2.0f), 12.0f * style::uiScale), c, labels[i]);
        }
        if (ImGui::IsMouseHoveringRect(bb.Min, bb.Max)) {
            int su = (int)(val / 6.0f);
            if (val >= 54) { snprintf(buf, sizeof buf, "S9 +%.0f dB", val - 54); }
            else { snprintf(buf, sizeof buf, "S%d", su); }
            ImGui::SetTooltip("%s · SNR %.0f dB\nS estimado pela relação sinal/ruído (o RTL-SDR não é calibrado em dBm).", buf, val);
        }

        onSNRMeterExtPoint.emit(SNRMeterExtPoint(min + ImVec2(0, -min.y), drawVal));
    }

    // Medidores de áudio L e R (pico em dB, -40..+3), com marca de pico
    void LRMeter(float l, float r, const ImVec2& size_arg) {
        ImGuiWindow* window = GetCurrentWindow();
        ImGuiStyle& style = GImGui->Style;
        ImVec2 min = window->DC.CursorPos;
        ImVec2 size = CalcItemSize(size_arg, CalcItemWidth(), 26);
        ImRect bb(min, min + size);
        ItemSize(size, style.FramePadding.y);
        if (!ItemAdd(bb, 0)) { return; }
        static float hold[2] = { -60, -60 };
        static double holdAt[2] = { 0, 0 };
        float v[2] = { l, r };
        float lw = ImGui::CalcTextSize("L").x + 4.0f * style::uiScale;
        float barW = size.x - lw;
        float h = 6.0f * style::uiScale;
        const char* nm[2] = { "L", "R" };
        double t = ImGui::GetTime();
        for (int c = 0; c < 2; c++) {
            float db = 20.0f * log10f((std::max)(v[c], 1e-6f));
            if (db >= hold[c]) { hold[c] = db; holdAt[c] = t; }
            else if (t - holdAt[c] > 1.2) { hold[c] -= ImGui::GetIO().DeltaTime * 20.0f; }
            float y = c * (h + 6.0f * style::uiScale) + 2.0f * style::uiScale;
            window->DrawList->AddText(min + ImVec2(0, y + h / 2 - ImGui::GetTextLineHeight() / 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), nm[c]);
            ImVec2 a = min + ImVec2(lw, y), b = min + ImVec2(lw + barW, y + h);
            window->DrawList->AddRectFilled(a, b, IM_COL32(40, 50, 68, 255), h / 2);
            auto xOf = [&](float d) { return lw + barW * std::clamp<float>((d + 40.0f) / 43.0f, 0.0f, 1.0f); };
            float x = xOf(db);
            float xg = xOf(-6.0f);
            if (x > lw) {
                window->DrawList->AddRectFilled(a, min + ImVec2((std::min)(x, xg), y + h), IM_COL32(70, 210, 122, 255), h / 2);
                if (x > xg) { window->DrawList->AddRectFilled(min + ImVec2(xg, y), min + ImVec2(x, y + h), x > xOf(0.0f) ? IM_COL32(239, 107, 91, 255) : IM_COL32(240, 194, 74, 255), h / 2); }
            }
            float hx = xOf(hold[c]);
            if (hx > lw + 1) { window->DrawList->AddRectFilled(min + ImVec2(hx - 1, y - 1), min + ImVec2(hx + 1, y + h + 1), IM_COL32(255, 255, 255, 200)); }
        }
    }
}
