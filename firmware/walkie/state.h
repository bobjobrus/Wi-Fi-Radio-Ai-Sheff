// Общее состояние рации: пишут задачи сети/звука, читают подсветка и страница настройки.
#pragma once
#include <Arduino.h>

enum class Link : uint8_t { NO_WIFI, NO_KEY, NO_HUB, WRONG_KEY, OK };
enum class Tx : uint8_t { IDLE, REQUEST, CHIRP, TALK, WAIT_RELEASE };
enum class Flash : uint8_t { NONE, BUSY, ERROR };

struct Status {
  volatile Link link = Link::NO_WIFI;
  volatile Tx tx = Tx::IDLE;
  volatile bool portal = false;        // открыта точка доступа для настройки
  volatile bool muted = false;
  volatile bool rx_active = false;
  volatile uint8_t rx_level = 0;
  volatile uint8_t tx_level = 0;
  volatile bool peers_ok = true;       // все мосты сети на связи
  volatile uint8_t radios_online = 0;
  volatile uint16_t rtt_ms = 0;
  volatile bool updating = false;
  volatile uint8_t update_pct = 0;
  volatile uint32_t bad_packets = 0;
  volatile Flash flash = Flash::NONE;
  volatile uint32_t flash_ms = 0;
  volatile uint32_t last_activity_ms = 0;
  volatile uint32_t rx_played = 0, rx_lost = 0, rx_late = 0, rx_underrun = 0;
  volatile uint8_t bat_pct = 255;      // 255 — измеряется (bat_measuring) / нет аккумулятора
  volatile uint16_t bat_mv = 0;         // оценка (без просадок под нагрузкой); 0 — не измерено
  volatile uint16_t bat_raw_mv = 0;     // последний замер как есть — для проверки
  volatile bool bat_low = false;        // «пора на зарядку»: заряд низкий уже 30 с подряд
  volatile bool bat_measuring = true;   // bat_pct = 255, но это «измеряется», а не питание от USB
  volatile bool eco = false;           // Wi-Fi сейчас в режиме экономии
  char hub_name[32] = "";
  char hub_addr[48] = "";
  char last_error[64] = "";
  const char* boot_reason = "";         // почему рация запустилась: включение, сторож, сбой, просадка питания…
  uint32_t id = 0;
  char host[24] = "";
};

extern Status g_st;

void flash(Flash f);
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
