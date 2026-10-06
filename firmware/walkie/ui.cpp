#include "ui.h"
#include <Arduino.h>
#include <math.h>
#include "audio.h"
#include "config.h"
#include "net.h"
#include "settings.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include "state.h"

// ─────────────── кольцо WS2812 через RMT ───────────────

static rmt_data_t s_bits[LED_COUNT * 24];
static uint8_t s_rgb[LED_COUNT][3];
static bool s_led_ok = false;

struct Rgb {
  uint8_t r, g, b;
};
static const Rgb GREEN = {0, 255, 40}, RED = {255, 0, 0}, BLUE = {0, 70, 255}, ORANGE = {255, 70, 0},
                 PURPLE = {150, 0, 255}, WHITE = {255, 255, 255}, AMBER = {255, 140, 0}, CYAN = {0, 200, 255};

static void set(int i, Rgb c, float k) {
  if (i < 0 || i >= LED_COUNT) return;
  // гамма — только для анимации (k), ползунок яркости действует линейно:
  // иначе при заводских настройках тусклые цвета округлялись до нуля и кольцо не горело
  float b = (g_set.led / 100.0f) * 0.6f;   // потолок: полное белое кольцо — не больше ~0,4 А
  k = fminf(fmaxf(k, 0.0f), 1.0f);
  float kk = powf(k, 2.2f) * b;
  const uint8_t ch[3] = {c.r, c.g, c.b};
  for (int j = 0; j < 3; j++) {
    float v = ch[j] * kk;
    // слабое, но видимое свечение не пропадает при округлении
    s_rgb[i][j] = (ch[j] && k > 0.02f && g_set.led) ? (uint8_t)fmaxf(1.0f, v + 0.5f) : (uint8_t)(v + 0.5f);
  }
}

static void fill(Rgb c, float k) {
  for (int i = 0; i < LED_COUNT; i++) set(i, c, k);
}

static void show() {
  if (!s_led_ok) return;
  int k = 0;
  for (int i = 0; i < LED_COUNT; i++) {
    uint8_t grb[3] = {s_rgb[i][1], s_rgb[i][0], s_rgb[i][2]};
    for (int j = 0; j < 3; j++) {
      for (int bit = 7; bit >= 0; bit--) {
        bool one = grb[j] & (1 << bit);
        s_bits[k].level0 = 1;
        s_bits[k].duration0 = one ? 8 : 4;    // 0,8 / 0,4 мкс при 10 МГц
        s_bits[k].level1 = 0;
        s_bits[k].duration1 = one ? 4 : 8;
        k++;
      }
    }
  }
  rmtWrite(PIN_LED, s_bits, LED_COUNT * 24, 50);
}

// ─────────────── регулятор (KY-040) ───────────────

// Таблица полного шага (Ben Buxton): один щелчок = один шаг, дребезг не страшен.
// Прерывание может прийти во время записи во флеш (настройки, обновление) — поэтому и таблица,
// и чтение выводов только из ОЗУ/регистров, без digitalRead.
static DRAM_ATTR const uint8_t kEnc[7][4] = {
    {0x0, 0x2, 0x4, 0x0}, {0x3, 0x0, 0x1, 0x10}, {0x3, 0x2, 0x0, 0x0}, {0x3, 0x2, 0x1, 0x0},
    {0x6, 0x0, 0x4, 0x0}, {0x6, 0x5, 0x0, 0x20}, {0x6, 0x5, 0x4, 0x0},
};
static volatile uint8_t s_enc_state = 0;
static volatile int s_enc_steps = 0;

static inline IRAM_ATTR uint8_t pin_level(int pin) {
  return pin < 32 ? (REG_READ(GPIO_IN_REG) >> pin) & 1 : (REG_READ(GPIO_IN1_REG) >> (pin - 32)) & 1;
}

static void IRAM_ATTR enc_isr() {
  uint8_t pins = (pin_level(PIN_ENC_B) << 1) | pin_level(PIN_ENC_A);
  s_enc_state = kEnc[s_enc_state & 0x0F][pins];
  if (s_enc_state & 0x10) s_enc_steps = s_enc_steps + 1;
  if (s_enc_state & 0x20) s_enc_steps = s_enc_steps - 1;
}

