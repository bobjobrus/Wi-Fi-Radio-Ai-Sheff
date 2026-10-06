// Прогон защиты от свиста (firmware/walkie/howl.cpp) по настоящим записям WAV 16 кГц:
// речь — сколько ложных срабатываний (должно быть 0), запись свиста — когда найден.
//   clang++ -std=c++17 -O2 -I firmware/walkie tools/howl_eval.cpp firmware/walkie/howl.cpp -o /tmp/howl_eval
//   /tmp/howl_eval запись1.wav запись2.wav …
// Записи речи берутся «попугаем»: hub/sim_radio.py … echo --save ПАПКА (в git их нет — голос автора).
// 30.09.2026: 23 записи рации 1 (116 с речи, в одной — ровный писк 1860 Гц в комнате) — 0 срабатываний;
// звук видео со свистом — найден на 1,66 с, 1800 Гц.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include "howl.h"
static std::vector<float> rd(const char* p) {
  std::vector<float> v; FILE* f = fopen(p, "rb"); if (!f) return v;
  char h[44]; if (fread(h, 1, 44, f) != 44) { fclose(f); return v; }
  int ch = *(int16_t*)(h + 22); int16_t s[2];
  while (fread(s, 2, ch, f) == (size_t)ch) v.push_back(s[0]);
  fclose(f); return v;
}
int main(int argc, char** argv) {
  for (int a = 1; a < argc; a++) {
    auto x = rd(argv[a]); HowlGuard g; g.burst_start(0);
    int hits = 0; float first = -1, hz = 0;
    for (size_t i = 0; i + HowlGuard::N <= x.size(); i += HowlGuard::N) {
      float y[HowlGuard::N]; memcpy(y, &x[i], sizeof(y));
      if (g.process(y, (uint32_t)(i / 16))) { hits++; if (first < 0) { first = i / 16000.0f; hz = g.notch_hz(g.notches() - 1); } }
    }
    const char* n = strrchr(argv[a], '/');
    printf("%-26s %5.1f с  срабатываний %d%s", n ? n + 1 : argv[a], x.size() / 16000.0f, hits, hits ? "" : "\n");
    if (hits) printf("  первое на %.2f с, %.0f Гц\n", first, hz);
  }
}
