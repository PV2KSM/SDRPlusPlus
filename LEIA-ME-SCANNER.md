# SDR++Brown — versão simples (RTL-SDR v3: banda aérea, DMR, scanner, ADS-B, ACARS/CPDLC)

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
- **ADS-B (aviões em 1090 MHz)** com radar no painel e mapa no navegador
  (veja a seção ADS-B abaixo).
- **Build enxuta:** só RTL-SDR, rtl_tcp, arquivo IQ, áudio, rádio, DSD, ADS-B,
  gerenciador de frequências, scanner de faixa, gravador e redução de ruído.
- **Layout:** Frequency Manager (listas + scanner) e ADS-B logo abaixo do Radio;
  o resto começa fechado (vale para configuração nova).

## Abas: Aviação, Radioamador, Explorar

As abas ficam na **faixa de controle**, acima da cascata. Cada aba mostra só os
painéis do seu uso e **lembra a própria sintonia** (frequência, modo, largura) e a
**própria lista de canais** do scanner.

Na mesma faixa:

- **MODO:** só os modos que fazem sentido na aba (Aviação: AM, NFM, DMR;
  Radioamador: NFM, USB, LSB, CW, AM, DMR; Explorar: todos).
- **LARGURA:** larguras sugeridas para o modo (Aviação/AM: 6k, 8,33k, 12,5k, 25k;
  SSB: 1,8–3k; CW: 200–800 Hz...) e um campo para ajuste fino (arraste ou dê duplo
  clique e digite). **A largura fica salva por aba e por modo**: AM na Aviação pode
  ser 8,33k e AM no Radioamador 6k, cada uma lembrada separadamente. Arrastar a
  borda do VFO na cascata também é lembrado.
- **SQUELCH:** liga/desliga e nível.

Embaixo da cascata, a **barra de status** mostra a sintonia, o scanner
(procurando/recebendo/segurando + canal), o DMR (sincronismo e color code), o
ADS-B (aeronaves) e o ACARS (mensagens e a última recebida).

Os painéis do menu lateral agora têm nomes em português (Fonte, Rádio, Canais e
scanner, Gravador, Saída de áudio...).

- **Aviação:** Fonte, Rádio, canais/scanner, ADS-B, ACARS, gravador, redução de ruído.
  Começa em 118,100 MHz AM.
- **Radioamador:** Source, Radio, listas/scanner, plano de bandas, FT8, varredura
  de faixa, gravador. Começa em 145,000 MHz NFM.
- **Explorar:** tudo liberado, como o SDR++ original.

Ao trocar de aba, o scanner, o ADS-B e o ACARS param (o RTL-SDR tem um sintonizador só).
O tema novo **Simples** (escuro, cantos arredondados) é o padrão; os temas antigos
continuam no menu Theme, dentro de Explorar.

## DMR e P25

- **Largura do DMR/P25 de 5 em 5 kHz até 50 kHz** (botões `-5k` / `+5k` e
  atalhos 12,5k / 25k / 50k na faixa de controle). Com a faixa larga, o AFC
  encontra sinais até ~20 kHz fora do centro (erro de ppm grande do RTL-SDR) e
  filtra só o canal depois de achar.
- **AFC** (correção automática de frequência): a barra de status mostra quanto
  está corrigindo; **CENTRALIZAR** leva o Rádio ao centro do sinal.
- **DMR:** talkgroup, ID de origem, slot, emergência e criptografia.
- **P25 fase 1:** NAC, talkgroup/origem, criptografia e emergência na barra de
  status.
- **P25 troncalizado (painel "P25 · DMR"):** sintonize o **canal de controle** do
  sistema; o painel mostra Sistema/WACN/RFSS/Site, a frequência de controle e as
  **conversas recentes** (talkgroup → frequência). Clique numa para ouvir, ou
  ligue **Seguir conversas**: o Rádio vai para a frequência de voz quando um
  talkgroup recebe canal e volta para o controle quando a conversa acaba. Dá para
  limitar a alguns TGs e pular as criptografadas.
- **Simulcast (LSM/CQPSK)**: chave no painel para P25 de várias torres na mesma
  frequência (constelação vira uma faixa e o DUID sai "UNK").
- **DMR troncalizado**: Tier III (concessões TV/PV_GRANT), **Capacity Plus**
  (canal de descanso e canais ativos com TG) e **Connect Plus** (concessões de
  voz). Esses sistemas anunciam números de canal, não frequências: preencha o
  **mapa de canais** no painel (Tier III = LPCN, Connect Plus = LCN, Capacity
  Plus = nº da repetidora). Com o mapa, "Seguir conversas" funciona igual ao P25.
  Tier II (repetidora comum) já funcionava.
