// Настройки рации в энергонезависимой памяти (NVS, раздел «walkie»).
#pragma once
#include <Arduino.h>

#define WIFI_SLOTS 3

struct WifiCred {
  char ssid[33];
  char pass[65];
};

struct Settings {
  WifiCred wifi[WIFI_SLOTS];
  char name[32];        // «Касса», «Склад», «Дом»
  char server[64];      // адрес моста «host[:port]», пусто — искать в своей сети
  char netkey[40];      // ключ сети
  uint8_t volume;       // 0..20 (шаг 2 дБ)
  bool agc;             // автоусиление микрофона
  int8_t mic_gain;      // дБ, если АРУ выключена
  bool mic_right;       // микрофон на правом канале (L/R на 3,3 В)
  uint8_t led;          // яркость подсветки 0..100
  bool roger;           // короткий сигнал в конце чужой передачи
  bool enc_invert;      // регулятор крутится «не в ту сторону»
  bool eco;             // экономия Wi-Fi в тишине (для аккумулятора)
  bool amp_pdm;         // аналоговый усилитель PAM8403 через фильтр (вместо MAX98357A); после смены — перезапуск
};

extern Settings g_set;

void settings_load();
void settings_save();
void settings_save_later();           // громкость: сохранить через 3 с после последнего поворота
void settings_tick();                 // из loop()
void settings_factory_reset(bool full = false);
int wifi_count();
bool wifi_add(const char* ssid, const char* pass);
void wifi_remove(int idx);
void settings_lock();
void settings_unlock();
