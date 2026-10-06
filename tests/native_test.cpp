// Проверка переносимых частей прошивки на компьютере (без ESP32):
//   clang++ -std=c++17 -I firmware/walkie tests/native_test.cpp firmware/walkie/adpcm.cpp firmware/walkie/jitter.cpp \
//           firmware/walkie/battery_est.cpp firmware/walkie/howl.cpp
// Запускается из tests/test_native.py — там же сверка ADPCM с эталоном на Python.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "adpcm.h"
#include "battery_est.h"
#include "howl.h"
#include "jitter.h"

static int fails = 0;
#define CHECK(cond, msg)                                    \
  do {                                                      \
    if (!(cond)) {                                          \
      fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); \
      fails++;                                              \
    }                                                       \
  } while (0)

// Тот же сигнал, что в tests/test_native.py
static void signal(int frame, int16_t* out) {
  for (int i = 0; i < 320; i++) {
    int n = frame * 320 + i;
    double v = 9000 * sin(2 * M_PI * 440 * n / 16000.0) + 3000 * sin(2 * M_PI * 1300 * n / 16000.0) +
               ((n * 7919) % 601) - 300;
    out[i] = (int16_t)lrint(v);
  }
}

static void adpcm_dump() {
  AdpcmState st;
  int16_t pcm[320], dec[320];
  uint8_t buf[163];
  for (int f = 0; f < 6; f++) {
    signal(f, pcm);
    adpcm_encode_frame(st, pcm, 320, buf);
    printf("ENC ");
    for (int i = 0; i < 163; i++) printf("%02x", buf[i]);
    printf("\n");
    adpcm_decode_frame(buf, 320, dec);
    long long e = 0, s = 0;
    for (int i = 0; i < 320; i++) {
      e += (long long)(pcm[i] - dec[i]) * (pcm[i] - dec[i]);
      s += (long long)pcm[i] * pcm[i];
    }
    if (f > 0) CHECK(10 * log10((double)s / (double)(e + 1)) > 18, "ADPCM SNR");
  }
}

static uint8_t frame_for(uint16_t seq) {
  return (uint8_t)(seq * 37 + 11);
}

static void push(JitterBuffer& jb, uint16_t seq, uint32_t t) {
  uint8_t f[wt::AUDIO_ENC_LEN];
  memset(f, frame_for(seq), sizeof(f));
  jb.push(1, 42, seq, f, 10, t);
}


// ── Заряд аккумулятора (battery_est): замер раз в секунду, как в прошивке ──
// Прогон секунд [from_s, to_s): f(s) → (напряжение, занята ли рация). Собирает опубликованные проценты.
struct BatRun {
  int min_pct = 1000, max_pct = -1;
  bool low_seen = false;
  uint32_t first_low_ms = 0;
  int first_pub_s = -1, first_pub_pct = -1;
};
template <typename F>
static BatRun bat_run(BatteryEstimator& e, int from_s, int to_s, F f, uint32_t t0 = 0) {
  BatRun r;
  for (int s = from_s; s < to_s; s++) {
    float mv;
    bool busy;
    f(s, mv, busy);
    uint32_t now = t0 + (uint32_t)s * 1000u;
    e.add(mv, now, busy);
    uint8_t p = e.pct();
    if (p != 255) {
      if (r.first_pub_s < 0) {
        r.first_pub_s = s;
        r.first_pub_pct = p;
      }
      if (p < r.min_pct) r.min_pct = p;
      if (p > r.max_pct) r.max_pct = p;
    }
    if (e.low() && !r.low_seen) {
      r.low_seen = true;
      r.first_low_ms = now;
    }
  }
  return r;
}
// Первая публикация: замеры с 15-й секунды (WARMUP_MS), нужно 5 спокойных → на 19-й
static const int FIRST_PUB_S = BatteryEstimator::WARMUP_MS / 1000 + BatteryEstimator::MIN_SAMPLES - 1;

static void test_bat_curve() {
  CHECK(battery_pct_from_mv(3200) == 0 && battery_pct_from_mv(3300) == 0, "кривая: пусто");
  CHECK(battery_pct_from_mv(4200) == 100, "кривая: полно");
  CHECK(battery_pct_from_mv(3550) == 20 && battery_pct_from_mv(3933) == 77 && battery_pct_from_mv(3700) == 43,
        "кривая: середина");
  for (int mv = 3300; mv < 4200; mv++)
    CHECK(battery_pct_from_mv((float)mv) <= battery_pct_from_mv((float)mv + 1), "кривая не убывает");
}

// 28.09: при включении Wi-Fi проседает напряжение — первые 5 с замер 3,30 В (0 %), дальше 3,55 В (20 %)
static void test_bat_boot_sag() {
  BatteryEstimator e(15);
  CHECK(e.measuring() && e.pct() == 255, "до замеров — «измеряется»");
  BatRun r = bat_run(e, 0, 180, [](int s, float& mv, bool& busy) { mv = s < 5 ? 3300 : 3550; busy = false; });
  CHECK(r.first_pub_s == FIRST_PUB_S, "заряд сообщается с 19-й секунды");
  CHECK(r.min_pct >= 19 && r.max_pct <= 21, "и сразу настоящие ~20 %");
  CHECK(!r.low_seen, "ложного «пора на зарядку» нет");
}

