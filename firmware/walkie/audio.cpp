#include "audio.h"
#include <Arduino.h>
#include <math.h>
#include "adpcm.h"
#include "config.h"
#include "howl.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "jitter.h"
#include "settings.h"
#include "state.h"

static const int N = wt::FRAME_SAMPLES;

static i2s_chan_handle_t s_rx = nullptr;
static i2s_chan_handle_t s_tx = nullptr;
static QueueHandle_t s_txq = nullptr;
static SemaphoreHandle_t s_jb_mutex = nullptr;
static JitterBuffer s_jb;
static uint32_t s_ended_talker = 0, s_ended_burst = 0;    // закончившийся поток: запоздавшие кадры не играть
static volatile bool s_streaming = false;
static volatile uint16_t s_test_hz = 0;
static volatile int32_t s_raw_mn[2] = {INT32_MAX, INT32_MAX}, s_raw_mx[2] = {INT32_MIN, INT32_MIN};
static volatile uint32_t s_raw_frames = 0;                   // служебная проверка: тон вместо микрофона
static volatile Tone s_tone_req = Tone::NONE;

// ─────────────── сигналы ───────────────

struct Seg {
  uint16_t hz;
  uint16_t ms;
};
static const Seg kPermit[] = {{1400, 50}, {0, 15}, {1800, 70}};                 // «можно говорить»
static const Seg kBusy[] = {{420, 110}, {0, 70}, {420, 110}, {0, 70}, {420, 110}};  // «занято»
static const Seg kError[] = {{330, 380}};                                       // «нет связи»
static const Seg kRoger[] = {{1800, 45}, {1250, 60}};                           // «конец приёма»
static const Seg kConnected[] = {{880, 70}, {0, 30}, {1320, 90}};               // «подключилась»
static const Seg kLost[] = {{600, 120}, {400, 180}};                            // «пропал мост»
static const Seg kClick[] = {{1000, 35}};                                       // щелчок громкости
static const Seg kLowBat[] = {{700, 90}, {0, 60}, {520, 90}, {0, 60}, {400, 140}}; // «садится аккумулятор»

class ToneGen {
 public:
  void start(Tone t) {
    switch (t) {
      case Tone::PERMIT: set(kPermit, 3); break;
      case Tone::BUSY: set(kBusy, 5); break;
      case Tone::ERROR: set(kError, 1); break;
      case Tone::ROGER: set(kRoger, 2); break;
      case Tone::CONNECTED: set(kConnected, 3); break;
      case Tone::LOST: set(kLost, 2); break;
      case Tone::CLICK: set(kClick, 1); break;
      case Tone::LOW_BATTERY: set(kLowBat, 5); break;
      default: segs_ = nullptr;
    }
  }
  bool active() const { return segs_ != nullptr; }
  // добавляет сигнал к acc (float), амплитуда amp
  void render(float* acc, int n, float amp) {
    for (int k = 0; k < n && segs_; k++) {
      const Seg& s = segs_[i_];
      if (s.hz) {
        float env = 1.0f;
        const uint32_t fade = 48;  // 3 мс
        if (pos_ < fade) env = (float)pos_ / fade;
        if (len_ - pos_ < fade) env = fminf(env, (float)(len_ - pos_) / fade);
        acc[k] += sinf(phase_) * amp * env;
        phase_ += 2.0f * (float)M_PI * s.hz / wt::SAMPLE_RATE;
        if (phase_ > 2.0f * (float)M_PI) phase_ -= 2.0f * (float)M_PI;
      }
      if (++pos_ >= len_) {
        pos_ = 0;
        phase_ = 0;
        if (++i_ >= n_) {
          segs_ = nullptr;
        } else {
          len_ = (uint32_t)segs_[i_].ms * wt::SAMPLE_RATE / 1000;
        }
      }
    }
  }

 private:
  void set(const Seg* s, uint8_t n) {
    segs_ = s;
    n_ = n;
    i_ = 0;
    pos_ = 0;
    phase_ = 0;
    len_ = (uint32_t)s[0].ms * wt::SAMPLE_RATE / 1000;
  }
  const Seg* segs_ = nullptr;
  uint8_t n_ = 0, i_ = 0;
  uint32_t pos_ = 0, len_ = 0;
  float phase_ = 0;
};

void audio_tone(Tone t) { s_tone_req = t; }

float audio_volume_gain() {
  uint8_t v = g_set.volume;
  if (v == 0) return 0.0f;
  return powf(10.0f, (float)((int)v - 20) * 2.0f / 20.0f);   // шаг 2 дБ, 20 = 0 дБ
}

