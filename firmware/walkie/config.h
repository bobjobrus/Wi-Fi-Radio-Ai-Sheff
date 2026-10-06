// Настройки сборки WiFi-рации. Выводы — для платы ESP32-S3-DevKitC-1 (и клонов YD-ESP32-S3).
#pragma once

#define FW_VERSION 9            // целое; мост раздаёт рациям прошивку с бо́льшим номером

// ── Микрофон INMP441 (разводка как в популярной сборке xiaozhi «bread-compact») ──
#define PIN_MIC_WS 4
#define PIN_MIC_SCK 5
#define PIN_MIC_SD 6
// ── Усилитель MAX98357A ──
#define PIN_AMP_DIN 7
#define PIN_AMP_BCLK 15
#define PIN_AMP_LRC 16
#define PIN_AMP_SD 17           // HIGH — усилитель работает, LOW — спит (нет шипения)
// ── Или аналоговый усилитель PAM8403 (настройка amp_pdm): звук импульсами PDM на вывод 7 →
//    фильтр 10 кОм / 1 кОм / 22 нФ → 1 мкФ → вход L усилителя. Вывод 15 драйверу нужен, но никуда не идёт ──
#define PIN_PDM_OUT PIN_AMP_DIN
#define PIN_PDM_CLK PIN_AMP_BCLK
// ── Кольцо WS2812 вокруг кнопки ──
#define PIN_LED 18
#define LED_COUNT 12
// ── Кнопка «говорить» (замыкает на GND) ──
#define PIN_PTT 1
// ── Регулятор громкости KY-040 (питать ТОЛЬКО от 3,3 В!) ──
#define PIN_ENC_A 2             // CLK
#define PIN_ENC_B 42            // DT
#define PIN_ENC_SW 41           // SW — нажатие: выключить/включить звук
// ── Аккумулятор: делитель 100 к + 100 к от «+» аккумулятора на GPIO8 (АЦП1) ──
#define PIN_BAT 8
#define BAT_DIVIDER 2.0f        // (R1 + R2) / R2
#define BAT_LOW_PCT 15          // предупредить
#define BAT_CRIT_PCT 5          // предупреждать чаще

#define DEFAULT_SERVER ""   // адрес своего моста можно вписать сюда; пусто — задать в настройках рации (или мост ищется в своей сети)
#define AP_PREFIX "RADIO-"      // имя точки доступа для настройки: RADIO-XXXX
#define LOCAL_PORT 47001

// ── Звук ──
#define AGC_TARGET_RMS 3000.0f  // ≈ −21 дБ от полной шкалы
#define AGC_GATE_RMS 20.0f      // тише — это тишина, усиление не поднимаем
#define AGC_MIN_DB 0.0f
#define AGC_MAX_DB 36.0f
#define TALK_LIMIT_MS 100000    // своя защита от залипшей кнопки (мост режет на 90 с)
#define WDT_TIMEOUT_MS 30000   // сторож: цикл рации или задача сети молчат дольше — перезапуск
#define ECO_IDLE_MS 15000       // столько тишины — Wi-Fi в режим экономии (батарея живёт в 2–3 раза дольше)
#define CPU_MHZ 160
