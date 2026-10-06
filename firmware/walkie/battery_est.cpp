#include "battery_est.h"

// Li-ion при токе ~0,1–0,2 А: напряжение → проценты (грубая кривая, та же, что в прошивках 1–4)
uint8_t battery_pct_from_mv(float mv) {
  static const float V[] = {3300, 3450, 3550, 3620, 3680, 3740, 3800, 3870, 3950, 4040, 4150};
  if (mv <= V[0]) return 0;
  for (int i = 1; i < 11; i++)
    if (mv < V[i]) return (uint8_t)((i - 1) * 10 + 10 * (mv - V[i - 1]) / (V[i] - V[i - 1]));
  return 100;
}

void BatteryEstimator::add(float raw_mv, uint32_t now_ms, bool busy) {
  raw_ = raw_mv;
  if (!started_) {
    started_ = true;
    first_ms_ = now_ms;
  }
  if (!warm_) {                      // первые 15 с после включения — не в счёт
    if (now_ms - first_ms_ < WARMUP_MS) return;
    warm_ = true;
  }
  bool skip = busy || (busy_seen_ && now_ms - busy_ms_ < QUIET_MS);
  if (busy) {
    if (!busy_run_) {
      busy_run_ = true;
      busy_start_ = now_ms;
    }
    busy_seen_ = true;
    busy_ms_ = now_ms;
  } else {
    busy_run_ = false;
  }
  if (skip && !(busy && now_ms - busy_start_ >= MAX_BUSY_MS)) return;   // занята 5 мин без перерыва — мерить
  if (raw_mv < NO_BATTERY_MV) {      // делителя нет — питание от USB
    usb_ = true;
    mv_ = 0;
    win_n_ = win_pos_ = 0;
    low_run_ = low_ = false;
    return;
  }
  usb_ = false;
  win_[win_pos_] = raw_mv;
  win_pos_ = (win_pos_ + 1) % WINDOW;
  if (win_n_ < WINDOW) win_n_++;
  float top = 0;
  for (int i = 0; i < win_n_; i++)
    if (win_[i] > top) top = win_[i];
  if (mv_ == 0 || top > mv_)
    mv_ = top;                       // вверх — сразу: наибольший из окна уже без провалов
  else
    mv_ += (top - mv_) * SMOOTH;
  if (win_n_ < MIN_SAMPLES) return;  // ещё «измеряется» — «низкий заряд» не копим
  uint8_t p = battery_pct_from_mv(mv_);
  if (p <= low_pct_) {
    if (!low_run_) {
      low_run_ = true;
      low_since_ = now_ms;
    }
    if (now_ms - low_since_ >= LOW_HOLD_MS) low_ = true;
  } else {
    low_run_ = false;                // «30 с подряд» — заново
    if (p >= low_pct_ + 3) low_ = false;   // запас 3 %: на границе не мигать «низко / нормально»
  }
}

uint8_t BatteryEstimator::pct() const {
  if (usb_ || win_n_ < MIN_SAMPLES) return 255;
  return battery_pct_from_mv(mv_);
}