// Просадка всё первое время и чуть дольше (до 17-й секунды) — первое значение всё равно настоящее
static void test_bat_boot_long_sag() {
  for (int sag_end : {11, 13, 16, 18}) {
    BatteryEstimator e(15);
    BatRun r = bat_run(e, 0, 120, [sag_end](int s, float& mv, bool& busy) { mv = s < sag_end ? 3300 : 3700; busy = false; });
    CHECK(r.first_pub_pct >= 42 && r.min_pct >= 42, "первое опубликованное — настоящие ~43 %");
    CHECK(!r.low_seen, "низкий заряд не объявлен");
  }
}

// Включили — и сразу приём/разговор (находка проверки 29.09): до спокойных замеров заряд не сообщается вовсе
static void test_bat_boot_then_busy() {
  BatteryEstimator e(15);
  BatRun r = bat_run(e, 0, 120, [](int s, float& mv, bool& busy) {
    busy = s >= 5 && s < 40;
    mv = s < 5 ? 3300 : (busy ? 3400 : 3700);          // 3,70 В — это ~43 %
  });
  CHECK(r.first_pub_s == 39 + 3 + 4, "заряд — после разговора (последняя занятая секунда 39), 3 с отдыха и 5 замеров");
  CHECK(r.min_pct >= 42, "ни одного ложного низкого значения");
  BatteryEstimator e2(15);
  BatRun r2 = bat_run(e2, 0, 60, [](int s, float& mv, bool& busy) {   // короткий: с 16-й по 20-ю секунду
    busy = s >= 16 && s < 21;
    mv = busy ? 3400 : (s < 5 ? 3300 : 3700);
  });
  CHECK(r2.min_pct >= 42, "и после короткого приёма сразу за «привыканием»");
}

// Разговор и приём: просадка 0,4 В на 60 с и ещё 2 с после — не в счёт
static void test_bat_busy_ignored() {
  BatteryEstimator e(15);
  bat_run(e, 0, 60, [](int, float& mv, bool& busy) { mv = 3933; busy = false; });
  uint8_t before = e.pct();
  BatRun r = bat_run(e, 60, 220, [](int s, float& mv, bool& busy) {
    busy = s >= 70 && s < 130;
    mv = (s >= 70 && s < 132) ? 3533 : 3933;          // 2 с после разговора напряжение ещё не вернулось
  });
  CHECK(before == 77, "до разговора 77 %");
  CHECK(r.min_pct >= 76 && r.max_pct <= 78, "разговор не роняет заряд (было 78 → 70 %)");
}

// Долгий разговор по очереди 20 минут (10 с говорят, 1–2 с пауза, в паузе напряжение ещё просажено) —
// заряд не падает (находка второй проверки 29.09: через 5 мин было «6 %»), а после разговора сразу догоняет
static void test_bat_long_conversation() {
  for (int gap : {1, 2}) {
    BatteryEstimator e(15);
    bat_run(e, 0, 60, [](int, float& mv, bool& busy) { mv = 3700; busy = false; });
    BatRun r = bat_run(e, 60, 60 + 1200, [gap](int s, float& mv, bool& busy) {
      busy = (s % (10 + gap)) < 10;
      mv = 3400;                                        // и в паузах тоже
    });
    CHECK(r.min_pct >= 42, "разговор с паузами не роняет заряд");
    CHECK(!r.low_seen, "тревоги нет");
    BatRun r2 = bat_run(e, 1260, 1300, [](int, float& mv, bool& busy) { mv = 3650; busy = false; });
    CHECK(r2.min_pct >= 34 && e.pct() == 35, "после разговора — новые настоящие 35 %");
  }
}

// Кнопку прижали в сумке: после 100 с связь обрывается, но «занята» остаётся — через 5 мин замеры идут снова
static void test_bat_stuck_busy() {
  BatteryEstimator e(15);
  bat_run(e, 0, 60, [](int, float& mv, bool& busy) { mv = 3700; busy = false; });
  const int N = 3 * 3600;
  BatRun r = bat_run(e, 60, 60 + N, [](int s, float& mv, bool& busy) {
    busy = true;
    mv = 3700 - 350.0f * (s - 60) / N;                  // садится до 3,35 В (~3 %)
  });
  CHECK(r.min_pct <= 5, "заряд не застыл: к концу ~3 %");
  CHECK(r.low_seen, "и «пора на зарядку» объявлена");
}

// Короткие провалы без признака «занята» (подсветка на полную, сигнал, всплеск Wi-Fi) — отбрасываются
static void test_bat_short_dips() {
  BatteryEstimator e(15);
  bat_run(e, 0, 60, [](int, float& mv, bool& busy) { mv = 3600; busy = false; });
  uint8_t before = e.pct();
  BatRun r = bat_run(e, 60, 400, [](int s, float& mv, bool& busy) {
    busy = false;
    mv = (s % 40) < 6 ? 3300 : 3600;                   // каждые 40 с — 6 с просадки на 0,3 В
  });
  CHECK(before == 27, "до провалов 27 %");
  CHECK(r.min_pct >= 26, "провалы по 6 с не видны");
  CHECK(!r.low_seen, "и тревоги нет");
}

