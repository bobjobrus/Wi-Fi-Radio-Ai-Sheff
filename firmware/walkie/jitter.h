// Буфер против «рывков» сети: копит 60–160 мс речи и отдаёт ровно по 20 мс.
// Переносимый код (без Arduino) — проверяется тестом на компьютере (tests/test_jitter.cpp).
//
// Правила:
//  • играть начинаем, когда накопилось prefill кадров (или прошло время ожидания);
//  • кадра нет, а более поздние уже пришли → он потерян, играем «заплатку» (MISSING) и идём дальше;
//  • буфер пуст, поток не окончен → ждём (MISSING без сдвига), после 3 пустых кадров копим заново
//    и на следующие передачи берём запас побольше;
//  • конец передачи: TALK_END и буфер опустел, или 600 мс тишины.
#pragma once
#include <stdint.h>
#include <string.h>
#include "protocol.h"

class JitterBuffer {
 public:
  enum Pop : uint8_t { IDLE, WAIT, FRAME, MISSING, ENDED };

  void begin(uint32_t talker, uint32_t burst, uint32_t now_ms);
  void stop();
  bool active() const { return active_; }
  bool matches(uint32_t talker, uint32_t burst) const {
    return active_ && talker == talker_ && burst == burst_;
  }
  uint32_t talker() const { return talker_; }
  uint32_t burst() const { return burst_; }
  // Кадр речи (163 байта, уже расшифрован). false — не наш поток или опоздал.
  bool push(uint32_t talker, uint32_t burst, uint16_t seq, const uint8_t* frame, uint8_t level,
            uint32_t now_ms);
  void end(uint32_t talker, uint32_t burst);
  Pop pop(uint8_t* out, uint8_t* level, uint32_t now_ms);

  uint8_t prefill() const { return prefill_; }
  int buffered() const;

  // статистика для страницы рации
  uint32_t stat_played = 0, stat_lost = 0, stat_late = 0, stat_underrun = 0, stat_dropped = 0;

  static const int SLOTS = 16;         // 320 мс
  static const uint8_t PREFILL_MIN = 3, PREFILL_MAX = 8;
  static const uint32_t SILENCE_END_MS = 600;

 private:
  struct Slot {
    bool used;
    uint16_t seq;
    uint8_t level;
    uint8_t data[wt::AUDIO_ENC_LEN];
  };
  Slot slots_[SLOTS];
  bool active_ = false, playing_ = false, ended_ = false, have_first_ = false;
  uint32_t talker_ = 0, burst_ = 0;
  uint16_t next_ = 0;
  uint32_t first_ms_ = 0, last_rx_ms_ = 0;
  uint8_t prefill_ = PREFILL_MIN;      // запас на эту передачу
  uint8_t target_ = PREFILL_MIN;       // запас на следующие (подстраивается)
  uint8_t empty_run_ = 0;
  uint8_t calm_bursts_ = 0;
  bool had_underrun_ = false;
  void clear_slots();
  bool any_after_next() const;
};
