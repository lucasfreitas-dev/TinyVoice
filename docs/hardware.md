# TinyVoice — Hardware & Wiring

Esquema de ligação para o protótipo com **ESP32-WROOM-32** (dev board genérica), **INMP441**, **MAX98357A**, **botão arcade** (com LED integrado) e **alto-falante 4 Ω / 3 W**.

> Todos os GPIOs ficam centralizados em [`firmware/tinyvoice/include/pins.h`](../firmware/tinyvoice/include/pins.h). Altere apenas lá se precisar remapear pinos.

---

## Lista de componentes

| # | Componente | Função |
|---|------------|--------|
| 1 | ESP32-WROOM-32 dev board | Microcontrolador, Wi-Fi, USB |
| 2 | INMP441 (breakout circular) | Microfone digital I2S |
| 3 | MAX98357A (breakout roxo) | Amplificador I2S 3 W |
| 4 | Alto-falante 4 Ω / 3 W | Saída de áudio |
| 5 | Botão arcade verde | Gravar / reproduzir |
| 6 | Breadboard + jumpers | Prototipagem |
| 7 | Resistores 220 Ω (×1 ou ×3) | LED do botão ou LED RGB |

---

## Mapa de GPIOs (padrão do firmware)

| Função | GPIO ESP32 | Notas |
|--------|------------|-------|
| Mic I2S — SCK (BCLK) | **14** | Clock do INMP441 |
| Mic I2S — WS (LRCLK) | **15** | Word select |
| Mic I2S — SD (DOUT) | **32** | Dados do microfone |
| Speaker I2S — BCLK | **27** | Clock do MAX98357A |
| Speaker I2S — LRC | **26** | Left/right clock |
| Speaker I2S — DIN | **25** | Dados para o amp |
| Botão (switch) | **4** | Entrada com pull-up interno |
| LED verde (botão) | **17** | Via resistor 220 Ω |
| LED vermelho (erro) | **16** | Opcional; ou use RGB |
| LED azul (gravação) | **18** | Opcional; ou use RGB |

**Pinos evitados:** GPIO 0, 2, 12, 15 (strapping/boot), GPIO 6–11 (flash SPI).

---

## Diagrama geral

```
                         ┌─────────────────────────────────────┐
                         │           ESP32-WROOM-32            │
                         │                                     │
    INMP441              │  GPIO 14 ─────────── Mic SCK        │
    ┌────────┐           │  GPIO 15 ─────────── Mic WS         │
    │ VDD ───┼── 3.3V ───┤  3.3V                                 │
    │ GND ───┼── GND ─────┤  GND                                  │
    │ SCK ───┼─────────────┤                                     │
    │ WS  ───┼─────────────┤                                     │
    │ SD  ───┼─────────────┤  GPIO 32                            │
    │ L/R ───┼── GND       │                                     │
    └────────┘              │  GPIO 27 ─────────── Spk BCLK       │
                            │  GPIO 26 ─────────── Spk LRC        │
    MAX98357A               │  GPIO 25 ─────────── Spk DIN        │
    ┌────────┐              │                                     │
    │ Vin ───┼── 5V (VIN) ──┤  5V / VIN                           │
    │ GND ───┼── GND ───────┤                                     │
    │ BCLK ──┼───────────────┤                                     │
    │ LRC ───┼───────────────┤                                     │
    │ DIN ───┼───────────────┤                                     │
    │ GAIN ──┼── (NC ou GND) │  ← 9 dB default; 3.3V = 15 dB     │
    │ SD  ───┼── 3.3V        │  ← habilita amp (active high)     │
    │ +  ────┼── Speaker +   │                                     │
    │ -  ────┼── Speaker -   │                                     │
    └────────┘              │  GPIO 4 ────┬── Botão (switch)     │
                            │             │                      │
    Botão arcade            │  GND ───────┴── (comum switch)     │
    ┌────────┐              │                                     │
    │ COM ───┼── GND        │  GPIO 17 ──[220Ω]── LED + (botão)  │
    │ NO  ───┼── GPIO 4     │  GND ────────────── LED - (botão)  │
    │ LED+ ──┼── [220Ω]── GPIO 17                                   │
    │ LED- ──┼── GND       │                                     │
    └────────┘              │  USB-C ← alimentação               │
                            └─────────────────────────────────────┘
```

---

## Ligação detalhada por módulo

### 1. INMP441 (microfone I2S)

Vista superior do breakout circular — pinos típicos:

| Pino INMP441 | Conectar a |
|--------------|------------|
| VDD | 3.3 V (ESP32) |
| GND | GND comum |
| SCK | GPIO **14** |
| WS | GPIO **15** |
| SD | GPIO **32** |
| L/R | **GND** (canal esquerdo / mono) |

```
ESP32          INMP441
─────          ───────
3.3V ───────── VDD
GND  ───────── GND
GND  ───────── L/R
GPIO14 ─────── SCK
GPIO15 ─────── WS
GPIO32 ─────── SD
```

### 2. MAX98357A (amplificador I2S)

| Pino MAX98357A | Conectar a |
|----------------|------------|
| Vin | **5 V** (VIN do ESP32 — mais headroom para 3 W) |
| GND | GND comum |
| BCLK | GPIO **27** |
| LRC | GPIO **26** |
| DIN | GPIO **25** |
| GAIN | Deixar em aberto **ou** GND (9 dB). Evite 15 dB no MVP (pode distorcer) |
| SD | **3.3 V** (mantém amp ligado) |
| Terminal **+** | Fio + do alto-falante |
| Terminal **−** | Fio − do alto-falante |