// Напряжение правда стало ниже и держится — оценка спускается к нему плавно, без «перелёта» вниз
static void test_bat_step_down() {
  BatteryEstimator e(15);
  bat_run(e, 0, 60, [](int, float& mv, bool& busy) { mv = 3933; busy = false; });
  BatRun r = bat_run(e, 60, 200, [](int, float& mv, bool& busy) { mv = 3700; busy = false; });
  CHECK(r.min_pct >= 43, "ниже настоящих 43 % не опускается");
  CHECK(e.pct() == 43, "и приходит к ним");
  BatteryEstimator e2(15);
  bat_run(e2, 0, 60, [](int, float& mv, bool& busy) { mv = 3933; busy = false; });
  bat_run(e2, 60, 60 + 45, [](int, float& mv, bool& busy) { mv = 3700; busy = false; });
  CHECK(e2.pct() <= 45, "за 45 с — почти пришла (окно 30 с + сглаживание)");
  BatteryEstimator e3(15);
  bat_run(e3, 0, 60, [](int, float& mv, bool& busy) { mv = 3933; busy = false; });
  bat_run(e3, 60, 60 + 31, [](int, float& mv, bool& busy) { mv = 3700; busy = false; });
  CHECK(e3.pct() >= 60, "вниз — плавно: в первую секунду после окна не падает сразу до 43 %");
}

// «30 с подряд»: заряд поднялся выше порога — отсчёт заново (15 % → 17 % → 15 %)
static void test_bat_low_hold_resets() {
  BatteryEstimator e(15);
  BatRun r = bat_run(e, 0, 200, [](int s, float& mv, bool& busy) {
    busy = false;
    mv = (s >= 39 && s < 79) ? 3520 : 3500;           // 3,50 В — 15 %, 3,52 В — 17 %
  });
  CHECK(r.low_seen && r.first_low_ms >= 135000, "тревога — только через 30 с после нового снижения");
}

// Аккумулятор правда садится: «пора на зарядку» — через 30 с низкого заряда, не раньше
static void test_bat_real_low() {
  BatteryEstimator e(15);
  BatRun r = bat_run(e, 0, 120, [](int, float& mv, bool& busy) { mv = 3400; busy = false; });
  CHECK(r.first_pub_s == FIRST_PUB_S && r.max_pct <= 7, "3,40 В — около 6 %");
  CHECK(r.low_seen && r.first_low_ms == (uint32_t)(FIRST_PUB_S + 30) * 1000u, "«пора на зарядку» через 30 с");
}

// Разряд 3,90 → 3,45 В за 6 часов (на деле рация живёт ~15–20 ч): оценка идёт следом — отстаёт на доли процента,
// а шум АЦП ±10 мВ делает её оптимистичнее на ~10 мВ (≈1,5 %); тревога — не раньше настоящих 15 %
static void test_bat_discharge_tracks() {
  BatteryEstimator e(15);
  int worst = 0;
  bool low_early = false;
  const int N = 6 * 3600;
  for (int s = 0; s < N; s++) {
    float mv = 3900 - 450.0f * s / N;
    float jitter = (float)(((s * 7919) % 21) - 10);  // шум АЦП ±10 мВ
    e.add(mv + jitter, (uint32_t)s * 1000u, false);
    if (s > 60) {
      int d = (int)e.pct() - (int)battery_pct_from_mv(mv);
      if (d < 0) d = -d;
      if (d > worst) worst = d;
      if (e.low() && battery_pct_from_mv(mv) > 16) low_early = true;
    }
  }
  CHECK(worst <= 2, "оценка отстаёт от разряда не больше чем на 2 %");
  CHECK(!low_early, "тревога не раньше настоящих 15 %");
  CHECK(e.low(), "к концу — «пора на зарядку»");
}

// Питание от USB без аккумулятора — 255 и не «измеряется»; поставили аккумулятор — 5 с «измеряется», потом заряд
static void test_bat_usb() {
  BatteryEstimator e(15);
  BatRun r = bat_run(e, 0, 30, [](int, float& mv, bool& busy) { mv = 40; busy = false; });
  CHECK(r.first_pub_s < 0 && !r.low_seen, "от USB — заряд не сообщается");
  CHECK(!e.measuring(), "и это не «измеряется», а «нет аккумулятора»");
  BatRun r2 = bat_run(e, 30, 40, [](int, float& mv, bool& busy) { mv = 3933; busy = false; });
  CHECK(r2.first_pub_s == 34 && r2.min_pct == 77, "аккумулятор появился — через 5 замеров 77 %");
}

// Счётчик миллисекунд переполняется раз в 49 суток — прогрев и «30 с низко» считаются верно
static void test_bat_millis_wrap() {
  BatteryEstimator e(15);
  uint32_t t0 = 0xFFFFFFFFu - 5000u;                  // переполнение через 5 с после включения
  BatRun r = bat_run(e, 0, 80, [](int, float& mv, bool& busy) { mv = 3400; busy = false; }, t0);
  CHECK(r.first_pub_s == FIRST_PUB_S, "прогрев 15 с через переполнение");
  CHECK(r.low_seen && (uint32_t)(r.first_low_ms - t0) == (uint32_t)(FIRST_PUB_S + 30) * 1000u,
        "«30 с низко» через переполнение");
}


