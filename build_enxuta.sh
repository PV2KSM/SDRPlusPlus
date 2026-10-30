#!/usr/bin/env bash
# Build enxuta do SDR++Brown para RTL-SDR v3: banda aérea, DMR e scanner.
# Liga só o necessário; todo o resto fica desligado.
#
# Uso (Linux):  ./build_enxuta.sh            -> compila em ./build-enxuta
#               ./build_enxuta.sh --install  -> compila e instala (sudo)
set -euo pipefail
cd "$(dirname "$0")"

KEEP=(
  OPT_BUILD_RTL_SDR_SOURCE        # seu RTL-SDR v3
  OPT_BUILD_RTL_TCP_SOURCE        # RTL-SDR remoto (rtl_tcp), sem dependências
  OPT_BUILD_FILE_SOURCE           # tocar gravações IQ (.wav) para testar
  OPT_BUILD_AUDIO_SINK            # saída de áudio
  OPT_BUILD_RADIO                 # AM/FM/NFM (banda aérea)
  OPT_BUILD_CH_EXTRAVHF_DECODER   # DSD interno: DMR, P25, NXDN, dPMR, YSF
  OPT_BUILD_ADSB_DECODER          # ADS-B 1090 MHz: radar + mapa no navegador
  OPT_BUILD_FREQUENCY_MANAGER     # listas de canais + scanner por lista
  OPT_BUILD_SCANNER               # varredura de faixa (achar frequências novas)
  OPT_BUILD_RECORDER              # gravar áudio/IQ
  OPT_BUILD_NOISE_REDUCTION_LOGMMSE # redução de ruído (ótima em AM)
)

ARGS=()
for opt in $(grep -oE '^option\(OPT_BUILD_[A-Z0-9_]+' CMakeLists.txt | sed 's/option(//'); do
  if printf '%s\n' "${KEEP[@]}" | grep -qx "$opt"; then
    ARGS+=("-D$opt=ON")
  else
    ARGS+=("-D$opt=OFF")
  fi
done

cmake -S . -B build-enxuta -DCMAKE_BUILD_TYPE=Release "${ARGS[@]}"
cmake --build build-enxuta -j"$(nproc)"

if [[ "${1:-}" == "--install" ]]; then
  sudo cmake --install build-enxuta
fi

echo
echo "Pronto. Para rodar sem instalar:"
echo "  sh create_root.sh && ./build-enxuta/sdrpp -r root_dev"