// ─────────────── состояние ───────────────

static bool s_ptt_raw = false, s_ptt = false;
static uint32_t s_ptt_edge = 0;
static bool s_sw_raw = false, s_sw = false;
static uint32_t s_sw_edge = 0, s_sw_down_ms = 0;
static uint32_t s_volume_until = 0;
static uint32_t s_frame_ms = 0;
static bool s_reset_armed = false;
static uint32_t s_battery_until = 0;

void ui_begin() {
  pinMode(PIN_PTT, INPUT_PULLUP);
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  attachInterrupt(PIN_ENC_A, enc_isr, CHANGE);
  attachInterrupt(PIN_ENC_B, enc_isr, CHANGE);
  s_led_ok = rmtInit(PIN_LED, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_4, 10000000);
  fill(WHITE, 0.4f);
  show();
}

bool ui_ptt_held_at_boot() {
  uint32_t t0 = millis();
  while (millis() - t0 < 1500) {
    if (digitalRead(PIN_PTT) != LOW) return false;
    fill(PURPLE, (millis() - t0) / 1500.0f);
    show();
    delay(20);
  }
  return true;
}

void ui_show_volume() { s_volume_until = millis() + 1500; }

static void change_volume(int d) {
  int v = (int)g_set.volume + d;
  v = v < 0 ? 0 : (v > 20 ? 20 : v);
  if (v == g_set.volume) return;
  g_set.volume = (uint8_t)v;
  settings_save_later();
  ui_show_volume();
  if (g_st.muted) g_st.muted = false;
  if (!g_st.rx_active) audio_tone(Tone::CLICK);   // слышно, какая стала громкость
}

static void buttons(uint32_t now) {
  bool raw = digitalRead(PIN_PTT) == LOW;
  if (raw != s_ptt_raw) {
    s_ptt_raw = raw;
    s_ptt_edge = now;
  }
  if (raw != s_ptt && now - s_ptt_edge > 25) {
    s_ptt = raw;
    net_ptt(s_ptt);
  }

  int steps;
  noInterrupts();
  steps = s_enc_steps;
  s_enc_steps = 0;
  interrupts();
  // знак «минус»: 29.09 на рации 1 (CLK → GPIO2, DT → GPIO42, как в схеме) по часовой громкость уменьшалась
  if (steps) change_volume(g_set.enc_invert ? steps : -steps);

  bool sw = digitalRead(PIN_ENC_SW) == LOW;
  if (sw != s_sw_raw) {
    s_sw_raw = sw;
    s_sw_edge = now;
  }
  if (sw != s_sw && now - s_sw_edge > 30) {
    s_sw = sw;
    if (s_sw) {
      s_sw_down_ms = now;
    } else {
      uint32_t held = now - s_sw_down_ms;
      if (held < 1000) {             // короткое нажатие: звук выкл/вкл
        g_st.muted = !g_st.muted;
        if (!g_st.muted) audio_tone(Tone::CLICK);
        ui_show_volume();
      } else if (held < 5000) {      // подержать 1–4 с: показать заряд аккумулятора
        s_battery_until = now + 2500;
      }
      s_reset_armed = false;
    }
  }
  if (s_sw && now - s_sw_down_ms > 5000) s_reset_armed = true;   // держат: предупреждаем миганием
  if (s_sw && now - s_sw_down_ms > 10000) {                        // 10 с — сброс всех настроек
    logf("сброс настроек регулятором");
    fill(RED, 1.0f);
    show();
    settings_factory_reset();
    delay(500);
    ESP.restart();
  }
}

static void vu(Rgb c, uint8_t level, float base) {
  int n = (int)((level / 255.0f) * LED_COUNT * 1.6f + 0.5f);   // речь редко доходит до верха шкалы
  if (n > LED_COUNT) n = LED_COUNT;
  for (int i = 0; i < LED_COUNT; i++) set(i, c, i < n ? 1.0f : base);
}