// ── Защита от свиста между рациями (howl) ──
// Полосовой фильтр — «динамик + комната + микрофон» усиливают сильнее всего на одной частоте
struct Band {
  float b0, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  Band(float f, float q) {
    float w = 6.2831853f * f / 16000.0f, al = sinf(w) / (2 * q), a0 = 1 + al;
    b0 = al / a0; b2 = -al / a0; a1 = -2 * cosf(w) / a0; a2 = (1 - al) / a0;
  }
  float step(float x) {
    float o = b0 * x + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x; y2 = y1; y1 = o;
    return o;
  }
};
// «Речь»: гласные 300 мс (основной тон 110–170 Гц гуляет, 12 гармоник с формантами) и паузы 200 мс, плюс шум
struct Voice {
  double ph[13] = {};
  uint32_t n = 0, rnd = 12345;
  float next() {
    int ms = (int)(n / 16 % 500);
    float f0 = 140.0f + 30.0f * sinf(n / 16000.0f * 3.1f) + 8.0f * sinf(n / 16000.0f * 17.0f);
    float v = 0;
    if (ms < 300) {
      float env = ms < 30 ? ms / 30.0f : (ms > 260 ? (300 - ms) / 40.0f : 1.0f);
      for (int h = 1; h <= 12; h++) {
        ph[h] += 6.283185307 * f0 * h / 16000.0;
        float f = f0 * h, amp = 300.0f / h * (1.0f + 2.0f * expf(-powf((f - 700) / 250, 2)) + 1.2f * expf(-powf((f - 1200) / 300, 2)));
        v += amp * env * sinf((float)ph[h]);
      }
    }
    rnd = rnd * 1103515245u + 12345u;
    n++;
    return v + ((int)(rnd >> 16 & 0xFF) - 128) * 0.05f;
  }
};
// Петля: микрофон A = речь + k·комната(передача A с задержкой). Возвращает уровень передачи (дБ) за последние 0,5 с
static float howl_loop(bool guard, float loop_db, float delay_s, float secs, float res_hz, int* found_at_frame,
                       HowlGuard* g_out = nullptr) {
  HowlGuard g;
  Band room(res_hz, 6.0f);
  Voice voice;
  const int D = (int)(delay_s * 16000);
  static float line[16000 * 2];
  for (int i = 0; i < 16000 * 2; i++) line[i] = 0;
  int wpos = 0;
  float k = powf(10.0f, loop_db / 20.0f), tail2 = 0;
  int frames = (int)(secs * 50), tail_n = 0;
  *found_at_frame = -1;
  g.burst_start(0);
  for (int f = 0; f < frames; f++) {
    float y[HowlGuard::N];
    for (int i = 0; i < HowlGuard::N; i++) {
      int rpos = (wpos + i - D + 32000) % 32000;
      y[i] = voice.next() + k * room.step(line[rpos]);
    }
    if (guard && g.process(y, (uint32_t)f * 20) && *found_at_frame < 0) *found_at_frame = f;
    for (int i = 0; i < HowlGuard::N; i++) {
      float v = fminf(fmaxf(y[i], -32768.0f), 32767.0f);
      line[(wpos + i) % 32000] = v;
      if (f >= frames - 25) { tail2 += v * v; tail_n++; }
    }
    wpos = (wpos + HowlGuard::N) % 32000;
  }
  if (g_out) *g_out = g;
  return 10 * log10f(tail2 / tail_n + 1e-9f);
}

static void test_howl_loop_suppressed() {
  int at;
  float free_db = howl_loop(false, 4.0f, 0.22f, 5.0f, 1800.0f, &at);
  HowlGuard g;
  float guarded_db = howl_loop(true, 4.0f, 0.22f, 5.0f, 1800.0f, &at, &g);
  float speech_db = howl_loop(false, -60.0f, 0.22f, 5.0f, 1800.0f, &at);   // петли нет — просто речь
  CHECK(free_db > speech_db + 20, "без защиты петля свистит (+20 дБ к речи и больше)");
  howl_loop(true, 4.0f, 0.22f, 5.0f, 1800.0f, &at);
  CHECK(at > 0 && at < 150, "свист найден в первые 3 с");
  CHECK(guarded_db < speech_db + 3, "с защитой уровень — как у одной речи");
  CHECK(g.notches() >= 1 && fabsf(g.notch_hz(0) - 1800) < 180, "вырез — у частоты свиста");
  CHECK(g.cut_db() <= HowlGuard::MAX_CUT_DB, "микрофон убавлен не больше предела");
  CHECK(g.hold_agc(), "после свиста АРУ не прибавляет");
}

// Свист на другой частоте и петля посильнее
static void test_howl_other_freq() {
  int at;
  HowlGuard g;
  float speech_db = howl_loop(false, -60.0f, 0.15f, 5.0f, 2600.0f, &at);
  float guarded_db = howl_loop(true, 8.0f, 0.15f, 5.0f, 2600.0f, &at, &g);
  bool near = false;
  for (int i = 0; i < g.notches(); i++) near = near || fabsf(g.notch_hz(i) - 2600) < 180;
  CHECK(guarded_db < speech_db + 3 && near, "2600 Гц, петля +8 дБ — погашена (вырез у резонанса: свист бывает на ±150 Гц)");
}

