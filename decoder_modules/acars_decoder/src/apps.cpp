#include "apps.h"
#include <libacars/libacars.h>
#include <libacars/acars.h>
#include <libacars/vstring.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace acars {

    static std::string appName(const std::string& label, const std::string& text) {
        if (text.find(".AT1.") != std::string::npos || text.find(".AT1") != std::string::npos) { return "CPDLC"; }
        if (text.find(".CR1.") != std::string::npos || text.find(".CC1.") != std::string::npos ||
            text.find(".DR1.") != std::string::npos) { return "CPDLC (conexão)"; }
        if (text.find(".ADS.") != std::string::npos || text.find(".DIS.") != std::string::npos) { return "ADS-C"; }
        if (label == "MA") { return "MIAM"; }
        if (text.find("/MD") == 0 || text.find("- #M") == 0) { return "Media Advisory"; }
        return "Aplicação";
    }

    static std::string valueAfter(const std::string& txt, const std::string& key) {
        size_t p = txt.find(key);
        if (p == std::string::npos) { return ""; }
        p += key.size();
        size_t e = txt.find('\n', p);
        std::string v = txt.substr(p, e == std::string::npos ? std::string::npos : e - p);
        size_t a = v.find_first_not_of(' ');
        return a == std::string::npos ? "" : v.substr(a);
    }

    // Resumo em português do que a libacars decodificou
    static std::string summarize(const std::string& app, const std::string& d) {
        if (app == "ADS-C") {
            std::string lat = valueAfter(d, "Lat: "), lon = valueAfter(d, "Lon: "), alt = valueAfter(d, "Alt: ");
            std::string gs = valueAfter(d, "Ground speed: "), trk = valueAfter(d, "True track: ");
            if (lat.empty()) { return "Relatório ADS-C"; }
            char b[200];
            snprintf(b, sizeof(b), "Posição %.4f, %.4f", atof(lat.c_str()), atof(lon.c_str()));
            std::string s = b;
            if (!alt.empty()) { s += " · " + alt; }
            if (!gs.empty()) { s += " · " + gs; }
            if (!trk.empty()) { s += " · rumo " + trk; }
            return s;
        }
        if (app.rfind("CPDLC", 0) == 0) {
            std::string dir = d.find("Uplink") != std::string::npos ? "Controle > avião: " : (d.find("Downlink") != std::string::npos ? "Avião > controle: " : "");
            size_t p = d.find("Message data:");
            if (p != std::string::npos) {
                size_t a = d.find('\n', p);
                if (a != std::string::npos) {
                    size_t e = d.find('\n', a + 1);
                    std::string l = d.substr(a + 1, e == std::string::npos ? std::string::npos : e - a - 1);
                    size_t f = l.find_first_not_of(' ');
                    if (f != std::string::npos) {
                        // parâmetros: linhas seguintes mais recuadas ("Fix: VANDA" -> "VANDA")
                        std::string vals;
                        size_t q = e;
                        while (q != std::string::npos && q + 1 < d.size()) {
                            size_t qe = d.find('\n', q + 1);
                            std::string ln = d.substr(q + 1, qe == std::string::npos ? std::string::npos : qe - q - 1);
                            size_t ind = ln.find_first_not_of(' ');
                            if (ind == std::string::npos || ind <= f) { break; }
                            size_t c = ln.find(": ");
                            std::string v = (c == std::string::npos) ? ln.substr(ind) : ln.substr(c + 2);
                            if (!v.empty()) { vals += (vals.empty() ? "" : ", ") + v; }
                            q = qe;
                        }
                        return dir + l.substr(f) + (vals.empty() ? "" : "  (" + vals + ")");
                    }
                }
            }
            if (d.find("Connect") != std::string::npos || d.find("CR1") != std::string::npos) { return dir + "pedido de conexão CPDLC"; }
            return dir + "mensagem CPDLC";
        }
        return "";
    }

    bool decodeApps(Message& m) {
        if (m.text.empty()) { return false; }
        la_msg_dir dir = m.downlink ? LA_MSG_DIR_AIR2GND : LA_MSG_DIR_GND2AIR;
        la_proto_node* node = la_acars_decode_apps(m.label.c_str(), m.text.c_str(), dir);
        if (!node) { return false; }
        la_vstring* vs = la_proto_tree_format_text(NULL, node);
        bool ok = false;
        if (vs && vs->str && strlen(vs->str) > 0) {
            m.decoded = vs->str;
            m.app = appName(m.label, m.text);
            m.summary = summarize(m.app, m.decoded);
            ok = true;
        }
        if (vs) { la_vstring_destroy(vs, true); }
        la_proto_tree_destroy(node);
        return ok;
    }
}
