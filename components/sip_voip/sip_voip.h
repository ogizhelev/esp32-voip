#pragma once
// sip_voip — ESPHome external component: SIP intercom endpoint for the ESP32-A1S
// (ES8388). Wraps Espressif's esp_rtc engine and drives a CONTINUOUS full-duplex I2S
// so the ES8388 clock never stops (fixes the mic ESP_ERR_TIMEOUT clock-drop).
//
// Architecture (Path A):
//   esp_rtc  --send_audio()-->  uplink ring buffer  <--  I2S task (mic -> G.711 encode)
//   esp_rtc  --recv_audio()-->  downlink ring buffer --> I2S task (G.711 decode -> spk)
// The esp_rtc callbacks only shuffle bytes to/from the ring buffers (non-blocking); a
// separate free-running task owns I2S read/write and never stops the clock.

#include "esphome/core/component.h"
#include <string>

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
}

namespace esphome {
namespace sip_voip {

// ---- ESP32-A1S v2.2 (ES8388) pin map ----
static const int PIN_I2C_SDA = 33;
static const int PIN_I2C_SCL = 32;
static const int PIN_I2S_MCLK = 0;    // also BOOT strap
static const int PIN_I2S_BCLK = 27;
static const int PIN_I2S_WS = 25;
static const int PIN_I2S_DOUT = 26;   // ESP -> codec DAC (DSDIN)
static const int PIN_I2S_DIN = 35;    // codec ADC -> ESP (ASDOUT), input-only
static const int PIN_PA_ENABLE = 21;  // NS4150 speaker amp enable
static const int ES8388_I2C_ADDR = 0x10;

static const uint32_t SIP_SAMPLE_RATE = 8000;   // G.711 is 8 kHz
static const int SIP_FRAME_SAMPLES = 160;       // 20 ms @ 8 kHz
static const int SIP_FRAME_PCM_BYTES = SIP_FRAME_SAMPLES * 2;  // 16-bit
static const int SIP_FRAME_G711_BYTES = SIP_FRAME_SAMPLES;     // 1 byte/sample

class SipVoip : public Component {
 public:
  void set_server(const std::string &s) { this->server_ = s; }
  void set_port(uint16_t p) { this->port_ = p; }
  void set_username(const std::string &u) { this->username_ = u; }
  void set_password(const std::string &p) { this->password_ = p; }
  void set_codec(uint8_t c) { this->codec_ = c; }  // 1 = G711A (pcma), 2 = G711U (pcmu)
  void set_auto_answer(bool v) { this->auto_answer_ = v; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // Call control (wire to ESPHome actions/buttons from YAML lambdas).
  void call(const std::string &dest);
  void answer();
  void hangup();

  // Called from the static esp_rtc C callbacks — move bytes only, never block.
  int on_send_audio(uint8_t *data, int len);   // engine pulls encoded uplink
  int on_recv_audio(uint8_t *data, int len);   // engine delivers encoded downlink
  void on_rtc_event(int event);

 protected:
  bool init_audio_();     // ES8388 + full-duplex I2S + ring buffers + audio task
  bool start_engine_();   // esp_rtc_service_init once the network is up
  static void audio_task_trampoline_(void *arg);
  void audio_task_();

  std::string server_, username_, password_;
  uint16_t port_{5060};
  uint8_t codec_{1};
  bool auto_answer_{true};

  bool audio_ready_{false};
  bool engine_started_{false};
  volatile bool answer_pending_{false};
  volatile bool in_call_{false};

  void *rtc_{nullptr};  // esp_rtc_handle_t (opaque here)
  RingbufHandle_t uplink_rb_{nullptr};
  RingbufHandle_t downlink_rb_{nullptr};
  TaskHandle_t audio_task_handle_{nullptr};
  void *i2s_tx_{nullptr};  // i2s_chan_handle_t
  void *i2s_rx_{nullptr};  // i2s_chan_handle_t
};

}  // namespace sip_voip
}  // namespace esphome
