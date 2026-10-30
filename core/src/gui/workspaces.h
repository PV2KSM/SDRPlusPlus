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
    void drawTabs();                              // barra de abas, no topo do painel lateral
    bool isMenuVisible(const std::string& name);  // filtro do menu lateral
    Workspace current();
    void switchTo(Workspace ws);
}
