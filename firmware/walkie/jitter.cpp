#include "jitter.h"

void JitterBuffer::clear_slots() {
  for (int i = 0; i < SLOTS; i++) slots_[i].used = false;
}

void JitterBuffer::begin(uint32_t talker, uint32_t burst, uint32_t now_ms) {
  if (active_) stop();
  clear_slots();
  active_ = true;
  playing_ = ended_ = have_first_ = false;
  talker_ = talker;
  burst_ = burst;
  next_ = 0;
  first_ms_ = last_rx_ms_ = now_ms;
  prefill_ = target_;
  empty_run_ = 0;
  had_underrun_ = false;
}

void JitterBuffer::stop() {
  if (!active_) return;
  active_ = false;
  // подстройка запаса: были провалы — прибавить, три спокойные передачи подряд — убавить
  if (had_underrun_) {
    if (target_ < PREFILL_MAX) target_++;
    calm_bursts_ = 0;
  } else if (++calm_bursts_ >= 3) {
    calm_bursts_ = 0;
    if (target_ > PREFILL_MIN) target_--;
  }
}

int JitterBuffer::buffered() const {
  int n = 0;
  for (int i = 0; i < SLOTS; i++) {
    if (!slots_[i].used) continue;
    int16_t d = (int16_t)(slots_[i].seq - next_);
    if (d >= 0 && d < SLOTS) n++;
  }
  return n;
}

bool JitterBuffer::any_after_next() const {
  for (int i = 0; i < SLOTS; i++) {
    if (!slots_[i].used) continue;
    int16_t d = (int16_t)(slots_[i].seq - next_);
    if (d > 0 && d < SLOTS) return true;
  }
  return false;
}

bool JitterBuffer::push(uint32_t talker, uint32_t burst, uint16_t seq, const uint8_t* frame,
                        uint8_t level, uint32_t now_ms) {
  if (!matches(talker, burst) || ended_) return false;
  last_rx_ms_ = now_ms;
  if (!have_first_) {
    have_first_ = true;
    first_ms_ = now_ms;
    next_ = seq;
  } else if (!playing_) {
    // до начала воспроизведения можно «отступить» назад: пакеты пришли вперемешку
    int16_t back = (int16_t)(seq - next_);
    if (back < 0 && back > -SLOTS / 2) next_ = seq;
  }
  int16_t d = (int16_t)(seq - next_);
  if (d < 0) {
    stat_late++;
    return false;
  }
  if (d >= SLOTS) {
    // долгий провал: всё старое выбросить и продолжить с этого места
    clear_slots();
    next_ = seq;
  }
  Slot& s = slots_[seq % SLOTS];
  s.used = true;
  s.seq = seq;
  s.level = level;
  memcpy(s.data, frame, wt::AUDIO_ENC_LEN);
  return true;
}

void JitterBuffer::end(uint32_t talker, uint32_t burst) {
  if (matches(talker, burst)) ended_ = true;
}

JitterBuffer::Pop JitterBuffer::pop(uint8_t* out, uint8_t* level, uint32_t now_ms) {
  if (!active_) return IDLE;
  if (!ended_ && now_ms - last_rx_ms_ > SILENCE_END_MS) {
    stop();
    return ENDED;
  }
  if (!playing_) {
    if (!have_first_) {
      if (ended_) {
        stop();
        return ENDED;
      }
      return WAIT;
    }
    int b = buffered();
    if (b >= prefill_ || ended_ || now_ms - first_ms_ >= (uint32_t)prefill_ * wt::FRAME_MS + 80) {
      playing_ = true;
      empty_run_ = 0;
    } else {
      return WAIT;
    }
  }
  // слишком большой запас (отправитель чуть спешит или пришла пачка) — сократить задержку
  if (buffered() > prefill_ + 5) {
    Slot& old = slots_[next_ % SLOTS];
    if (old.used && old.seq == next_) old.used = false;
    next_++;
    stat_dropped++;
  }
  Slot& s = slots_[next_ % SLOTS];
  if (s.used && s.seq == next_) {
    memcpy(out, s.data, wt::AUDIO_ENC_LEN);
    if (level) *level = s.level;
    s.used = false;
    next_++;
    empty_run_ = 0;
    stat_played++;
    return FRAME;
  }
  if (any_after_next()) {
    // этот кадр потерян — дальше уже есть
    next_++;
    stat_lost++;
    empty_run_ = 0;
    return MISSING;
  }
  if (ended_) {
    stop();
    return ENDED;
  }
  // буфер пуст: сеть не успевает — ждём, не сдвигаясь
  stat_underrun++;
  had_underrun_ = true;
  if (++empty_run_ >= 3) {
    playing_ = false;                  // копим заново, с запасом побольше
    if (prefill_ < PREFILL_MAX) prefill_++;
    first_ms_ = now_ms;
  }
  return MISSING;
}
