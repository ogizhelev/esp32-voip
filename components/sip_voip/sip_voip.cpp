#include "sip_voip.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"
#include <cstring>

extern "C" {
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_rtc.h"   // from espressif/esp_media_protocols (prebuilt engine)
}

namespace esphome {
namespace sip_voip {

static const char *const TAG = "sip_voip";

// A single global pointer so the C callbacks can reach the instance. There is only
// ever one SIP endpoint on a device.
static SipVoip *g_self = nullptr;

// ES8388 I2C master-bus handles (new IDF i2c driver)
static i2c_master_bus_handle_t es_bus_ = nullptr;
static i2c_master_dev_handle_t es_dev_ = nullptr;

// ---------------------------------------------------------------------------
// G.711 companding (16-bit linear PCM <-> 8-bit A-law / u-law)
// ---------------------------------------------------------------------------
static inline uint8_t linear_to_alaw(int16_t pcm) {
  int mask = 0x55, seg = 0, aval;
  int p = pcm;
  if (p >= 0) { mask |= 0x80; } else { p = -p - 1; if (p < 0) p = 0; }
  if (p > 0x7FFF) p = 0x7FFF;
  int tmp = p >> 4;
  if (tmp >= 0x100) { for (seg = 1; seg < 8 && (tmp >> (seg + 3)) & 0x1F; seg++) {} }
  // simpler table-free approximation per ITU G.711
  seg = 0; { int v = p >> 8; while (v) { v >>= 1; seg++; } if (seg > 8) seg = 8; }
  if (seg >= 1) p >>= (seg == 1 ? 4 : seg + 3);
  else p >>= 4;
  aval = (seg << 4) | (p & 0x0F);
  return (uint8_t) (aval ^ mask);
}
static inline int16_t alaw_to_linear(uint8_t a) {
  a ^= 0x55;
  int t = (a & 0x0F) << 4;
  int seg = (a & 0x70) >> 4;
  switch (seg) { case 0: t += 8; break; case 1: t += 0x108; break;
    default: t += 0x108; t <<= (seg - 1); }
  return (int16_t) ((a & 0x80) ? t : -t);
}
#define BIAS 0x84
static inline uint8_t linear_to_ulaw(int16_t pcm) {
  int sign = (pcm >> 8) & 0x80;
  if (sign) pcm = -pcm;
  if (pcm > 0x7FFF) pcm = 0x7FFF;
  pcm += BIAS;
  int seg = 0; { int v = pcm >> 7; while (v) { v >>= 1; seg++; } if (seg > 7) seg = 7; }
  int uval = (seg << 4) | ((pcm >> (seg + 3)) & 0x0F);
  return (uint8_t) (~(sign | uval));
}
static inline int16_t ulaw_to_linear(uint8_t u) {
  u = ~u;
  int t = ((u & 0x0F) << 3) + BIAS;
  t <<= ((u & 0x70) >> 4);
  t -= BIAS;
  return (int16_t) ((u & 0x80) ? (BIAS - t) : (t - BIAS));
}

// ---------------------------------------------------------------------------
// ES8388 init over I2C (⚠ WIP: register sequence based on the AI-Thinker/ESP-ADF
// es8388 driver; verify levels/inputs against the datasheet on hardware.)
// ---------------------------------------------------------------------------
static esp_err_t es_w(uint8_t reg, uint8_t val) {
  uint8_t buf[2] = {reg, val};
  return i2c_master_transmit(es_dev_, buf, 2, 100);
}

static bool es8388_init() {
  // New IDF i2c-master-bus API (legacy driver/i2c.h is removed on recent ESP-IDF).
  i2c_master_bus_config_t bus_cfg = {};
  bus_cfg.i2c_port = I2C_NUM_0;
  bus_cfg.sda_io_num = (gpio_num_t) PIN_I2C_SDA;
  bus_cfg.scl_io_num = (gpio_num_t) PIN_I2C_SCL;
  bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_cfg.glitch_ignore_cnt = 7;
  bus_cfg.flags.enable_internal_pullup = true;
  if (i2c_new_master_bus(&bus_cfg, &es_bus_) != ESP_OK) {
    ESP_LOGE(TAG, "i2c_new_master_bus failed");
    return false;
  }
  i2c_device_config_t dev_cfg = {};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.device_address = ES8388_I2C_ADDR;
  dev_cfg.scl_speed_hz = 100000;
  if (i2c_master_bus_add_device(es_bus_, &dev_cfg, &es_dev_) != ESP_OK) {
    ESP_LOGE(TAG, "i2c add ES8388 failed");
    return false;
  }

  // Reset + power/clock
  es_w(0x00, 0x80); es_w(0x00, 0x00);  // soft reset
  es_w(0x08, 0x00);                    // I2S slave (ESP is I2S master)
  es_w(0x02, 0xF3);                    // power down DEM/STM during config
  es_w(0x2B, 0x80);                    // DACCONTROL21: same LRCK for ADC/DAC (duplex)
  es_w(0x00, 0x05);                    // VMID + EnRef
  es_w(0x01, 0x40);                    // low-power off

  // ADC (mic/line in). 0x0a: 0x00=LINPUT1/RINPUT1 (onboard mic), 0x50=LINPUT2/RINPUT2 (AUX)
  es_w(0x03, 0x00);                    // ADC power up
  es_w(0x09, 0x00);                    // no mic boost (line level); 0x88 for mic boost
  es_w(0x0A, 0x00);                    // input = LINE1 (onboard mics). AUX tap: use 0x50
  es_w(0x0B, 0x02);
  es_w(0x0C, 0x0C);                    // 16-bit, I2S
  es_w(0x0D, 0x02);                    // MCLK/LRCK = 256
  es_w(0x10, 0x00); es_w(0x11, 0x00);  // ADC L/R volume 0 dB

  // DAC (speaker/line out)
  es_w(0x04, 0x3C);                    // DAC power: LOUT1/ROUT1 + LOUT2/ROUT2
  es_w(0x17, 0x18);                    // 16-bit, I2S
  es_w(0x18, 0x02);                    // MCLK/LRCK = 256
  es_w(0x1A, 0x00); es_w(0x1B, 0x00);  // DAC L/R volume 0 dB
  es_w(0x26, 0x00);
  es_w(0x27, 0xB8); es_w(0x2A, 0xB8);  // mixer: route DAC to outputs
  es_w(0x2E, 0x1E); es_w(0x2F, 0x1E);  // LOUT1/ROUT1 (headphone) volume
  es_w(0x30, 0x1E); es_w(0x31, 0x1E);  // LOUT2/ROUT2 (speaker amp) volume
  es_w(0x19, 0x02);                    // DAC unmute
  es_w(0x02, 0x00);                    // power up DEM/STM
  es_w(0x03, 0x09);                    // ADC on + mic bias

  // NS4150 speaker amp enable
  gpio_set_direction((gpio_num_t) PIN_PA_ENABLE, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t) PIN_PA_ENABLE, 1);
  return true;
}

// ---------------------------------------------------------------------------
// Full-duplex I2S0 (TX + RX share BCLK/WS/MCLK). Opened once, never stopped.
// ---------------------------------------------------------------------------
bool SipVoip::init_audio_() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  i2s_chan_handle_t tx = nullptr, rx = nullptr;
  if (i2s_new_channel(&chan_cfg, &tx, &rx) != ESP_OK) {  // both -> full duplex
    ESP_LOGE(TAG, "i2s_new_channel failed");
    return false;
  }
  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SIP_SAMPLE_RATE);  // MCLK = 256 * Fs
  std_cfg.slot_cfg =
      I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  std_cfg.gpio_cfg.mclk = (gpio_num_t) PIN_I2S_MCLK;
  std_cfg.gpio_cfg.bclk = (gpio_num_t) PIN_I2S_BCLK;
  std_cfg.gpio_cfg.ws = (gpio_num_t) PIN_I2S_WS;
  std_cfg.gpio_cfg.dout = (gpio_num_t) PIN_I2S_DOUT;
  std_cfg.gpio_cfg.din = (gpio_num_t) PIN_I2S_DIN;
  if (i2s_channel_init_std_mode(tx, &std_cfg) != ESP_OK ||
      i2s_channel_init_std_mode(rx, &std_cfg) != ESP_OK) {
    ESP_LOGE(TAG, "i2s_channel_init_std_mode failed");
    return false;
  }
  i2s_channel_enable(tx);
  i2s_channel_enable(rx);  // enable both and NEVER disable mid-call
  this->i2s_tx_ = tx;
  this->i2s_rx_ = rx;

  this->uplink_rb_ = xRingbufferCreate(4096, RINGBUF_TYPE_BYTEBUF);
  this->downlink_rb_ = xRingbufferCreate(8192, RINGBUF_TYPE_BYTEBUF);
  if (!this->uplink_rb_ || !this->downlink_rb_) {
    ESP_LOGE(TAG, "ring buffer alloc failed");
    return false;
  }
  xTaskCreatePinnedToCore(&SipVoip::audio_task_trampoline_, "sip_i2s", 4096, this, 18,
                          &this->audio_task_handle_, 0);
  return true;
}