// ─────────────── I2S ───────────────

static bool init_i2s() {
  // Микрофон: I2S1, 16 кГц, слоты по 32 бита, оба канала — нужный выбираем программно
  i2s_chan_config_t rc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  rc.dma_desc_num = 4;
  rc.dma_frame_num = N;
  if (i2s_new_channel(&rc, nullptr, &s_rx) != ESP_OK) return false;
  i2s_std_config_t rs = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(wt::SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = (gpio_num_t)PIN_MIC_SCK,
              .ws = (gpio_num_t)PIN_MIC_WS,
              .dout = I2S_GPIO_UNUSED,
              .din = (gpio_num_t)PIN_MIC_SD,
              .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
          },
  };
  if (i2s_channel_init_std_mode(s_rx, &rs) != ESP_OK) return false;
  if (i2s_channel_enable(s_rx) != ESP_OK) return false;

  // Динамик: I2S0 (режим PDM есть только у него)
  i2s_chan_config_t tc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  tc.dma_desc_num = 4;
  tc.dma_frame_num = N;
  tc.auto_clear = true;   // нет данных — идут нули, а не повтор хвоста
  if (i2s_new_channel(&tc, &s_tx, nullptr) != ESP_OK) return false;
  if (g_set.amp_pdm) {
    // Аналоговый усилитель (PAM8403): поток импульсов на один вывод, фильтр RC делает из него звук.
    // Настройки Espressif для режима «PDM → ЦАП через фильтр» (высокое отношение сигнал/шум).
    i2s_pdm_tx_config_t pc = {
        .clk_cfg = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(wt::SAMPLE_RATE),
        .slot_cfg = I2S_PDM_TX_SLOT_PCM_FMT_DAC_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .clk = (gpio_num_t)PIN_PDM_CLK,
                .dout = (gpio_num_t)PIN_PDM_OUT,
                .dout2 = I2S_GPIO_UNUSED,
                .invert_flags = {.clk_inv = false},
            },
    };
    if (i2s_channel_init_pdm_tx_mode(s_tx, &pc) != ESP_OK) return false;
    return i2s_channel_enable(s_tx) == ESP_OK;
  }
  // Цифровой усилитель MAX98357A: 16 кГц, 16 бит, стерео (одинаковое в оба канала)
  i2s_std_config_t ts = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(wt::SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = (gpio_num_t)PIN_AMP_BCLK,
              .ws = (gpio_num_t)PIN_AMP_LRC,
              .dout = (gpio_num_t)PIN_AMP_DIN,
              .din = I2S_GPIO_UNUSED,
              .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
          },
  };
  if (i2s_channel_init_std_mode(s_tx, &ts) != ESP_OK) return false;
  if (i2s_channel_enable(s_tx) != ESP_OK) return false;
  return true;
}

// ─────────────── микрофон ───────────────

static HowlGuard s_howl;                // защита от свиста между рациями — только в задаче микрофона
static volatile uint32_t s_howl_found = 0;
static volatile uint16_t s_howl_hz = 0;
void audio_howl_info(uint32_t& found, uint16_t& last_hz) {
  found = s_howl_found;
  last_hz = s_howl_hz;
}

