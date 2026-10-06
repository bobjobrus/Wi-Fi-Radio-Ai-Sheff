// WiFi-рация Ай-Шефф — прошивка для ESP32-S3.
//
// Нажал кнопку → короткий «бип» → говоришь; отпустил — все остальные рации слышат.
// Связь через мост (hub/walkie_hub.py): рации в разных Wi-Fi-сетях (дом, магазин) слышат друг друга.
//
// Сборка: tools/build_firmware.sh (arduino-cli, ядро esp32 3.3.12, плата ESP32S3 Dev Module).
// Настройка с компьютера по USB: tools/provision.py; команды в мониторе порта — «help».
// ВАЖНО: функции в этом файле объявлять ДО использования — сборка идёт без ctags (см. build_firmware.sh).
#include <WiFi.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <stdarg.h>
#include "audio.h"
#include "battery.h"
#include "config.h"
#include "crypto.h"
#include "net.h"
#include "settings.h"
#include "state.h"
#include "ui.h"
#include "web.h"
#include "wifi_mgr.h"

Status g_st;

void flash(Flash f) {
  g_st.flash_ms = millis();
  g_st.flash = f;
}

// Журнал — в оба USB-разъёма платы: Serial0 = «COM» (через мост-чип), Serial = «USB» (родной порт ESP32-S3)
void logf(const char* fmt, ...) {
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  unsigned long t = millis();
  Serial0.printf("[%5lu.%01lu] %s\n", t / 1000, (t % 1000) / 100, buf);
  if (Serial) Serial.printf("[%5lu.%01lu] %s\n", t / 1000, (t % 1000) / 100, buf);
}

static uint32_t make_id() {
  uint64_t mac = ESP.getEfuseMac();
  uint32_t h = 2166136261u;                 // FNV-1a по 6 байтам MAC
  for (int i = 0; i < 6; i++) {
    h ^= (uint8_t)(mac >> (8 * i));
    h *= 16777619u;
  }
  return h ? h : 1;
}

// ─────────────── команды по USB ───────────────

static void print_status(Print& o) {
  o.printf("id %08X  имя «%s»  прошивка %d\n", (unsigned)g_st.id, g_set.name, FW_VERSION);
  o.printf("мост: %s  адрес в настройках: «%s»  ключ: %s\n", g_st.hub_addr, g_set.server,
           g_set.netkey[0] ? "задан" : "НЕТ");
  o.printf("связь: %d  Wi-Fi: %s %s %d дБм  задержка %u мс  раций в сети %u\n", (int)g_st.link,
           WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI(), g_st.rtt_ms, g_st.radios_online);
  for (int i = 0; i < WIFI_SLOTS; i++)
    if (g_set.wifi[i].ssid[0]) o.printf("сеть %d: «%s»\n", i, g_set.wifi[i].ssid);
  o.printf("аккумулятор %u%% %u мВ (замер %u мВ)  экономия Wi-Fi %s\n", g_st.bat_pct, g_st.bat_mv, g_st.bat_raw_mv,
           g_st.eco ? "сейчас" : "нет");
  o.printf("усилитель: %s\n", g_set.amp_pdm ? "PAM8403 (PDM через фильтр)" : "MAX98357A (I2S)");
  o.printf("громкость %u  АРУ %d  приём: сыграно %u потеряно %u опоздало %u провалов %u  чужих пакетов %u\n",
           g_set.volume, g_set.agc, (unsigned)g_st.rx_played, (unsigned)g_st.rx_lost, (unsigned)g_st.rx_late,
           (unsigned)g_st.rx_underrun, (unsigned)g_st.bad_packets);
}

static uint32_t s_testtx_until = 0;      // служебная проверка передачи: когда отпустить «кнопку»