void SipVoip::audio_task_trampoline_(void *arg) { static_cast<SipVoip *>(arg)->audio_task_(); }

// Free-running: mic -> G.711 -> uplink_rb ; downlink_rb -> G.711 -> speaker (silence if idle).
void SipVoip::audio_task_() {
  static int16_t pcm[SIP_FRAME_SAMPLES];
  static uint8_t g711[SIP_FRAME_G711_BYTES];
  auto *tx = (i2s_chan_handle_t) this->i2s_tx_;
  auto *rx = (i2s_chan_handle_t) this->i2s_rx_;
  const bool alaw = (this->codec_ == 1);
  while (true) {
    size_t got = 0;
    // --- capture (paces the loop ~20 ms) ---
    if (i2s_channel_read(rx, pcm, SIP_FRAME_PCM_BYTES, &got, pdMS_TO_TICKS(100)) == ESP_OK &&
        got == SIP_FRAME_PCM_BYTES && this->in_call_) {
      for (int i = 0; i < SIP_FRAME_SAMPLES; i++)
        g711[i] = alaw ? linear_to_alaw(pcm[i]) : linear_to_ulaw(pcm[i]);
      xRingbufferSend(this->uplink_rb_, g711, SIP_FRAME_G711_BYTES, 0);
    }
    // --- playback ---
    size_t n = 0;
    uint8_t *in = (uint8_t *) xRingbufferReceiveUpTo(this->downlink_rb_, &n, 0,
                                                     SIP_FRAME_G711_BYTES);
    if (in && n > 0) {
      for (size_t i = 0; i < n; i++)
        pcm[i] = alaw ? alaw_to_linear(in[i]) : ulaw_to_linear(in[i]);
      vRingbufferReturnItem(this->downlink_rb_, in);
      size_t put = 0;
      i2s_channel_write(tx, pcm, n * 2, &put, pdMS_TO_TICKS(100));
    } else {
      // nothing to play -> write silence so BCLK never gaps
      memset(pcm, 0, SIP_FRAME_PCM_BYTES);
      size_t put = 0;
      i2s_channel_write(tx, pcm, SIP_FRAME_PCM_BYTES, &put, pdMS_TO_TICKS(100));
    }
  }
}