static void mic_task(void*) {
  static int32_t raw[N * 2];
  static float y[N];
  static int16_t pcm[N];
  static TxFrame fr;
  AdpcmState enc;
  float hp_x = 0, hp_y = 0;
  const float a = 0.954f;       // срез низа ~120 Гц: гул и «бубнение» не нужны
  float gain_db = 24.0f, prev_g = powf(10.0f, gain_db / 20.0f);
  bool was = false;
  for (;;) {
    size_t got = 0;
    if (i2s_channel_read(s_rx, raw, sizeof(raw), &got, portMAX_DELAY) != ESP_OK || got != sizeof(raw)) continue;
    const int ch = g_set.mic_right ? 1 : 0;
    for (int i = 0; i < N; i++)                       // диагностика: что реально приходит по SD, до обработки
      for (int c = 0; c < 2; c++) {
        int32_t v = raw[2 * i + c] >> 8;
        if (v < s_raw_mn[c]) s_raw_mn[c] = v;
        if (v > s_raw_mx[c]) s_raw_mx[c] = v;
      }
    s_raw_frames = s_raw_frames + 1;
    float peak = 0, sum2 = 0;
    static float test_ph = 0;
    const uint16_t test_hz = s_test_hz;
    for (int i = 0; i < N; i++) {
      float x = (float)(raw[2 * i + ch] >> 8) * (1.0f / 256.0f);   // 24 бита → шкала 16 бит
      if (test_hz) {                                                  // проверка без микрофона
        x = 400.0f * sinf(test_ph);
        test_ph += 6.2831853f * test_hz / wt::SAMPLE_RATE;
        if (test_ph > 6.2831853f) test_ph -= 6.2831853f;
      }
      hp_y = a * (hp_y + x - hp_x);
      hp_x = x;
      y[i] = hp_y;
    }
    const bool st = s_streaming;
    if (st && !test_hz) {                           // свист между рациями (howl.h): найти и вырезать
      uint32_t now = millis();
      if (!was) s_howl.burst_start(now);
      if (s_howl.process(y, now)) {
        s_howl_found = s_howl_found + 1;
        s_howl_hz = (uint16_t)s_howl.notch_hz(s_howl.notches() - 1);
      }
    }
    for (int i = 0; i < N; i++) {
      float ay = fabsf(y[i]);
      if (ay > peak) peak = ay;
      sum2 += y[i] * y[i];
    }
    float rms = sqrtf(sum2 / N);
    if (g_set.agc) {
      if (rms > AGC_GATE_RMS) {
        float want = 20.0f * log10f(AGC_TARGET_RMS / rms);
        want = fminf(fmaxf(want, AGC_MIN_DB), AGC_MAX_DB);
        if (want < gain_db)
          gain_db += (want - gain_db) * 0.3f;       // громко — убавить быстро
        else if (!(st && s_howl.hold_agc()))        // после свиста в этой передаче — не прибавлять
          gain_db += fminf(0.25f, want - gain_db);  // тихо — прибавлять плавно (12 дБ/с)
      }
    } else {
      gain_db = g_set.mic_gain;
    }
    float g = powf(10.0f, gain_db / 20.0f);
    if (peak * g > 29000.0f) g = 29000.0f / peak;   // ограничитель: без хрипа на крике
    float out_peak = 0;
    for (int i = 0; i < N; i++) {
      float gi = prev_g + (g - prev_g) * (float)i / N;
      float v = y[i] * gi;
      v = fminf(fmaxf(v, -32768.0f), 32767.0f);
      pcm[i] = (int16_t)v;
      out_peak = fmaxf(out_peak, fabsf(v));
    }
    prev_g = g;
    uint8_t lvl = (uint8_t)fminf(255.0f, out_peak / 128.0f);
    if (st) {
      if (!was) enc = AdpcmState();
      adpcm_encode_frame(enc, pcm, N, fr.data);
      fr.level = lvl;
      g_st.tx_level = lvl;
      if (xQueueSend(s_txq, &fr, 0) != pdTRUE) {
        TxFrame drop;                       // сеть не успевает — выкинуть самый старый
        xQueueReceive(s_txq, &drop, 0);
        xQueueSend(s_txq, &fr, 0);
      }
    } else if (was) {
      g_st.tx_level = 0;
    }
    was = st;
  }
}

void audio_set_streaming(bool on) {
  if (on && !s_streaming) xQueueReset(s_txq);
  s_streaming = on;
}

bool audio_tx_pop(TxFrame& f) { return xQueueReceive(s_txq, &f, 0) == pdTRUE; }

void audio_test_tone(uint16_t hz) { s_test_hz = hz; }

void audio_mic_raw(int32_t mn[2], int32_t mx[2], uint32_t& frames) {
  for (int c = 0; c < 2; c++) {
    mn[c] = s_raw_mn[c]; mx[c] = s_raw_mx[c];
    s_raw_mn[c] = INT32_MAX; s_raw_mx[c] = INT32_MIN;
  }
  frames = s_raw_frames; s_raw_frames = 0;
}

// ─────────────── приём ───────────────

void audio_rx_frame(uint32_t talker, uint32_t burst, uint16_t seq, const uint8_t* frame, uint8_t level) {
  xSemaphoreTake(s_jb_mutex, portMAX_DELAY);
  if (!s_jb.matches(talker, burst)) {
    if (talker == s_ended_talker && burst == s_ended_burst) {   // хвост уже закончившейся передачи
      xSemaphoreGive(s_jb_mutex);
      return;
    }
    s_jb.begin(talker, burst, millis());
  }
  s_jb.push(talker, burst, seq, frame, level, millis());
  xSemaphoreGive(s_jb_mutex);
  g_st.last_activity_ms = millis();
}

void audio_rx_end(uint32_t talker, uint32_t burst) {
  xSemaphoreTake(s_jb_mutex, portMAX_DELAY);
  s_jb.end(talker, burst);
  xSemaphoreGive(s_jb_mutex);
}