// Сетка петель: 3 частоты × сила +4…+10 дБ × задержки 0,15 и 0,22 с — все гасятся до уровня речи
static void test_howl_loop_grid() {
  int at, bad = 0;
  float speech_db = howl_loop(false, -60.0f, 0.2f, 5.0f, 1800.0f, &at);
  for (float hz : {1200.0f, 1800.0f, 2600.0f})
    for (float db : {4.0f, 6.0f, 8.0f, 10.0f})
      for (float d : {0.15f, 0.22f}) {
        float g = howl_loop(true, db, d, 5.0f, hz, &at);
        if (g > speech_db + 3) {
          bad++;
          fprintf(stderr, "  петля %+.0f дБ %.0f Гц %.2f с: %.1f дБ при речи %.1f\n", db, hz, d, g, speech_db);
        }
      }
  CHECK(bad == 0, "все 24 петли погашены");
  // сильные петли (рации вплотную, громкость на максимуме): одних вырезов мало — нужно «тише при повторе»
  for (float hz : {1200.0f, 1800.0f, 2600.0f})
    for (float db : {14.0f, 18.0f}) {
      float g = howl_loop(true, db, 0.22f, 6.0f, hz, &at);
      if (g > speech_db + 3) {
        bad++;
        fprintf(stderr, "  сильная петля %+.0f дБ %.0f Гц: %.1f дБ при речи %.1f\n", db, hz, g, speech_db);
      }
    }
  CHECK(bad == 0, "и 6 сильных петель (+14, +18 дБ) — тоже");
}


// Проверка 30.09: свист прыгает между двумя частотами дальше ±150 Гц (два резонанса почти равной силы) — ловится
static void test_howl_alternating() {
  for (int seg : {3, 5, 7})
    for (float grow : {0.0f, 10.0f}) {
      HowlGuard g;
      g.burst_start(0);
      int found = 0;
      double ph = 0;
      for (int f = 0; f < 250; f++) {
        float y[HowlGuard::N];
        float hz = (f / seg) % 2 ? 3290.0f : 2450.0f, a = fminf(100.0f * powf(10.0f, grow * f * 0.02f / 20), 2000.0f);
        for (int i = 0; i < HowlGuard::N; i++) {
          ph += 6.283185307 * hz / 16000;
          y[i] = f < 15 ? 0 : a * (float)sin(ph);
        }
        if (g.process(y, (uint32_t)f * 20)) found++;
      }
      CHECK(found >= 1, "попеременные 2450/3290 Гц — найдены");
    }
}

// Петля с двумя резонансами почти равной силы (930 и 1140 Гц) — гасится
static void test_howl_two_resonances() {
  int bad = 0;
  for (float d : {0.18f, 0.25f})
    for (float db : {4.0f, 8.0f}) {
      HowlGuard g;
      Band r1(930.0f, 7.0f), r2(1140.0f, 7.0f);
      Voice voice;
      const int D = (int)(d * 16000);
      static float line[32000];
      for (float& v : line) v = 0;
      int wpos = 0;
      float k = powf(10.0f, db / 20.0f), tail = 0, sp = 0;
      int tn = 0;
      g.burst_start(0);
      for (int f = 0; f < 300; f++) {
        float y[HowlGuard::N];
        for (int i = 0; i < HowlGuard::N; i++) {
          float back = line[(wpos + i - D + 32000) % 32000];
          float v = voice.next();
          y[i] = v + k * (r1.step(back) + 0.95f * r2.step(back));
          if (f >= 275) sp += v * v;
        }
        g.process(y, (uint32_t)f * 20);
        for (int i = 0; i < HowlGuard::N; i++) {
          float v = fminf(fmaxf(y[i], -32768.0f), 32767.0f);
          line[(wpos + i) % 32000] = v;
          if (f >= 275) { tail += v * v; tn++; }
        }
        wpos = (wpos + HowlGuard::N) % 32000;
      }
      if (10 * log10f(tail / tn + 1e-9f) > 10 * log10f(sp / tn + 1e-9f) + 3) bad++;
    }
  CHECK(bad == 0, "два резонанса 930/1140 Гц — погашены");
}

// Короткие писки и ноты 0,2–0,45 с с атакой 0/20/50 мс, начало в любом месте кадра — не свист
static void test_howl_short_tones() {
  int found = 0, total = 0;
  for (float hz : {1000.0f, 2500.0f, 3700.0f})
    for (float len : {0.2f, 0.3f, 0.45f})
      for (float att : {0.0f, 0.02f, 0.05f})
        for (int off = 0; off < 320; off += 40) {
          HowlGuard g;
          g.burst_start(0);
          uint32_t r = 7;
          for (int f = 0; f < 60; f++) {
            float y[HowlGuard::N];
            for (int i = 0; i < HowlGuard::N; i++) {
              float t = (f * HowlGuard::N + i - 16 * 320 - off) / 16000.0f;   // начало — на 16-м кадре + сдвиг
              float env = t < 0 || t > len ? 0 : (att > 0 && t < att ? t / att : 1.0f);
              r = r * 1103515245u + 12345u;
              y[i] = 200 * env * sinf(6.2831853f * hz * t) + ((int)(r >> 16 & 0xFF) - 128) / 64.0f;
            }
            if (g.process(y, (uint32_t)f * 20)) found++;
          }
          total++;
        }
  if (found) fprintf(stderr, "  короткие тоны: срабатываний %d из %d\n", found, total);
  CHECK(found == 0, "писки, ноты и звонки до 0,45 с — не свист");
}