// ---------------------------------------------------------------------------
// esp_rtc callbacks (run in the engine's own tasks — move bytes only, no blocking)
// ---------------------------------------------------------------------------
static int cb_send_audio(unsigned char *data, int len, void *ctx) {
  return g_self ? g_self->on_send_audio(data, len) : 0;
}
static int cb_recv_audio(unsigned char *data, int len, void *ctx) {
  return g_self ? g_self->on_recv_audio(data, len) : len;
}
static int cb_event(esp_rtc_event_t ev, void *ctx) {
  if (g_self) g_self->on_rtc_event((int) ev);
  return 0;
}

int SipVoip::on_send_audio(uint8_t *data, int len) {
  size_t got = 0;
  void *p = xRingbufferReceiveUpTo(this->uplink_rb_, &got, 0, len);
  if (!p) return 0;  // engine pads with silence
  memcpy(data, p, got);
  vRingbufferReturnItem(this->uplink_rb_, p);
  return (int) got;
}
int SipVoip::on_recv_audio(uint8_t *data, int len) {
  xRingbufferSend(this->downlink_rb_, data, len, 0);
  return len;
}
void SipVoip::on_rtc_event(int event) {
  switch (event) {
    case ESP_RTC_EVENT_REGISTERED: ESP_LOGI(TAG, "SIP registered"); break;
    case ESP_RTC_EVENT_UNREGISTERED: ESP_LOGW(TAG, "SIP unregistered"); break;
    case ESP_RTC_EVENT_INCOMING:
      ESP_LOGI(TAG, "Incoming call");
      if (this->auto_answer_ && !this->in_call_) this->answer_pending_ = true;
      break;
    case ESP_RTC_EVENT_CALL_ANSWERED: ESP_LOGI(TAG, "Call connected"); this->in_call_ = true; break;
    case ESP_RTC_EVENT_HANGUP: ESP_LOGI(TAG, "Call ended"); this->in_call_ = false; break;
    case ESP_RTC_EVENT_ERROR: ESP_LOGW(TAG, "SIP error"); break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------
void SipVoip::setup() {
  g_self = this;
  ESP_LOGCONFIG(TAG, "Setting up sip_voip (%s@%s:%u)", this->username_.c_str(),
                this->server_.c_str(), this->port_);
  this->audio_ready_ = this->init_audio_();
  if (!this->audio_ready_) {
    ESP_LOGE(TAG, "audio init failed");
    this->mark_failed();
  }
}

bool SipVoip::start_engine_() {
  std::string local = network::get_ip_address().str();  // station IP (REQUIRED by esp_rtc)
  static std::string uri;
  uri = "udp://" + this->username_ + ":" + this->password_ + "@" + this->server_ + ":" +
        std::to_string(this->port_);
  static std::string local_copy;
  local_copy = local;

  static esp_rtc_data_cb_t cbs;
  cbs = {};
  cbs.send_audio = cb_send_audio;
  cbs.receive_audio = cb_recv_audio;

  static esp_rtc_config_t cfg;
  cfg = {};
  cfg.uri = uri.c_str();
  cfg.local_addr = local_copy.c_str();
  cfg.acodec_type = (this->codec_ == 1) ? RTC_ACODEC_G711A : RTC_ACODEC_G711U;
  cfg.data_cb = &cbs;
  cfg.event_handler = cb_event;
  cfg.ctx = this;
  cfg.register_interval = 3600;
  cfg.keepalive = 30;
  cfg.srtp_mode = ESP_RTC_SRTP_OFF;
  cfg.user_agent = "ESPHome-A1S";

  this->rtc_ = (void *) esp_rtc_service_init(&cfg);  // registers automatically
  if (this->rtc_ == nullptr) {
    ESP_LOGE(TAG, "esp_rtc_service_init failed");
    return false;
  }
  ESP_LOGI(TAG, "esp_rtc started, uri=%s local=%s", uri.c_str(), local_copy.c_str());
  return true;
}

void SipVoip::loop() {
  if (!this->audio_ready_) return;
  if (!this->engine_started_ && network::is_connected()) {
    this->engine_started_ = this->start_engine_();
  }
  if (this->answer_pending_) {
    this->answer_pending_ = false;
    this->answer();
  }
}

void SipVoip::call(const std::string &dest) {
  if (this->rtc_) esp_rtc_call((esp_rtc_handle_t) this->rtc_, dest.c_str());
}
void SipVoip::answer() {
  if (this->rtc_) esp_rtc_answer((esp_rtc_handle_t) this->rtc_);
}
void SipVoip::hangup() {
  if (this->rtc_) esp_rtc_bye((esp_rtc_handle_t) this->rtc_);
}

void SipVoip::dump_config() {
  ESP_LOGCONFIG(TAG, "sip_voip:");
  ESP_LOGCONFIG(TAG, "  Server: %s:%u", this->server_.c_str(), this->port_);
  ESP_LOGCONFIG(TAG, "  Username: %s", this->username_.c_str());
  ESP_LOGCONFIG(TAG, "  Codec: %s", this->codec_ == 1 ? "PCMA (G711A)" : "PCMU (G711U)");
  ESP_LOGCONFIG(TAG, "  Auto-answer: %s", YESNO(this->auto_answer_));
}

}  // namespace sip_voip
}  // namespace esphome
