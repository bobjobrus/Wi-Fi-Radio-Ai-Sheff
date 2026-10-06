#include "wifi_mgr.h"
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include "config.h"
#include "crypto.h"
#include "settings.h"
#include "state.h"

static DNSServer s_dns;
static bool s_portal = false;
static bool s_portal_forced = false;
static bool s_mdns = false;
static bool s_scanning = false;
static uint32_t s_disc_since = 0, s_last_try = 0, s_connected_ms = 0;
static int s_rr = 0;       // по кругу, если сканирование ничего не дало

static void start_portal(bool ap_only) {
  if (s_portal) return;
  s_portal = true;
  g_st.portal = true;
  WiFi.mode(ap_only ? WIFI_AP : WIFI_AP_STA);
  char ssid[24];
  snprintf(ssid, sizeof(ssid), AP_PREFIX "%04X", (unsigned)(g_st.id & 0xFFFF));
  WiFi.softAP(ssid);
  delay(100);
  s_dns.start(53, "*", WiFi.softAPIP());
  logf("точка доступа %s открыта: подключитесь и откройте http://%s/", ssid, WiFi.softAPIP().toString().c_str());
}

static void stop_portal() {
  if (!s_portal) return;
  s_dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  s_portal = false;
  g_st.portal = false;
  logf("точка доступа закрыта");
}

static void begin_known(int idx) {
  settings_lock();
  char ssid[33], pass[65];
  strlcpy(ssid, g_set.wifi[idx].ssid, sizeof(ssid));
  strlcpy(pass, g_set.wifi[idx].pass, sizeof(pass));
  settings_unlock();
  logf("подключаюсь к «%s»", ssid);
  WiFi.begin(ssid, pass[0] ? pass : nullptr);
}

static void try_connect() {
  s_last_try = millis();
  int n = wifi_count();
  if (n == 0) return;
  if (s_portal && WiFi.softAPgetStationNum() > 0) return;   // человек настраивает с телефона — не мешаем
  if (n == 1) {
    begin_known(0);
    return;
  }
  if (!s_scanning) {
    WiFi.scanNetworks(true);      // асинхронно; итог заберём в wifi_loop
    s_scanning = true;
  }
}

static void scan_done(int found) {
  s_scanning = false;
  int best = -1, best_rssi = -1000;
  for (int i = 0; i < found; i++) {
    String s = WiFi.SSID(i);
    for (int k = 0; k < WIFI_SLOTS; k++) {
      if (g_set.wifi[k].ssid[0] && s == g_set.wifi[k].ssid && WiFi.RSSI(i) > best_rssi) {
        best = k;
        best_rssi = WiFi.RSSI(i);
      }
    }
  }
  WiFi.scanDelete();
  if (best < 0) {                 // ничего знакомого не видно (или скрытая сеть) — пробуем по очереди
    best = s_rr++ % wifi_count();
  }
  begin_known(best);
}

void wifi_begin(bool force_portal) {
  WiFi.persistent(false);
  WiFi.setHostname(g_st.host);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);           // без энергосбережения: меньше задержка и потери на приёме
  WiFi.setAutoReconnect(true);
  s_disc_since = millis();
  if (force_portal || wifi_count() == 0) {
    s_portal_forced = force_portal;
    start_portal(wifi_count() == 0);
  } else if (!crypto_has_key()) {
    start_portal(false);          // Wi-Fi есть, а ключа сети нет — дать ввести
  }
  if (wifi_count() > 0) try_connect();
}

void wifi_reconnect() {
  WiFi.disconnect(false, false);
  s_disc_since = millis();
  try_connect();
}

bool wifi_portal_active() { return s_portal; }

void wifi_loop() {
  uint32_t now = millis();
  if (s_portal) s_dns.processNextRequest();
  if (s_scanning) {
    int r = WiFi.scanComplete();
    if (r >= 0) {
      scan_done(r);
    } else if (r == WIFI_SCAN_FAILED) {
      s_scanning = false;
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    if (s_disc_since) {
      s_disc_since = 0;
      s_connected_ms = now;
      logf("Wi-Fi: «%s», адрес %s, сигнал %d дБм", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
           WiFi.RSSI());
      if (!s_mdns) s_mdns = MDNS.begin(g_st.host);
      if (s_mdns) MDNS.addService("http", "tcp", 80);
    }
    // настроились и связь держится минуту — точку доступа закрыть (если её не открывали кнопкой)
    if (s_portal && !s_portal_forced && crypto_has_key() && now - s_connected_ms > 60000 &&
        WiFi.softAPgetStationNum() == 0 && g_st.link == Link::OK)
      stop_portal();
  } else {
    if (!s_disc_since) {
      s_disc_since = now;
      logf("Wi-Fi пропал");
    }
    if (wifi_count() > 0 && now - s_last_try > 15000 && !s_scanning) try_connect();
    if (!s_portal && now - s_disc_since > 60000) start_portal(false);
    // 15 минут без сети и никто не настраивает — перезапуск (иногда помогает от зависшего Wi-Fi)
    if (now - s_disc_since > 15UL * 60000 && (!s_portal || WiFi.softAPgetStationNum() == 0)) {
      logf("15 минут без Wi-Fi — перезапуск");
      delay(200);
      ESP.restart();
    }
  }
}