static void render(uint32_t t) {
  fill({0, 0, 0}, 0);
  float breathe = 0.5f + 0.5f * sinf(t / 600.0f);
  bool blink = (t / 120) % 2;
  if (s_reset_armed) {
    fill(RED, (t / 100) % 2 ? 1.0f : 0.1f);
  } else if (g_st.updating) {
    int n = g_st.update_pct * LED_COUNT / 100;
    for (int i = 0; i < LED_COUNT; i++) set(i, BLUE, i < n ? 1.0f : 0.12f);
  } else if (g_st.flash != Flash::NONE && t - g_st.flash_ms < 900) {
    fill(g_st.flash == Flash::BUSY ? RED : ORANGE, blink ? 1.0f : 0.05f);
  } else if (t < s_battery_until) {
    uint8_t p = g_st.bat_pct;
    if (p == 255 && g_st.bat_measuring) {
      fill(WHITE, 0.15f);            // ещё измеряется (первые 20 с после включения)
    } else if (p == 255) {
      fill(BLUE, 0.5f);              // питание от USB
    } else {
      int n = (p * LED_COUNT + 50) / 100;
      Rgb c = p <= BAT_LOW_PCT ? RED : (p <= 40 ? AMBER : GREEN);
      for (int i = 0; i < LED_COUNT; i++) set(i, c, i < n ? 0.9f : 0.04f);
    }
  } else if (t < s_volume_until) {
    int n = (g_set.volume * LED_COUNT + 10) / 20;
    for (int i = 0; i < LED_COUNT; i++) set(i, g_st.muted ? PURPLE : WHITE, i < n ? 0.8f : 0.04f);
  } else if (g_st.tx == Tx::TALK) {
    vu(RED, g_st.tx_level, 0.3f);
  } else if (g_st.tx == Tx::REQUEST || g_st.tx == Tx::CHIRP) {
    int p = (t / 40) % LED_COUNT;
    set(p, WHITE, 1.0f);
    set((p + LED_COUNT / 2) % LED_COUNT, WHITE, 1.0f);
  } else if (g_st.rx_active) {
    if (g_st.muted)
      fill(CYAN, 0.3f + 0.4f * breathe);    // звук выключен, но кто-то говорит — видно
    else
      vu(GREEN, g_st.rx_level, 0.25f);
  } else if (g_st.portal && g_st.link != Link::OK) {
    fill(PURPLE, 0.1f + 0.6f * breathe);
  } else {
    switch (g_st.link) {
      case Link::NO_WIFI:
        set((t / 90) % LED_COUNT, BLUE, 0.9f);
        break;
      case Link::NO_KEY:
      case Link::WRONG_KEY:
        for (int i = 0; i < LED_COUNT; i++) set(i, ((i + t / 400) % 2) ? PURPLE : RED, 0.5f);
        break;
      case Link::NO_HUB:
        set(0, ORANGE, (t / 500) % 2 ? 0.9f : 0.0f);
        set(LED_COUNT / 2, ORANGE, (t / 500) % 2 ? 0.9f : 0.0f);
        break;
      case Link::OK: {
        // в простое — короткая вспышка раз в 3 с (на аккумуляторе постоянное свечение зря тратит заряд)
        bool beat = (t % 3000) < 90;
        bool low = g_st.bat_low;
        Rgb c = low ? RED : (g_st.peers_ok ? GREEN : AMBER);   // янтарный: другой мост недоступен
        if (g_st.bat_pct == 255 && !g_st.bat_measuring) fill(c, 0.1f);   // от USB — можно светить постоянно
        else if (beat) { set(0, c, 0.7f); set(LED_COUNT / 2, c, 0.7f); }
        if (g_st.muted) set(LED_COUNT / 4, PURPLE, 0.6f);
        break;
      }
    }
  }
}

void ui_loop() {
  uint32_t now = millis();
  buttons(now);
  if (now - s_frame_ms >= 33) {
    s_frame_ms = now;
    render(now);
    show();
  }
}
