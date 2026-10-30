#pragma once
#include <string>

// Abas de trabalho: Aviação, Radioamador e Explorar.
// Cada aba mostra só os painéis do seu uso e lembra a própria sintonia
// (frequência, modo e largura). Explorar mostra tudo.
namespace workspaces {
    enum Workspace {
        AVIATION = 0,
        HAM = 1,
        EXPLORE = 2,
        _COUNT
    };

    void init();                                  // lê a aba salva (chamar depois do config)
    void drawTabs();                              // abas sozinhas (painel lateral do celular)
    void drawControlBar(bool compact = false);    // faixa de controle: abas, modos, largura e squelch
    void drawStatusBar();                         // barra de status embaixo da cascata
    float statusBarHeight();
    bool isMenuVisible(const std::string& name);  // filtro do menu lateral
    std::string displayName(const std::string& name); // título em português do painel
    Workspace current();
    void switchTo(Workspace ws);
}
