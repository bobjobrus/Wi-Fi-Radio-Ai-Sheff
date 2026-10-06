#include "settings.h"
#include <Preferences.h>
#include "config.h"
#include "state.h"

Settings g_set;
static Preferences s_prefs;
static SemaphoreHandle_t s_mutex = nullptr;
static uint32_t s_save_at = 0;

void settings_lock() {
  if (!s_mutex) s_mutex = xSemaphoreCreateRecursiveMutex();
  xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
}
void settings_unlock() { xSemaphoreGiveRecursive(s_mutex); }

static void get_str(const char* key, char* dst, size_t cap, const char* def) {
  String v = s_prefs.getString(key, def);
  strlcpy(dst, v.c_str(), cap);
}

void settings_load() {
  settings_lock();
  memset(&g_set, 0, sizeof(g_set));
  s_prefs.begin("walkie", true);
  for (int i = 0; i < WIFI_SLOTS; i++) {
    char k[8];
    snprintf(k, sizeof(k), "ssid%d", i);
    get_str(k, g_set.wifi[i].ssid, sizeof(g_set.wifi[i].ssid), "");
    snprintf(k, sizeof(k), "pass%d", i);
    get_str(k, g_set.wifi[i].pass, sizeof(g_set.wifi[i].pass), "");
  }
  char def_name[24];
  snprintf(def_name, sizeof(def_name), "Рация %04X", (unsigned)(g_st.id & 0xFFFF));
  get_str("name", g_set.name, sizeof(g_set.name), def_name);
  get_str("server", g_set.server, sizeof(g_set.server), DEFAULT_SERVER);
  get_str("netkey", g_set.netkey, sizeof(g_set.netkey), "");
  g_set.volume = s_prefs.getUChar("volume", 14);
  g_set.agc = s_prefs.getBool("agc", true);
  g_set.mic_gain = s_prefs.getChar("micgain", 24);
  g_set.mic_right = s_prefs.getBool("micright", false);
  g_set.led = s_prefs.getUChar("led", 60);
  g_set.roger = s_prefs.getBool("roger", true);
  g_set.enc_invert = s_prefs.getBool("encinv", false);
  // экономия Wi-Fi по умолчанию ВЫКЛ (прошивка 6): в простое рация берёт так мало тока, что модуль заряда
  // (как пауэрбанк) может решить, что нагрузку отключили, и выключить 5 В — 29.09 рация 1 «выключилась сама»
  g_set.eco = s_prefs.getBool("eco6", false);   // новый ключ: старое «eco = да» у настроенных раций не в счёт
  g_set.amp_pdm = s_prefs.getBool("amppdm", false);
  s_prefs.end();
  if (g_set.volume > 20) g_set.volume = 14;
  if (g_set.led > 100) g_set.led = 60;
  settings_unlock();
}

void settings_save() {
  settings_lock();
  s_prefs.begin("walkie", false);
  for (int i = 0; i < WIFI_SLOTS; i++) {
    char k[8];
    snprintf(k, sizeof(k), "ssid%d", i);
    s_prefs.putString(k, g_set.wifi[i].ssid);
    snprintf(k, sizeof(k), "pass%d", i);
    s_prefs.putString(k, g_set.wifi[i].pass);
  }
  s_prefs.putString("name", g_set.name);
  s_prefs.putString("server", g_set.server);
  s_prefs.putString("netkey", g_set.netkey);
  s_prefs.putUChar("volume", g_set.volume);
  s_prefs.putBool("agc", g_set.agc);
  s_prefs.putChar("micgain", g_set.mic_gain);
  s_prefs.putBool("micright", g_set.mic_right);
  s_prefs.putUChar("led", g_set.led);
  s_prefs.putBool("roger", g_set.roger);
  s_prefs.putBool("encinv", g_set.enc_invert);
  s_prefs.putBool("eco6", g_set.eco);
  s_prefs.putBool("amppdm", g_set.amp_pdm);
  s_prefs.end();
  s_save_at = 0;
  settings_unlock();
}

void settings_save_later() { s_save_at = millis() + 3000; }

void settings_tick() {
  if (s_save_at && (int32_t)(millis() - s_save_at) >= 0) {
    s_save_at = 0;
    settings_lock();
    s_prefs.begin("walkie", false);
    s_prefs.putUChar("volume", g_set.volume);
    s_prefs.end();
    settings_unlock();
  }
}

void settings_factory_reset(bool full) {
  // регулятором (full = false) стираются Wi-Fi и настройки звука/подсветки; ключ сети, имя рации
  // и адрес моста остаются — иначе после сброса рацию может вернуть в эфир только автор.
  // Полный сброс — только командой factory по USB.
  settings_lock();
  s_prefs.begin("walkie", false);
  String name = s_prefs.getString("name", ""), server = s_prefs.getString("server", ""),
         netkey = s_prefs.getString("netkey", "");
  bool amp_pdm = s_prefs.getBool("amppdm", false);
  s_prefs.clear();
  if (full) name = server = netkey = "", amp_pdm = false;
  if (name.length()) s_prefs.putString("name", name);
  if (server.length()) s_prefs.putString("server", server);
  if (netkey.length()) s_prefs.putString("netkey", netkey);
  if (amp_pdm) s_prefs.putBool("amppdm", true);
  s_prefs.end();
  settings_unlock();
}

int wifi_count() {
  int n = 0;
  for (int i = 0; i < WIFI_SLOTS; i++)
    if (g_set.wifi[i].ssid[0]) n++;
  return n;
}

bool wifi_add(const char* ssid, const char* pass) {
  if (!ssid || !ssid[0] || strlen(ssid) > 32 || strlen(pass) > 64) return false;
  settings_lock();
  int slot = -1;
  for (int i = 0; i < WIFI_SLOTS; i++)
    if (strcmp(g_set.wifi[i].ssid, ssid) == 0) slot = i;              // та же сеть — обновить пароль
  for (int i = 0; i < WIFI_SLOTS && slot < 0; i++)
    if (!g_set.wifi[i].ssid[0]) slot = i;
  if (slot < 0) {                                                      // всё занято — сдвинуть, новая в конец
    for (int i = 0; i < WIFI_SLOTS - 1; i++) g_set.wifi[i] = g_set.wifi[i + 1];
    slot = WIFI_SLOTS - 1;
  }
  strlcpy(g_set.wifi[slot].ssid, ssid, sizeof(g_set.wifi[slot].ssid));
  strlcpy(g_set.wifi[slot].pass, pass, sizeof(g_set.wifi[slot].pass));
  settings_save();
  settings_unlock();
  return true;
}

void wifi_remove(int idx) {
  if (idx < 0 || idx >= WIFI_SLOTS) return;
  settings_lock();
  for (int i = idx; i < WIFI_SLOTS - 1; i++) g_set.wifi[i] = g_set.wifi[i + 1];
  memset(&g_set.wifi[WIFI_SLOTS - 1], 0, sizeof(WifiCred));
  settings_save();
  settings_unlock();
}
