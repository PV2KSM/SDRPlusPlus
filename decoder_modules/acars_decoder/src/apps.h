#pragma once
#include "acars.h"

namespace acars {
    // Decodifica as aplicações dentro do texto (CPDLC, ADS-C, MIAM, ARINC 622...)
    // usando a libacars. Preenche m.decoded e m.app; devolve true se reconheceu algo.
    bool decodeApps(Message& m);
}