// Насвистанная мелодия 8 с (ноты по 0,3 с, 1175–2349 Гц) — микрофон не приглушается (АРУ может придержаться:
// ноты, что соседствуют ближе 150 Гц, изредка похожи на растущий тон — это только «не прибавлять»)
static void test_howl_melody() {
  static const float notes[] = {1175, 1319, 1480, 1568, 1760, 1976, 2093, 2349, 2093, 1760, 1568, 1319, 1175,
                                1397, 1568, 1760, 2093, 2349, 1976, 1568, 1397, 1175, 1319, 1480, 1760};
  HowlGuard g;
  g.burst_start(0);
  uint32_t r = 7;
  double ph = 0;
  bool held = false;
  for (int f = 0; f < 450; f++) {
    float y[HowlGuard::N];
    for (int i = 0; i < HowlGuard::N; i++) {
      float t = (f * HowlGuard::N + i) / 16000.0f - 0.5f, v = 0;
      if (t >= 0 && t < 8.0f) {
        int k = (int)(t / 0.32f);
        float tn = t - k * 0.32f;
        if (k < 25 && tn < 0.3f) {
          float env = tn < 0.05f ? tn / 0.05f : (tn > 0.28f ? (0.3f - tn) / 0.02f : 1);
          ph += 6.283185307 * notes[k] / 16000;
          v = 150 * env * (float)sin(ph);
        }
      }
      r = r * 1103515245u + 12345u;
      y[i] = v + ((int)(r >> 16 & 0xFF) - 128) / 128.0f * 3;
    }
    g.process(y, (uint32_t)f * 20);
    held = held || g.cut_db() > 0;
  }
  CHECK(!held, "мелодия не приглушает микрофон");
  CHECK(g.notches() <= 2, "и оставляет не больше двух вырезов");
}


// Полная цепочка, как в mic_task (audio.cpp): срез низа → защита → АРУ (порог 20, цель 3000, 0…36 дБ, вниз 30 %/кадр,
// вверх 0,25 дБ/кадр, если не hold_agc) → ограничитель 29000. Проверка 30.09: без АРУ в модели регресс не был виден —
// АРУ держит повторный свист ровным, и правило «растёт» его не видит. Возвращает уровень последней секунды (дБ).
struct Mode { float f, q, db; };
static float chain_burst(HowlGuard& g, const Mode* modes, int nm, float coup_db, float delay_s, uint32_t t0, bool guard) {
  Band* bands[8];
  for (int k = 0; k < nm; k++) bands[k] = new Band(modes[k].f, modes[k].q);
  Voice voice;
  static float line[32000];
  for (float& v : line) v = 0;
  const int D = (int)(delay_s * 16000);
  int wpos = 0;
  float c = powf(10.0f, coup_db / 20.0f), gain_db = 20.0f, prev_g = powf(10.0f, 2.0f), hp_x = 0, hp_y = 0;
  double tail = 0;
  int tn = 0;
  uint32_t nz = 99 + t0;
  if (guard) g.burst_start(t0);
  for (int f = 0; f < 250; f++) {                                 // 5 с, голос замолкает на 2-й секунде
    float y[HowlGuard::N];
    for (int i = 0; i < HowlGuard::N; i++) {
      float back = line[(wpos + i - D + 32000) % 32000], room = 0;
      for (int k = 0; k < nm; k++) room += powf(10.0f, modes[k].db / 20.0f) * bands[k]->step(back);
      float v = voice.next() * 0.3f;
      nz = nz * 1664525u + 1013904223u;                            // тихий шум комнаты (±1)
      float x = (f < 100 ? v : 0) + c * room + ((int)(nz >> 16 & 0xFFFF) - 32768) / 32768.0f;
      hp_y = 0.954f * (hp_y + x - hp_x);
      hp_x = x;
      y[i] = hp_y;
    }
    if (guard) g.process(y, t0 + (uint32_t)f * 20);
    float peak = 0, s2 = 0;
    for (int i = 0; i < HowlGuard::N; i++) {
      peak = fmaxf(peak, fabsf(y[i]));
      s2 += y[i] * y[i];
    }
    float rms = sqrtf(s2 / HowlGuard::N);
    if (rms > 20) {
      float want = fminf(fmaxf(20 * log10f(3000 / rms), 0.0f), 36.0f);
      if (want < gain_db) gain_db += (want - gain_db) * 0.3f;
      else if (!(guard && g.hold_agc())) gain_db += fminf(0.25f, want - gain_db);
    }
    float G = powf(10.0f, gain_db / 20.0f);
    if (peak * G > 29000) G = 29000 / peak;
    for (int i = 0; i < HowlGuard::N; i++) {
      float v = fminf(fmaxf(y[i] * (prev_g + (G - prev_g) * i / HowlGuard::N), -32768.0f), 32767.0f);
      line[(wpos + i) % 32000] = v;
      if (f >= 200) { tail += v * v; tn++; }
    }
    prev_g = G;
    wpos = (wpos + HowlGuard::N) % 32000;
  }
  for (int k = 0; k < nm; k++) delete bands[k];
  return 10 * log10f((float)(tail / tn) + 1e-9f);
}

