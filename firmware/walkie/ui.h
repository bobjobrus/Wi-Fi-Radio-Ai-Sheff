// Кнопка «говорить», регулятор громкости с нажатием, кольцо WS2812.
#pragma once
#include <stdint.h>

void ui_begin();
void ui_loop();                  // из loop(): кнопки + подсветка (~30 кадров/с)
bool ui_ptt_held_at_boot();      // держат кнопку при включении → режим настройки
void ui_show_volume();