static void command(String line, Print& o) {
  line.trim();
  if (!line.length()) return;
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? String() : line.substring(sp + 1);
  arg.trim();
  cmd.toLowerCase();
  if (cmd == "help") {
    o.println("show | name <имя> | server <адрес[:порт]|auto> | key <ключ> | wifi <сеть>|<пароль> | wifi-clear");
    o.println("volume <0..20> | agc on|off | led <0..100> | amp i2s|pdm | testtx <с> [Гц] | testmic <с> | micraw | reboot | factory");
  } else if (cmd == "show") {
    print_status(o);
  } else if (cmd == "name" && arg.length()) {
    strlcpy(g_set.name, arg.c_str(), sizeof(g_set.name));
    settings_save();
    o.println("OK name");
  } else if (cmd == "server") {
    strlcpy(g_set.server, arg == "auto" ? "" : arg.c_str(), sizeof(g_set.server));
    settings_save();
    net_server_changed();
    o.println("OK server");
  } else if (cmd == "key") {
    if (crypto_key_valid(arg.c_str())) {
      strlcpy(g_set.netkey, arg.c_str(), sizeof(g_set.netkey));
      settings_save();
      net_server_changed();
      o.println("OK key");
    } else {
      o.println("ERR key: нужно не меньше 12 букв и цифр");
    }
  } else if (cmd == "wifi") {
    int bar = arg.indexOf('|');
    String ssid = bar < 0 ? arg : arg.substring(0, bar);
    String pass = bar < 0 ? String() : arg.substring(bar + 1);
    if (wifi_add(ssid.c_str(), pass.c_str())) {
      wifi_reconnect();
      o.println("OK wifi");
    } else {
      o.println("ERR wifi");
    }
  } else if (cmd == "wifi-clear") {
    for (int i = WIFI_SLOTS - 1; i >= 0; i--) wifi_remove(i);
    o.println("OK wifi-clear");
  } else if (cmd == "volume") {
    g_set.volume = constrain(arg.toInt(), 0, 20);
    settings_save();
    o.println("OK volume");
  } else if (cmd == "agc") {
    g_set.agc = arg == "on";
    settings_save();
    o.println("OK agc");
  } else if (cmd == "amp") {
    g_set.amp_pdm = arg == "pdm";
    settings_save();
    o.println(g_set.amp_pdm ? "OK amp pdm (PAM8403), перезапуск" : "OK amp i2s (MAX98357A), перезапуск");
    delay(100);
    ESP.restart();
  } else if (cmd == "led") {
    g_set.led = constrain(arg.toInt(), 0, 100);
    settings_save();
    o.println("OK led");
  } else if (cmd == "reboot") {
    o.println("OK reboot");
    delay(100);
    ESP.restart();
  } else if (cmd == "testtx") {
    // testtx <секунды> [Гц] — как будто нажали кнопку и в микрофон звучит тон (проверка без микрофона и кнопки)
    int sec = constrain(arg.length() ? arg.toInt() : 3, 1, 20);
    int sp = arg.indexOf(' ');
    int hz = sp > 0 ? constrain(arg.substring(sp + 1).toInt(), 100, 4000) : 1000;
    audio_test_tone(hz);
    net_ptt(true);
    s_testtx_until = millis() + sec * 1000UL;
    o.printf("OK testtx %d с, %d Гц\n", sec, hz);
  } else if (cmd == "testmic") {
    // testmic <секунды> — как будто держат кнопку: в эфир идёт живой микрофон (проверка без кнопки)
    int sec = constrain(arg.length() ? arg.toInt() : 10, 1, 80);
    audio_test_tone(0);
    net_ptt(true);
    s_testtx_until = millis() + sec * 1000UL;
    o.printf("OK testmic %d с\n", sec);
  } else if (cmd == "micraw") {
    // сырые отсчёты микрофона за прошедшее время: оба канала (L — вывод L/R на G, R — на 3,3 В)
    int32_t mn[2], mx[2]; uint32_t n;
    audio_mic_raw(mn, mx, n);
    delay(1000);
    audio_mic_raw(mn, mx, n);
    o.printf("OK micraw кадров %u  L: %ld…%ld  R: %ld…%ld\n", (unsigned)n, (long)mn[0], (long)mx[0], (long)mn[1], (long)mx[1]);
  } else if (cmd == "factory") {
    settings_factory_reset(true);
    o.println("OK factory");
    delay(100);
    ESP.restart();
  } else {
    o.println("ERR неизвестная команда (help)");
  }
}

static void serial_poll(Stream& s, String& buf) {
  while (s.available()) {
    char c = (char)s.read();
    if (c == '\n' || c == '\r') {
      if (buf.length()) command(buf, s);
      buf = "";
    } else if (buf.length() < 200) {
      buf += c;
    }
  }
}

// ─────────────── старт ───────────────

static const char* reset_name(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "включение питания";
    case ESP_RST_SW: return "перезапуск программой";
    case ESP_RST_PANIC: return "сбой программы";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "сторож: зависание";
    case ESP_RST_BROWNOUT: return "просадка питания";
    case ESP_RST_EXT: return "кнопка RST";
    case ESP_RST_USB: return "по USB";
    default: return "другое";
  }
}

void setup() {
  Serial0.begin(115200);
  Serial.begin();
  Serial.setTxTimeoutMs(0);            // не ждать, если к «родному» USB никто не подключён
  g_st.id = make_id();
  snprintf(g_st.host, sizeof(g_st.host), "radio-%04x", (unsigned)(g_st.id & 0xFFFF));
  settings_load();
  crypto_set_key(g_set.netkey);
  g_st.boot_reason = reset_name(esp_reset_reason());
  logf("WiFi-рация, прошивка %d, id %08X, имя «%s», запуск: %s", FW_VERSION, (unsigned)g_st.id, g_set.name,
       g_st.boot_reason);
  // сторож: зависнут цикл рации или задача сети дольше 30 с — перезапуск и снова в сеть (29.09 рация 1 перестала
  // отвечать в простое; была ли это прошивка или модуль заряда — теперь видно на странице рации: «запуск: …»)
  esp_task_wdt_config_t wdt = {.timeout_ms = WDT_TIMEOUT_MS, .idle_core_mask = 0, .trigger_panic = true};
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
  enableLoopWDT();

  setCpuFrequencyMhz(CPU_MHZ);         // 160 МГц хватает с запасом, батарея живёт дольше
  battery_begin();
  ui_begin();
  bool force_portal = ui_ptt_held_at_boot();
  if (force_portal) logf("кнопка зажата при включении — режим настройки");
  if (!audio_begin()) strlcpy(g_st.last_error, "звук не запустился (проверьте провода I2S)", sizeof(g_st.last_error));
  wifi_begin(force_portal);
  net_begin();
  web_begin();
}

void loop() {
  static String b1, b2;
  ui_loop();
  wifi_loop();
  web_loop();
  settings_tick();
  battery_loop();
  serial_poll(Serial0, b1);
  serial_poll(Serial, b2);
  if (s_testtx_until && (int32_t)(millis() - s_testtx_until) >= 0) {
    s_testtx_until = 0;
    net_ptt(false);
    audio_test_tone(0);
  }
  delay(2);
}