```
ESP32          MAX98357A       Speaker
─────          ─────────       ───────
5V/VIN ─────── Vin
3.3V   ─────── SD
GND    ─────── GND ────────── (comum)
GPIO27 ─────── BCLK
GPIO26 ─────── LRC
GPIO25 ─────── DIN
               +  ─────────── +
               −  ─────────── −
```

> **Polaridade:** invertida no alto-falante só inverte fase — não danifica. Se o som parecer “oco”, troque +/−.

### 3. Botão arcade (switch + LED)

O botão da foto tem **4 terminais** na base:

| Terminal | Função | Ligação |
|----------|--------|---------|
| COM | Comum do microswitch | **GND** |
| NO | Normalmente aberto | **GPIO 4** |
| NC | Normalmente fechado | *não usar* |
| LED + | Anodo do LED interno | **GPIO 17** via resistor **220 Ω** |
| LED − | Catodo | **GND** |

Comportamento elétrico:
- Botão **solto:** GPIO 4 lê HIGH (pull-up interno `INPUT_PULLUP`)
- Botão **pressionado:** GPIO 4 vai a GND → LOW

```
        3.3V (lógica via pull-up interno do ESP32)

GPIO 4 ──── NO ────┐
                   │  [microswitch]
GND  ──── COM ─────┘

GPIO 17 ──[220Ω]── LED+ ── LED interno ── LED− ── GND
```

### 4. LED RGB externo (alternativa ao LED do botão)

Se preferir um LED RGB separado para os estados (BOOT, gravação, mensagem, erro):

| Cor | GPIO | Resistor |
|-----|------|----------|
| Vermelho | 16 | 220 Ω em série |
| Verde | 17 | 220 Ω em série |
| Azul | 18 | 220 Ω em série |
| Catodo comum | GND | — |

Tipo **cátodo comum:** GPIO → resistor → anodo da cor; catodos juntos no GND.

---

## GND comum e alimentação

```
USB 5V ──► ESP32 VIN/5V ──► MAX98357A Vin
                │
                └── regulador onboard ──► 3.3V ──► INMP441, ESP32, MAX98357 SD

Todos os GND (ESP32, INMP441, MAX98357A, botão, LED, breadboard rails) ──► GND comum
```

| Rail breadboard | Origem |
|-----------------|--------|
| Vermelho (+) | 3.3 V e/ou 5 V (use trilhos separados se possível) |
| Azul (−) | GND comum |

**Consumo estimado:** ESP32 ~150–250 mA (Wi-Fi ativo) + amp até ~500 mA em pico → USB 5 V / **≥ 1 A** recomendado.

---

## Layout sugerido na breadboard

```
        [ USB ]
           │
    ┌──────┴──────┐
    │   ESP32     │──── jumpers curtos ──── trilho 3.3V / GND
    └─────────────┘
         │  │  │
         │  │  └──────────────────────── MAX98357A ── fios ── Speaker
         │  └─────────────────────────── INMP441 (longe do speaker!)
         └────────────────────────────── Botão (fora da breadboard)

    INMP441: fixe com fita; evite vibração e sopros diretos no furo do mic.
    Speaker: apoiado ao lado; não encostar no microfone (feedback).
```

---

## Tabela rápida (para montagem)

| De | Para |
|----|------|
| ESP32 3.3V | INMP441 VDD |
| ESP32 GND | INMP441 GND, INMP441 L/R, MAX98357 GND, botão COM, LED−, trilho GND |
| ESP32 GPIO 14 | INMP441 SCK |
| ESP32 GPIO 15 | INMP441 WS |
| ESP32 GPIO 32 | INMP441 SD |
| ESP32 5V/VIN | MAX98357 Vin |
| ESP32 3.3V | MAX98357 SD |
| ESP32 GPIO 27 | MAX98357 BCLK |
| ESP32 GPIO 26 | MAX98357 LRC |
| ESP32 GPIO 25 | MAX98357 DIN |
| MAX98357 +/− | Alto-falante |
| ESP32 GPIO 4 | Botão NO |
| ESP32 GND | Botão COM |
| ESP32 GPIO 17 → 220Ω | Botão LED+ |
| ESP32 GND | Botão LED− |

---

## Verificação antes de ligar

1. **Continuidade:** nenhum curto entre 3.3 V e GND ou 5 V e GND.
2. **INMP441:** L/R no GND (mono).
3. **MAX98357 SD:** em 3.3 V (amp desligado se em GND).
4. **Speaker:** 4 Ω no terminal correto (não deixar solto com amp ligado).
5. **GPIO 4:** só NO + COM; NC isolado.

---

## Próximas evoluções (não ligar no MVP)

| Recurso | GPIO reservado (sugestão) |
|---------|---------------------------|
| Botão iluminado PWM | GPIO 17 (já usado) |
| Sensor presença (PIR) | GPIO 13 |
| Controle volume (potenciômetro) | GPIO 34 (ADC1) |
| Display I2C (SSD1306) | GPIO 21 SDA, GPIO 22 SCL |
| Bateria + carregador | VIN via TP4056; monitor opcional GPIO 35 |

---

## Referência de pinos (`pins.h`)

```cpp
// firmware/tinyvoice/include/pins.h
struct AudioPins {
    int micSck    = 14;
    int micWs     = 15;
    int micSd     = 32;
    int speakerBclk = 27;
    int speakerLrc  = 26;
    int speakerDin  = 25;
};

constexpr int BUTTON_PIN      = 4;
constexpr int LED_GREEN_PIN   = 17;  // LED integrado do botão arcade
constexpr int LED_RED_PIN     = 16;  // opcional RGB
constexpr int LED_BLUE_PIN    = 18;  // opcional RGB
```
