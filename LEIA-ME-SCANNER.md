# SDR++Brown — versão simples (RTL-SDR v3: banda aérea, DMR, scanner)

## O que muda em relação ao original

- **Sem cabo virtual para DMR.** O decodificador DSD já vem dentro do programa
  (modo `DSD` no menu Radio). O fluxo dos vídeos — SDR# → VB-Cable → DSD+ — vira
  um passo só: escolher o modo `DSD` na frequência.
- **Scanner por lista de canais** (no painel *Frequency Manager*):
  - Analógico (AM da banda aérea, NFM): para quando o SNR passa do limiar.
  - DSD/DMR: para só quando o decodificador pega **sincronismo digital**, não
    em qualquer portadora.
  - Fica no canal enquanto houver conversa e sai após o *tempo de espera*
    (padrão 2 s) sem sinal. Antes ficava um tempo fixo de 10 s.
  - Botões **Segurar / Pular / Bloquear**. Bloqueios ficam salvos por lista e
    aparecem como `[bloq.]` na tabela de canais.
  - **Canal prioritário** checado a cada N segundos (ex.: torre ou emergência).
  - Varre em ordem de frequência e só troca de modo quando precisa
    (sem estalos ao alternar entre canais AM e DMR iguais).
  - **Log de atividade** na tela e em `scanner_log.csv` (pasta root do programa).
- **Build enxuta:** só RTL-SDR, rtl_tcp, arquivo IQ, áudio, rádio, DSD,
  gerenciador de frequências, scanner de faixa, gravador e redução de ruído.
- **Layout:** Frequency Manager (listas + scanner) logo abaixo do Radio; o resto
  começa fechado (vale para configuração nova).

### Bugs do original corrigidos

- Canais salvos em modo **DSD perdiam o modo ao reiniciar** (viravam "não
  especificado") e, ao salvar a lista de novo, viravam **NFM** para sempre.
- O scanner nunca saía do primeiro canal: a configuração padrão vinha com base
  de ruído **-120 dB** num campo que mede SNR (0–40 dB).
- O comando `add_bookmark` gravava o modo errado para modos de plugin (DSD).

## Compilar (Linux)

```bash
sudo apt install build-essential cmake git libfftw3-dev libglfw3-dev \
    libvolk-dev libzstd-dev librtlsdr-dev librtaudio-dev liborc-0.4-dev libcurl4-openssl-dev
./build_enxuta.sh
sh create_root.sh && ./build-enxuta/sdrpp -r root_dev
```

## Montando as listas

1. **Banda aérea:** no Frequency Manager crie a lista `Aérea`. Para cada canal:
   sintonize, modo **AM**, largura **8000 Hz** (canais 8,33 kHz) ou **12500 Hz**,
   e clique `+`. Inclua 121.500 (emergência) e marque como prioritário se quiser.
2. **DMR:** crie a lista `DMR`. Sintonize a frequência, modo **DSD**, largura
   **12500 Hz**, `+`. Para achar frequências novas use o módulo *Scanner*
   (varredura de faixa) e depois salve as que tiverem DMR.
3. Pode misturar AM e DSD na mesma lista: o scanner troca de modo sozinho.

## Ajustes recomendados para o RTL-SDR v3

- **Ganho manual** (AGC desligado). Comece perto de 30–40 dB; perto de
  aeroporto o AGC satura.
- **Sample rate 2,4 MS/s.** O v3 enxerga ~2 MHz por vez; canais próximos
  entre si são varridos mais rápido.
- **Base de ruído:** com o scanner parado num canal vazio, clique
  *Medir agora* em *Ajustes do scanner*. Se parar em ruído, aumente a margem.
- **Redução de ruído (LogMMSE)** ligada ajuda muito no AM.
- DMR: *Tempo por canal DMR* de 450 ms é um bom começo; se perder chamadas
  curtas, aumente para 600–800 ms.

DMR **criptografado** não é decodificado — só DMR aberto.