- **TETRA**: painel "TETRA" (aba Explorar) — demodulador π/4-DQPSK com MCC/MNC,
  indicação de criptografia e voz em modo aberto (codec ETSI).
- **Identificar portadoras na tela**: testa cada pico e diz o que é.
- **Quadro de recepção, registro de chamadas (chamadas_digitais.csv) e nomes de TG/ID.**
- Ainda não: P25 **fase 2** (TDMA).

## ACARS e CPDLC (aba Aviação)

Painel **ACARS · CPDLC**: decodifica **vários canais ACARS ao mesmo tempo**
(131,550 / 131,725 / 131,825 / 131,450 / 131,125 / 130,025 MHz por padrão; clique
num canal para ligar/desligar, botão direito para remover, campo para incluir).

- **Iniciar ACARS** centraliza a faixa recebida nos canais ligados (se precisar) e
  põe o Rádio no 131,550 para você ouvir as rajadas. Com o RTL-SDR em 2,4 MS/s
  todos os canais cabem de uma vez.
- Bolinha do canal: verde = rajada agora; azul = ouvindo; vermelho = fora da faixa
  sintonizada; cinza = desligado.
- Demodulação **coerente de MSK** e correção de erros por paridade + CRC
  (testado com sinal sintético: 100% das mensagens a partir de ~12 dB de SNR).
- **CPDLC e ADS-C (FANS-1/A)** são decodificados pela biblioteca libacars
  (MIT, embutida): autorizações do controle, pedidos do piloto, posição/altitude/
  velocidade dos relatórios ADS-C. Aparece um resumo em português e os detalhes.
- **Abrir janela**: lista grande com filtro por matrícula/voo/label/texto, "só
  CPDLC/ADS-C" e "ocultar sinais de vida".
- Tudo fica gravado em `acars_log.txt` na pasta do programa (pode desligar).

Observação: CPDLC por **VDL modo 2** (136,975 MHz e vizinhos, 31,5 kbit/s D8PSK)
é outro sistema e ainda não está incluído; ACARS clássico (o deste painel) é o
mais comum na banda e também carrega CPDLC FANS-1/A.

### Bugs do original corrigidos

- Canais salvos em modo **DSD perdiam o modo ao reiniciar** (viravam "não
  especificado") e, ao salvar a lista de novo, viravam **NFM** para sempre.
- O scanner nunca saía do primeiro canal: a configuração padrão vinha com base
  de ruído **-120 dB** num campo que mede SNR (0–40 dB).
- O comando `add_bookmark` gravava o modo errado para modos de plugin (DSD).
- O tema escolhido só era aplicado ao trocar no menu; ao abrir o programa
  voltava para as cores padrão do ImGui.
- O ADS-B fechava o programa ao ser iniciado pela segunda vez (o bloco de
  processamento era inicializado de novo).

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

## ADS-B: aviões no mapa

1. Abra o painel **ADS-B** e clique em **Iniciar ADS-B (1090 MHz)**. O RTL-SDR
   vai inteiro para 1090 MHz (banda aérea e scanner param). **Parar ADS-B**
   volta para a frequência em que você estava.
2. O painel mostra quantos aviões, quantas mensagens por segundo, um **radar**
   (alcance de 25 a 400 km; passe o mouse num avião para ver os dados) e a lista.
3. **Abrir mapa no navegador** abre um mapa (OpenStreetMap) com os aviões,
   rastros, distância e detalhes ao clicar. Squawk 7500/7600/7700 aparece em
   vermelho. O mapa precisa de internet só para baixar as ruas/os quadros do mapa.
4. **Abrir também no celular:** marque a opção e abra no celular o endereço que
   aparece (ex.: `http://192.168.0.10:8091`), na mesma rede Wi-Fi.
5. **Sua posição:** por padrão usa o *My Grid* do painel Source (ex.: `GG66`).
   Ou desmarque e digite latitude/longitude. Sem posição o radar centraliza nos
   aviões e a distância não aparece.

Dicas para o RTL-SDR v3:
- **Sample rate 2,4 MS/s** e **ganho alto** (40–49 dB). ADS-B gosta de ganho.
- **Antena:** 1090 MHz pede antena curta. Com o kit dipolo do v3, use os
  elementos curtos com uns **6,5 cm de cada lado**, na vertical, perto de uma
  janela ou do lado de fora. Uma antena própria de 1090 MHz rende muito mais.
- Os dados ficam em `http://127.0.0.1:8091/data/aircraft.json`, no mesmo formato
  do dump1090.