void audio_rx_stop() {
  xSemaphoreTake(s_jb_mutex, portMAX_DELAY);
  if (s_jb.active()) {
    s_ended_talker = s_jb.talker();
    s_ended_burst = s_jb.burst();
    s_jb.stop();
  }
  xSemaphoreGive(s_jb_mutex);
}

static void spk_task(void*) {
  static int16_t pcm[N], last[N];
  static float acc[N];
  static int16_t out[N * 2];
  static uint8_t frame[wt::AUDIO_ENC_LEN];
  ToneGen tg;
  int plc = 0;
  uint32_t loud_ms = 0;
  bool amp = false;
  for (;;) {
    uint32_t now = millis();
    Tone req = s_tone_req;
    if (req != Tone::NONE) {
      s_tone_req = Tone::NONE;
      tg.start(req);
    }
    uint8_t lvl = 0;
    xSemaphoreTake(s_jb_mutex, portMAX_DELAY);
    JitterBuffer::Pop r = s_jb.pop(frame, &lvl, now);
    bool active = s_jb.active();
    if (r == JitterBuffer::ENDED) {       // запомнить, чтобы запоздавшие кадры не начали «новый» приём
      s_ended_talker = s_jb.talker();
      s_ended_burst = s_jb.burst();
    }
    g_st.rx_played = s_jb.stat_played;
    g_st.rx_lost = s_jb.stat_lost;
    g_st.rx_late = s_jb.stat_late;
    g_st.rx_underrun = s_jb.stat_underrun;
    xSemaphoreGive(s_jb_mutex);
    g_st.rx_active = active;
    bool sound = false;
    switch (r) {
      case JitterBuffer::FRAME:
        adpcm_decode_frame(frame, N, pcm);
        memcpy(last, pcm, sizeof(pcm));
        plc = 0;
        sound = true;
        g_st.rx_level = lvl;
        break;
      case JitterBuffer::MISSING: {
        // потерянный кадр: повторить предыдущий, затухая; дальше — тишина
        static const float fade[3] = {0.6f, 0.3f, 0.1f};
        for (int i = 0; i < N; i++) pcm[i] = plc < 3 ? (int16_t)(last[i] * fade[plc]) : 0;
        plc++;
        sound = true;
        break;
      }
      case JitterBuffer::WAIT:
        memset(pcm, 0, sizeof(pcm));
        sound = true;      // усилитель включаем заранее, пока копится запас
        break;
      case JitterBuffer::ENDED:
        memset(pcm, 0, sizeof(pcm));
        g_st.rx_level = 0;
        if (g_set.roger && !g_st.muted) tg.start(Tone::ROGER);
        break;
      default:
        memset(pcm, 0, sizeof(pcm));
        g_st.rx_level = 0;
    }
    float vol = g_st.muted ? 0.0f : audio_volume_gain();
    for (int i = 0; i < N; i++) acc[i] = pcm[i] * vol;
    if (tg.active()) {
      // сигналы слышны и при выключенном звуке — тихо, чтобы кнопка «отвечала»
      float tv = fmaxf(g_st.muted ? 0.0f : vol, 0.03f);
      tg.render(acc, N, 8000.0f * tv);
      sound = true;
    }
    const bool mono = g_set.amp_pdm;   // PDM — один канал, I2S-усилителю — оба
    for (int i = 0; i < N; i++) {
      int16_t v = (int16_t)fminf(fmaxf(acc[i], -32768.0f), 32767.0f);
      if (mono)
        out[i] = v;
      else
        out[2 * i] = out[2 * i + 1] = v;
    }
    if (sound) {
      loud_ms = now;
      if (!amp) {
        digitalWrite(PIN_AMP_SD, HIGH);
        amp = true;
      }
    } else if (amp && now - loud_ms > 3000) {
      digitalWrite(PIN_AMP_SD, LOW);
      amp = false;
    }
    size_t w = 0;
    i2s_channel_write(s_tx, out, mono ? N * sizeof(int16_t) : sizeof(out), &w, portMAX_DELAY);
  }
}

bool audio_begin() {
  pinMode(PIN_AMP_SD, OUTPUT);
  digitalWrite(PIN_AMP_SD, LOW);
  s_txq = xQueueCreate(8, sizeof(TxFrame));
  s_jb_mutex = xSemaphoreCreateMutex();
  if (!init_i2s()) {
    logf("I2S не запустился");
    return false;
  }
  xTaskCreatePinnedToCore(mic_task, "mic", 6144, nullptr, 6, nullptr, 1);
  xTaskCreatePinnedToCore(spk_task, "spk", 6144, nullptr, 7, nullptr, 1);
  return true;
}