static void test_howl_chain_agc() {
  // один резонанс, первая передача: сетка 1200–2600 Гц × петля +3…+15 дБ × задержка 0,175–0,3 с
  int fresh = 0, fresh_howl = 0, fresh_free = 0;
  for (float f0 : {1200.0f, 1500.0f, 1800.0f, 2100.0f, 2600.0f})
    for (float L : {3.0f, 7.0f, 11.0f, 13.0f, 15.0f})
      for (float d : {0.175f, 0.225f, 0.25f, 0.275f}) {
        Mode m[1] = {{f0, 6, 0}};
        HowlGuard g0, g1;
        fresh_free += chain_burst(g0, m, 1, L - 20, d, 1000, false) > 50;
        fresh_howl += chain_burst(g1, m, 1, L - 20, d, 1000, true) > 50;
        fresh++;
      }
  fprintf(stderr, "  цепочка с АРУ, первая передача: свистят без защиты %d из %d, с защитой %d\n", fresh_free, fresh, fresh_howl);
  CHECK(fresh_free >= fresh * 9 / 10 && fresh_howl <= fresh / 25, "цепочка с АРУ: с защитой свистит не больше 4 %");
  // шесть резонансов, 8 передач подряд (вырезы переходят из передачи в передачу)
  int howl0 = 0, howl1 = 0, n = 0;
  for (unsigned seed = 1; seed <= 4; seed++) {
    uint32_t r = seed * 2654435761u;
    Mode m[6];
    for (int k = 0; k < 6; k++) {
      r = r * 1103515245u + 12345u;
      m[k] = {800.0f + (r >> 8) % 2600, 5.0f + (r >> 20) % 6, k ? -(float)((r >> 4) % 7) : 0.0f};
    }
    HowlGuard g0, g1;
    for (int b = 0; b < 8; b++) {
      howl0 += chain_burst(g0, m, 6, -12, 0.2f, 1000 + b * 10000, false) > 50;
      howl1 += chain_burst(g1, m, 6, -12, 0.2f, 1000 + b * 10000, true) > 50;
      n++;
    }
  }
  fprintf(stderr, "  шесть резонансов: свистят без защиты %d из %d, с защитой %d\n", howl0, n, howl1);
  CHECK(howl0 >= n * 3 / 4 && howl1 <= n / 4, "шесть резонансов, 8 передач: с защитой свистит не больше четверти");
}

// Вырез от ровного тона живёт минуту, от настоящего свиста — полчаса
static void test_howl_notch_lifetime() {
  HowlGuard g;
  g.burst_start(0);
  for (int f = 0; f < 100; f++) {
    float y[HowlGuard::N];
    for (int i = 0; i < HowlGuard::N; i++) {
      float t = (f * HowlGuard::N + i) / 16000.0f;
      y[i] = t > 0.4f ? 600.0f * sinf(6.2831853f * 1500 * t) : 0;
    }
    g.process(y, (uint32_t)f * 20);
  }
  CHECK(g.notches() == 1, "ровный тон — вырезан");
  g.burst_start(2000 + 30000);
  CHECK(g.notches() == 1, "через 30 с вырез от ровного тона ещё стоит");
  g.burst_start(2000 + 61000);
  CHECK(g.notches() == 0, "через минуту снят");
}

// Одна речь 60 с — ни одного выреза
static void test_howl_speech_clean() {
  HowlGuard g;
  Voice v;
  g.burst_start(0);
  int found = 0;
  for (int f = 0; f < 3000; f++) {
    float y[HowlGuard::N];
    for (int i = 0; i < HowlGuard::N; i++) y[i] = v.next();
    if (g.process(y, (uint32_t)f * 20)) found++;
  }
  CHECK(found == 0 && g.notches() == 0, "речь не принимается за свист");
}

// Свой «пи-пи» 1800 Гц в начале передачи — не свист. Ровный посторонний тон (писк прибора) первые 0,5 с не трогаем,
// дальше вырезаем (от свиста, который АРУ держит на одном уровне, его не отличить), но громкость речи не трогаем
static void test_howl_beep_and_steady_tone() {
  HowlGuard g;
  g.burst_start(0);
  int found = 0;
  for (int f = 0; f < 200; f++) {
    float y[HowlGuard::N];
    for (int i = 0; i < HowlGuard::N; i++) {
      int n = f * HowlGuard::N + i;
      float t = n / 16000.0f;
      y[i] = (t < 0.28f ? 3000.0f * t / 0.28f * sinf(6.2831853f * 1800 * t) : 0)   // «пи-пи», нарастает
             + (t > 0.5f ? 800.0f * sinf(6.2831853f * 1234 * t) : 0);            // ровный писк прибора
    }
    if (g.process(y, (uint32_t)f * 20)) found++;
  }
  CHECK(found == 1 && g.notches() == 1 && fabsf(g.notch_hz(0) - 1234) < 40, "свой сигнал не вырезан, ровный тон — вырезан один раз");
  CHECK(!g.hold_agc() && g.cut_db() == 0, "ровный тон не держит АРУ и не приглушает микрофон");
  // вырезы держатся минуту и снимаются после
  int at;
  HowlGuard h;
  howl_loop(true, 4.0f, 0.22f, 5.0f, 1800.0f, &at, &h);
  h.burst_start(5000 + 10u * 60u * 1000u);
  CHECK(h.notches() >= 1 && !h.hold_agc(), "через 10 мин вырез ещё стоит (рации так и лежат рядом), АРУ снова свободна");
  h.burst_start(at * 20 + HowlGuard::KEEP_MS + 1000);
  CHECK(h.notches() == 0, "через полчаса без свиста вырезы сняты");
}

// Проигрывает поток: каждые 20 мс pop; возвращает последовательность результатов/номеров
static void test_jitter_in_order() {
  JitterBuffer jb;
  jb.begin(1, 42, 0);
  uint8_t out[wt::AUDIO_ENC_LEN], lvl;
  uint32_t t = 0;
  int played = 0;
  for (int k = 0; k < 40; k++, t += 20) {
    if (k < 30) push(jb, (uint16_t)(1000 + k), t);
    if (k == 30) jb.end(1, 42);
    auto r = jb.pop(out, &lvl, t);
    if (r == JitterBuffer::FRAME) {
      CHECK(out[0] == frame_for((uint16_t)(1000 + played)), "порядок кадров");
      played++;
    }
    if (r == JitterBuffer::ENDED) break;
  }
  CHECK(played == 30, "все 30 кадров сыграны");
  CHECK(!jb.active(), "поток закончен");
}

