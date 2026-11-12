# esp32-voip

A custom **ESPHome external component** (`sip_voip`) that turns an **AI-Thinker ESP32-A1S
Audio Kit** (classic ESP32-WROVER + **ES8388** codec) into a **SIP intercom endpoint** with
**full-duplex G.711 audio**, while staying in the ESPHome workflow (YAML, OTA, Home Assistant).

It wraps Espressif's SIP/RTC engine (**`esp_sip_service`** / `esp_rtc`, from ESP-ADF) so the
I2S clock stays **continuous** — which fixes the `mic ESP_ERR_TIMEOUT` / clock-drop seen when
driving the ES8388 with higher-level ESPHome audio in half/edge full-duplex.

Built to register to a Home-Assistant-side SIP server (e.g. the **VoIP Stack** / esphome-intercom
integration) or any PBX, so a browser/softphone can talk to the door two-way.

> **Status: WIP / experimental.** This is a from-scratch C++ component that embeds ESP-ADF inside
> ESPHome. Expect to build/flash over USB and iterate (register → one-way → two-way). It is not a
> drop-in; the audio bridge and ADF build integration are the hard parts.

## Why this exists
The A1S is a classic ESP32. ESPHome's own full-duplex I2S (2026.11) drops the shared clock when
the TX side goes idle, starving the ES8388 ADC (`ESP_ERR_TIMEOUT`). Espressif's `esp_sip_service`
runs the media engine with continuous full-duplex clocking by design, which is the robust fix on
this hardware — this component brings that engine into ESPHome.

## Hardware — ESP32-A1S v2.2 (ES8388)
| Signal | GPIO |
|---|---|
| I2C SDA / SCL | 33 / 32 |
| I2S MCLK | 0 |
| I2S BCLK | 27 |
| I2S LRCK/WS | 25 |
| I2S DOUT (→ codec DAC) | 26 |
| I2S DIN (← codec ADC) | 35 |

(ES8388 at I2C `0x10`. Confirm with an I2C scan; `0x1A` = AC101, not supported here.)

## Install (Home Assistant ESPHome add-on)
Requires the **ESPHome beta/2026.11+** add-on (esp-idf framework). Point `external_components` at
this repo:
```yaml
external_components:
  - source: github://ogizhelev/esp32-voip@main
    components: [sip_voip]
    refresh: 0s
```
A complete, ready config is in [`example.yaml`](example.yaml). First flash is over **USB**
(ESP-ADF pulls a large build); OTA after.

## Configuration (sip_voip)
```yaml
sip_voip:
  id: voip
  server: 10.10.4.5          # PBX / HA VoIP Stack SIP server (LAN IP, UDP 5060)
  username: esp32            # SIP account (register as)
  password: esp32
  # auth_username: esp32     # optional, if the login user differs from `username`
  # codec: pcma              # g711 a-law (default) / pcmu
  # auto_answer: true        # pick up incoming calls automatically (door behaviour)
```

## Build / flash notes
- ESPHome add-on on the **beta channel** (esp-idf). First build downloads ESP-ADF + toolchain —
  slow. Flash via USB the first time.
- Watch the serial log for `registered` and the call events.
- Keep G.711 (8 kHz mono), AEC off, no OPUS/TLS — classic-ESP32 is resource-tight.

## License
MIT — see [LICENSE](LICENSE).
