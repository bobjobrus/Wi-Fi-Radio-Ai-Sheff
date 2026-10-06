#include "howl.h"
#include <math.h>

HowlGuard::HowlGuard() {
  for (int i = 0; i < N; i++) win_[i] = 0.5f - 0.5f * cosf(6.2831853f * i / (N - 1));   // окно Ханна
  for (int k = 0; k <= K_HI + 1; k++) cosk_[k] = 2.0f * cosf(6.2831853f * k / N);      // Гёрцель: частота k·50 Гц
  for (int i = 0; i < SERIES; i++) ser_[i].on = false;
}

void HowlGuard::burst_start(uint32_t now_ms) {
  for (int i = 0; i < n_;) {                               // старые вырезы снять: от свиста — через 30 мин, от ровного тона — через минуту
    if (now_ms - t_[i] > (grown_[i] ? KEEP_MS : KEEP_STEADY_MS)) {
      for (int j = i + 1; j < n_; j++) {
        bq_[j - 1] = bq_[j];
        hz_[j - 1] = hz_[j];
        qz_[j - 1] = qz_[j];
        grown_[j - 1] = grown_[j];
        t_[j - 1] = t_[j];
      }
      n_--;
    } else {
      i++;
    }
  }
  for (int i = 0; i < n_; i++) bq_[i].x1 = bq_[i].x2 = bq_[i].y1 = bq_[i].y2 = 0;
  for (int i = 0; i < SERIES; i++) ser_[i].on = false;
  frame_ = 0;
  hold_ = false;
  grew_in_burst_ = false;
  cut_db_ = 0;
  atten_ = 1.0f;
}

void HowlGuard::add_notch(float hz, float qv, bool grown, uint32_t now_ms) {
  int i;
  if (n_ < MAX_NOTCH) {
    i = n_++;
  } else {                                                 // мест нет — самый старый вырез уходит
    for (int j = 1; j < MAX_NOTCH; j++) {
      bq_[j - 1] = bq_[j];
      hz_[j - 1] = hz_[j];
      qz_[j - 1] = qz_[j];
      grown_[j - 1] = grown_[j];
      t_[j - 1] = t_[j];
    }
    i = MAX_NOTCH - 1;
  }
  float w = 6.2831853f * hz / 16000.0f, al = sinf(w) / (2.0f * qv), c = cosf(w), a0 = 1.0f + al;
  Biquad& q = bq_[i];
  q.b0 = 1.0f / a0;
  q.b1 = -2.0f * c / a0;
  q.b2 = 1.0f / a0;
  q.a1 = -2.0f * c / a0;
  q.a2 = (1.0f - al) / a0;
  q.x1 = q.x2 = q.y1 = q.y2 = 0;
  hz_[i] = hz;
  qz_[i] = qv;
  grown_[i] = grown;
  t_[i] = now_ms;
}

void HowlGuard::detected(Series& s, bool grew, uint32_t now_ms) {
  float hz = s.wk / (s.w + 1e-9f) * 50.0f;                      // средняя частота серии
  int same = -1;                                                // рядом уже стоит вырез — петля обходит его
  for (int i = 0; i < n_; i++)
    if (fabsf(hz - hz_[i]) <= WANDER * 50.0f) same = i;
  if (!grew && grew_in_burst_) grew = true;
  if (same >= 0) hold_ = true;                                  // петля вернулась к своему вырезу — АРУ не прибавлять
  float qv = NOTCH_Q;
  bool conf = grew || same >= 0;                                // вырез от настоящего свиста — держать долго
  if (same >= 0) {
    conf = conf || grown_[same];
    qv = fmaxf(qz_[same] * 0.5f, 2.0f);
    for (int j = same + 1; j < n_; j++) {
      bq_[j - 1] = bq_[j];
      hz_[j - 1] = hz_[j];
      qz_[j - 1] = qz_[j];
      grown_[j - 1] = grown_[j];
      t_[j - 1] = t_[j];
    }
    n_--;
  }
  add_notch(hz, qv, conf, now_ms);
  if (grew) {
    if (grew_in_burst_ && cut_db_ < MAX_CUT_DB) {                // снова растёт — петля сильная: тише весь микрофон
      cut_db_ = fminf(cut_db_ + STEP_DB, MAX_CUT_DB);
      atten_ = powf(10.0f, -cut_db_ / 20.0f);
    }
    grew_in_burst_ = true;
    hold_ = true;
  }
  s.on = false;
}