static void test_jitter_reorder_and_loss() {
  JitterBuffer jb;
  jb.begin(1, 42, 0);
  uint8_t out[wt::AUDIO_ENC_LEN], lvl;
  // пришли вперемешку: 1, 0, 3, 2, (4 потерян), 5, 6, 7
  uint16_t order[] = {1, 0, 3, 2, 5, 6, 7};
  uint32_t t = 0;
  for (uint16_t s : order) {
    push(jb, s, t);
    t += 5;
  }
  jb.end(1, 42);
  int seq_played[16], n = 0, missing = 0;
  for (int k = 0; k < 20; k++, t += 20) {
    auto r = jb.pop(out, &lvl, t);
    if (r == JitterBuffer::FRAME) {
      for (uint16_t s = 0; s < 8; s++)
        if (out[0] == frame_for(s)) seq_played[n++] = s;
    } else if (r == JitterBuffer::MISSING) {
      missing++;
    } else if (r == JitterBuffer::ENDED) {
      break;
    }
  }
  CHECK(n == 7, "7 кадров сыграно");
  int expect[] = {0, 1, 2, 3, 5, 6, 7};
  for (int i = 0; i < n && i < 7; i++) CHECK(seq_played[i] == expect[i], "порядок после перестановки");
  CHECK(missing == 1, "ровно одна заплатка вместо потерянного 4");
  CHECK(jb.stat_lost == 1, "учёт потерь");
}

static void test_jitter_underrun_grows_prefill() {
  JitterBuffer jb;
  uint8_t out[wt::AUDIO_ENC_LEN], lvl;
  uint32_t t = 0;
  jb.begin(1, 42, t);
  // 3 кадра, затем пауза в сети 120 мс, затем ещё 3
  for (uint16_t s = 0; s < 3; s++) push(jb, s, t);
  int underrun = 0;
  for (int k = 0; k < 12; k++, t += 20) {
    if (k == 8)
      for (uint16_t s = 3; s < 6; s++) push(jb, s, t);
    auto r = jb.pop(out, &lvl, t);
    if (r == JitterBuffer::MISSING) underrun++;
  }
  CHECK(underrun >= 3, "провал отыгран заплатками");
  jb.end(1, 42);
  for (int k = 0; k < 10; k++, t += 20) jb.pop(out, &lvl, t);
  CHECK(!jb.active(), "закончился");
  jb.begin(1, 43, t);
  CHECK(jb.prefill() > JitterBuffer::PREFILL_MIN, "следующая передача с запасом побольше");
}

static void test_jitter_silence_end_and_foreign() {
  JitterBuffer jb;
  uint8_t out[wt::AUDIO_ENC_LEN], lvl, f[wt::AUDIO_ENC_LEN] = {0};
  jb.begin(1, 42, 0);
  CHECK(!jb.push(2, 42, 0, f, 0, 0), "чужой говорящий не принимается");
  CHECK(!jb.push(1, 41, 0, f, 0, 0), "чужая передача не принимается");
  push(jb, 0, 0);
  JitterBuffer::Pop r = JitterBuffer::WAIT;
  uint32_t t = 0;
  for (; t < 2000 && r != JitterBuffer::ENDED; t += 20) r = jb.pop(out, &lvl, t);
  CHECK(r == JitterBuffer::ENDED, "тишина 600 мс = конец");
  CHECK(t < 1000, "конец не позже чем через ~0,7 с");
}

static void test_jitter_wraparound() {
  JitterBuffer jb;
  jb.begin(1, 42, 0);
  uint8_t out[wt::AUDIO_ENC_LEN], lvl;
  uint32_t t = 0;
  int played = 0;
  for (int k = 0; k < 20; k++, t += 20) {
    push(jb, (uint16_t)(65530 + k), t);   // переход 65535 → 0
    if (jb.pop(out, &lvl, t) == JitterBuffer::FRAME) played++;
  }
  CHECK(played >= 16, "номера через ноль играются");
}

int main() {
  adpcm_dump();
  test_jitter_in_order();
  test_jitter_reorder_and_loss();
  test_jitter_underrun_grows_prefill();
  test_jitter_silence_end_and_foreign();
  test_jitter_wraparound();
  test_bat_curve();
  test_bat_boot_sag();
  test_bat_boot_long_sag();
  test_bat_boot_then_busy();
  test_bat_busy_ignored();
  test_bat_long_conversation();
  test_bat_stuck_busy();
  test_bat_short_dips();
  test_bat_step_down();
  test_bat_low_hold_resets();
  test_bat_real_low();
  test_bat_discharge_tracks();
  test_bat_usb();
  test_bat_millis_wrap();
  test_howl_loop_suppressed();
  test_howl_other_freq();
  test_howl_loop_grid();
  test_howl_speech_clean();
  test_howl_alternating();
  test_howl_two_resonances();
  test_howl_short_tones();
  test_howl_melody();
  test_howl_chain_agc();
  test_howl_notch_lifetime();
  test_howl_beep_and_steady_tone();
  printf("DONE fails=%d\n", fails);
  return fails ? 1 : 0;
}