bool HowlGuard::process(float* y, uint32_t now_ms) {
  bool found = false;
  // сначала вырезы, потом поиск: ищем по тому, что уйдёт в эфир. Посторонний ровный писк, вырезанный один раз,
  // больше не находится; свист, который вырез не погасил (петля нашла соседнюю частоту), — находится
  for (int j = 0; j < n_; j++) {
    Biquad& q = bq_[j];
    for (int i = 0; i < N; i++) {
      float x = y[i];
      float o = q.b0 * x + q.b1 * q.x1 + q.b2 * q.x2 - q.a1 * q.y1 - q.a2 * q.y2;
      q.x2 = q.x1;
      q.x1 = x;
      q.y2 = q.y1;
      q.y1 = o;
      y[i] = o;
    }
  }
  if (frame_ >= SKIP) {
    float e[K_HI + 2];
    float total = 0;
    for (int k = K_ALL; k <= K_HI + 1; k++) {
      float s1 = 0, s2 = 0, c = cosk_[k];
      for (int i = 0; i < N; i++) {
        float s = y[i] * win_[i] + c * s1 - s2;
        s2 = s1;
        s1 = s;
      }
      e[k] = s1 * s1 + s2 * s2 - c * s1 * s2;
      if (k <= K_HI) total += e[k];
    }
    // тоны кадра: местные пики с долей энергии > SHARE, два сильнейших
    int ck[2] = {-1, -1};
    for (int k = K_LO; k <= K_HI; k++) {
      if (!(e[k] >= e[k - 1] && e[k] > e[k + 1])) continue;
      float share = (e[k - 1] + e[k] + e[k + 1]) / (total + 1e-9f);
      float amp = sqrtf(fmaxf(e[k], 0.0f)) * 2.0f / (N * 0.5f);   // сумма окна Ханна ≈ N/2
      if (share <= SHARE || amp <= AMIN) continue;
      if (ck[0] < 0 || e[k] > e[ck[0]]) {
        ck[1] = ck[0];
        ck[0] = k;
      } else if (ck[1] < 0 || e[k] > e[ck[1]]) {
        ck[1] = k;
      }
    }
    bool used[SERIES] = {};
    int ncand = (ck[0] >= 0) + (ck[1] >= 0);
    for (int c = 0; c < ncand; c++) {
      int k = ck[c];
      float amp = sqrtf(fmaxf(e[k], 0.0f)) * 2.0f / (N * 0.5f);
      // частота пика точнее, чем шаг 50 Гц: парабола по логарифмам соседних полос
      float l0 = logf(e[k - 1] + 1e-9f), l1 = logf(e[k] + 1e-9f), l2 = logf(e[k + 1] + 1e-9f);
      float den = l0 - 2.0f * l1 + l2, d = den < -1e-6f ? 0.5f * (l0 - l2) / den : 0.0f;
      if (d > 0.5f) d = 0.5f;
      if (d < -0.5f) d = -0.5f;
      int best = -1, bd = WANDER + 1;
      for (int i = 0; i < SERIES; i++) {
        int dist = k - ser_[i].ref_k;
        if (dist < 0) dist = -dist;
        if (ser_[i].on && !used[i] && dist < bd) {
          bd = dist;
          best = i;
        }
      }
      if (best < 0) {                                             // новый тон — новая серия
        for (int i = 0; i < SERIES && best < 0; i++)
          if (!ser_[i].on) best = i;
        if (best < 0) {                                           // мест нет — вместо самой короткой
          for (int i = 0; i < SERIES; i++)
            if (!used[i] && (best < 0 || ser_[i].run < ser_[best].run)) best = i;
        }
        if (best < 0) continue;
        Series& s = ser_[best];
        s.on = true;
        s.ref_k = k;
        s.at_notch = false;
        for (int i = 0; i < n_; i++)                              // рядом с вырезом — отсчёт от него
          if (fabsf(k * 50.0f - hz_[i]) <= WANDER * 50.0f) { s.ref_k = (int)lroundf(hz_[i] / 50.0f); s.at_notch = true; }
        s.run = 0;
        s.strong = 0;
        s.kmin = 999;
        s.kmax = -1;
        s.a0 = 0;
        s.w = s.wk = 0;
      }
      Series& s = ser_[best];
      used[best] = true;
      s.amp[s.run % 8] = amp;
      s.run++;
      s.miss = 0;
      s.stale = 0;
      s.w += e[k];
      s.wk += e[k] * (k + d);
      if ((e[k - 1] + e[k] + e[k + 1]) / (total + 1e-9f) > SHARE_LONG) {
        s.strong++;
        if (k < s.kmin) s.kmin = k;
        if (k > s.kmax) s.kmax = k;
      }
      if (s.run > ONSET && s.run <= ONSET + 3) s.a0 += amp / 3;   // опора роста — громкость сразу после атаки
      bool grew = false;                                          // растёт ли ещё: наклон громкости (дБ) по 8 попаданиям
      if (s.run >= ONSET + 8) {                                   // (атака звука — первые ONSET — не в счёт)
        float db[8], mean = 0;
        for (int i = 0; i < 8; i++) {
          db[i] = 20.0f * log10f(s.amp[(s.run - 8 + i) % 8] + 1e-9f);
          mean += db[i] / 8;
        }
        float cov = 0;
        for (int i = 0; i < 8; i++) cov += (i - 3.5f) * (db[i] - mean);
        float last = (s.amp[(s.run - 1) % 8] + s.amp[(s.run - 2) % 8] + s.amp[(s.run - 3) % 8]) / 3;
        grew = cov / 42.0f >= GROW_SLOPE &&                       // Σ(i − 3,5)² = 42
               20.0f * log10f((last + 1e-9f) / (s.a0 + 1e-9f)) >= GROW_NET_DB;
      }
      if (grew || (s.strong >= LONG && (s.kmax - s.kmin <= LONG_SPAN || s.at_notch))) {
        detected(s, grew, now_ms);
        found = true;
      }
    }
    for (int i = 0; i < SERIES; i++) {                            // серии без попадания в этом кадре
      Series& s = ser_[i];
      if (!s.on || used[i]) continue;
      if (ncand == 0) s.miss++;                                   // тонов нет вовсе
      s.stale++;
      if (s.miss > MISS || s.stale > STALE) s.on = false;
    }
  }
  frame_++;
  if (atten_ < 1.0f)
    for (int i = 0; i < N; i++) y[i] *= atten_;
  return found;
}
