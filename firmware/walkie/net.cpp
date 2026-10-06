#include "net.h"
#include <Preferences.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include "audio.h"
#include "config.h"
#include "crypto.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "mbedtls/sha256.h"
#include "protocol.h"
#include "settings.h"
#include "state.h"

static QueueHandle_t s_ui_q = nullptr;
static volatile bool s_reconfigure = true;

static int s_sock = -1;
static sockaddr_in s_hub = {};
static bool s_hub_set = false;       // куда слать: адрес моста известен
static bool s_auto = true;           // адрес не задан — ищем широковещанием
static uint32_t s_hello_ms = 0, s_ack_ms = 0, s_resolve_ms = 0, s_lost_ms = 0;
static bool s_ever_ok = false;
static uint32_t s_bad_before_ack = 0;

static bool s_ptt = false;
static uint32_t s_burst = 0;
static uint16_t s_seq = 0;
static uint32_t s_req_ms = 0, s_req_start = 0, s_chirp_end = 0, s_talk_start = 0;
static uint8_t s_req_tries = 0;
static uint32_t s_end_burst = 0, s_end_next = 0;
static uint8_t s_end_left = 0;

static uint16_t s_fw_ver = 0, s_http_port = 47080;
static uint32_t s_fw_size = 0;
static uint8_t s_fw_sha[32];
static uint32_t s_ota_fail_ms = 0;
static char s_ota_done[17] = "";      // sha уже установленной с моста прошивки — не качать её снова

static uint32_t s_busy_ms = 0;       // последний разговор (свой или чужой)

// Wi-Fi-экономия: в простое модем спит между маяками точки (задержка первых пакетов до ~0,3 с —
// её съедает буфер), при разговоре — без сна
static void eco_update(uint32_t now) {
  bool busy = g_st.tx != Tx::IDLE || g_st.rx_active;
  if (busy) s_busy_ms = now;
  bool want = g_set.eco && now - s_busy_ms > ECO_IDLE_MS && !g_st.updating;
  if (want != g_st.eco) {
    WiFi.setSleep(want);
    g_st.eco = want;
  }
}

static uint8_t s_out[256];
static uint8_t s_in[512];

// ─────────────── отправка ───────────────

static size_t hdr(uint8_t* b, uint8_t type) {
  b[0] = 'W';
  b[1] = 'T';
  b[2] = wt::VERSION;
  b[3] = type;
  wt::put32(b + 4, g_st.id);
  return wt::HDR_LEN;
}

static void send_raw(const uint8_t* b, size_t n, const sockaddr_in& to) {
  sendto(s_sock, b, n, 0, (const sockaddr*)&to, sizeof(to));
}

static void send_hub(uint8_t* b, size_t n) {
  if (s_sock < 0 || !crypto_has_key()) return;
  n = crypto_sign(b, n);
  if (s_hub_set) {
    send_raw(b, n, s_hub);
  } else if (s_auto) {
    // ищем мост в своей сети: и на адрес подсети, и на общий широковещательный
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(wt::DEFAULT_PORT);
    to.sin_addr.s_addr = (uint32_t)WiFi.broadcastIP();
    send_raw(b, n, to);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    send_raw(b, n, to);
  }
}

static void send_hello() {
  uint8_t* b = s_out;
  size_t n = hdr(b, wt::T_HELLO);
  wt::put16(b + n, FW_VERSION);
  n += 2;
  b[n++] = g_st.muted ? wt::FLAG_MUTED : 0;
  b[n++] = (uint8_t)(int8_t)WiFi.RSSI();
  wt::put32(b + n, millis());
  n += 4;
  wt::put16(b + n, g_st.rtt_ms);
  n += 2;
  settings_lock();
  size_t nl = strlen(g_set.name);
  if (nl > wt::NAME_MAX_BYTES) {
    nl = wt::NAME_MAX_BYTES;
    while (nl > 0 && ((uint8_t)g_set.name[nl] & 0xC0) == 0x80) nl--;   // не резать букву пополам
  }
  b[n++] = (uint8_t)nl;
  memcpy(b + n, g_set.name, nl);
  settings_unlock();
  n += nl;
  b[n++] = g_st.bat_pct;               // 0..100 %, 255 — питание не от аккумулятора
  send_hub(b, n);
  s_hello_ms = millis();
}

static void send_burst_pkt(uint8_t type, uint32_t burst) {
  uint8_t* b = s_out;
  size_t n = hdr(b, type);
  wt::put32(b + n, burst);
  n += 4;
  send_hub(b, n);
}

static void send_req() {
  send_burst_pkt(wt::T_TALK_REQ, s_burst);
  s_req_ms = millis();
  s_req_tries++;
}

static void send_audio(const TxFrame& f) {
  uint8_t* b = s_out;
  size_t n = hdr(b, wt::T_AUDIO);
  wt::put32(b + n, s_burst);
  wt::put16(b + n + 4, s_seq);
  b[n + 6] = wt::CODEC_ADPCM16;
  b[n + 7] = f.level;
  n += wt::AUDIO_FIX;
  memcpy(b + n, f.data, wt::AUDIO_ENC_LEN);
  crypto_ctr(g_st.id, s_burst, s_seq, b + n, wt::AUDIO_ENC_LEN);
  n += wt::AUDIO_ENC_LEN;
  send_hub(b, n);
  s_seq++;
}

// ─────────────── передача (кнопка) ───────────────

static void stop_tx() {
  audio_set_streaming(false);
  g_st.tx_level = 0;
}

static void schedule_end() {
  s_end_burst = s_burst;
  s_end_left = 3;              // конец передачи — трижды, вдруг потеряется
  s_end_next = millis();
}

static void on_ptt(bool down) {
  s_ptt = down;
  g_st.last_activity_ms = millis();
  if (down && g_st.eco) {
    WiFi.setSleep(false);
    g_st.eco = false;
    s_busy_ms = millis();
  }
  if (down) {
    if (g_st.tx != Tx::IDLE) return;
    if (g_st.link != Link::OK) {
      audio_tone(Tone::ERROR);
      flash(Flash::ERROR);
      g_st.tx = Tx::WAIT_RELEASE;
      return;
    }
    if (g_st.rx_active) {       // кто-то говорит — ждём своей очереди
      audio_tone(Tone::BUSY);
      flash(Flash::BUSY);
      g_st.tx = Tx::WAIT_RELEASE;
      return;
    }
    s_burst = esp_random();
    if (!s_burst) s_burst = 1;
    s_seq = 0;
    s_req_tries = 0;
    s_req_start = millis();
    g_st.tx = Tx::REQUEST;
    send_req();
  } else {
    Tx t = g_st.tx;
    if (t == Tx::REQUEST || t == Tx::CHIRP || t == Tx::TALK) {
      stop_tx();
      schedule_end();
    }
    g_st.tx = Tx::IDLE;
  }
}

void net_ptt(bool down) {
  if (s_ui_q) xQueueSend(s_ui_q, &down, 0);
}

void net_server_changed() { s_reconfigure = true; }

// ─────────────── приём ───────────────

static void on_ack(const uint8_t* pl, size_t n, const sockaddr_in& from) {
  if (n < wt::ACK_FIX) return;
  uint32_t rtt = millis() - wt::get32(pl);
  if (rtt < 10000) g_st.rtt_ms = (uint16_t)rtt;
  g_st.radios_online = pl[4];
  g_st.peers_ok = pl[6] >= pl[5];
  s_fw_ver = wt::get16(pl + 8);
  s_fw_size = wt::get32(pl + 10);
  memcpy(s_fw_sha, pl + 14, 32);
  s_http_port = wt::get16(pl + 46);
  if (n > wt::ACK_FIX) {
    size_t nl = pl[wt::ACK_FIX];
    if (nl > sizeof(g_st.hub_name) - 1) nl = sizeof(g_st.hub_name) - 1;
    if (wt::ACK_FIX + 1 + nl <= n) {
      memcpy(g_st.hub_name, pl + wt::ACK_FIX + 1, nl);
      g_st.hub_name[nl] = 0;
    }
  }
  if (!s_hub_set) {            // нашли мост широковещанием
    s_hub = from;
    s_hub_set = true;
    IPAddress ip(from.sin_addr.s_addr);
    snprintf(g_st.hub_addr, sizeof(g_st.hub_addr), "%s:%u", ip.toString().c_str(), ntohs(from.sin_port));
    logf("мост найден: %s «%s»", g_st.hub_addr, g_st.hub_name);
  }
  bool was_ok = g_st.link == Link::OK;
  s_ack_ms = millis();
  s_bad_before_ack = 0;
  g_st.link = Link::OK;
  g_st.last_error[0] = 0;
  if (!was_ok) {
    logf("на связи с мостом «%s», задержка %u мс", g_st.hub_name, g_st.rtt_ms);
    if (!s_ever_ok || millis() - s_lost_ms > 10000) audio_tone(Tone::CONNECTED);
    s_ever_ok = true;
  }
}

static void on_grant(uint32_t burst) {
  if (g_st.tx != Tx::REQUEST || burst != s_burst) return;
  audio_rx_stop();
  audio_tone(Tone::PERMIT);
  s_chirp_end = millis() + 240;     // сигнал «можно говорить» (135 мс + до 80 мс в буферах вывода) в микрофон не пускаем
  g_st.tx = Tx::CHIRP;
}

static void on_deny(uint32_t burst, uint8_t reason) {
  if (burst != s_burst) return;
  Tx t = g_st.tx;
  if (t != Tx::REQUEST && t != Tx::CHIRP && t != Tx::TALK) return;
  if (reason == wt::DENY_UNKNOWN && t == Tx::REQUEST && s_req_tries < 6) {
    send_hello();                    // мост перезапускался и нас не знает — представиться и повторить
    send_req();
    return;
  }
  stop_tx();
  audio_tone(reason == wt::DENY_TIMEOUT ? Tone::ERROR : Tone::BUSY);
  flash(Flash::BUSY);
  logf("отказ эфира: причина %u", reason);
  g_st.tx = s_ptt ? Tx::WAIT_RELEASE : Tx::IDLE;
}

static void on_audio(uint32_t src, const uint8_t* pl, size_t n) {
  if (n != wt::AUDIO_FIX + wt::AUDIO_ENC_LEN || pl[6] != wt::CODEC_ADPCM16) return;
  if (g_st.eco) {                    // пошла речь — проснуться сразу, не ждать следующего круга
    WiFi.setSleep(false);
    g_st.eco = false;
    s_busy_ms = millis();
  }
  Tx t = g_st.tx;
  if (t == Tx::REQUEST || t == Tx::CHIRP || t == Tx::TALK) return;   // сами говорим
  if (src == g_st.id) return;
  uint32_t burst = wt::get32(pl);
  uint16_t seq = wt::get16(pl + 4);
  static uint8_t frame[wt::AUDIO_ENC_LEN];
  memcpy(frame, pl + wt::AUDIO_FIX, wt::AUDIO_ENC_LEN);
  crypto_ctr(src, burst, seq, frame, wt::AUDIO_ENC_LEN);
  audio_rx_frame(src, burst, seq, frame, pl[7]);
}

static void handle_packet(const uint8_t* p, size_t n, const sockaddr_in& from) {
  if (n < wt::HDR_LEN || p[0] != 'W' || p[1] != 'T') return;
  bool from_hub = s_hub_set && from.sin_addr.s_addr == s_hub.sin_addr.s_addr && from.sin_port == s_hub.sin_port;
  if (n == wt::HDR_LEN && p[3] == wt::T_BAD_KEY) {     // мост: «твоя подпись не сошлась»
    if (from_hub || !s_hub_set) s_bad_before_ack++;
    return;
  }
  if (n < wt::HDR_LEN + wt::MAC_LEN) return;
  if (!crypto_verify(p, n)) {
    g_st.bad_packets = g_st.bad_packets + 1;
    if (from_hub || !s_hub_set) s_bad_before_ack++;
    return;
  }
  if (p[2] != wt::VERSION) return;
  uint8_t type = p[3];
  uint32_t src = wt::get32(p + 4);
  const uint8_t* pl = p + wt::HDR_LEN;
  size_t pn = n - wt::HDR_LEN - wt::MAC_LEN;
  if (!from_hub && !(s_auto && !s_hub_set && type == wt::T_HELLO_ACK)) return;
  switch (type) {
    case wt::T_HELLO_ACK: on_ack(pl, pn, from); break;
    case wt::T_TALK_GRANT:
      if (pn >= 4) on_grant(wt::get32(pl));
      break;
    case wt::T_TALK_DENY:
      if (pn >= 5) on_deny(wt::get32(pl), pl[4]);
      break;
    case wt::T_AUDIO: on_audio(src, pl, pn); break;
    case wt::T_TALK_END:
      if (pn >= 4) audio_rx_end(src, wt::get32(pl));
      break;
    default: break;
  }
}

static void poll_rx(int timeout_ms) {
  fd_set rf;
  FD_ZERO(&rf);
  FD_SET(s_sock, &rf);
  timeval tv = {0, timeout_ms * 1000};
  if (select(s_sock + 1, &rf, nullptr, nullptr, &tv) <= 0) return;
  for (int k = 0; k < 24; k++) {
    sockaddr_in from = {};
    socklen_t fl = sizeof(from);
    int n = recvfrom(s_sock, s_in, sizeof(s_in), MSG_DONTWAIT, (sockaddr*)&from, &fl);
    if (n <= 0) break;
    handle_packet(s_in, (size_t)n, from);
  }
}

// ─────────────── адрес моста ───────────────

static void resolve_hub() {
  s_resolve_ms = millis();
  char srv[64];
  settings_lock();
  strlcpy(srv, g_set.server, sizeof(srv));
  bool key_ok = crypto_set_key(g_set.netkey);
  settings_unlock();
  if (!key_ok) {
    g_st.link = Link::NO_KEY;
    return;
  }
  String s(srv);
  s.trim();
  if (s.length() == 0 || s.equalsIgnoreCase("auto")) {
    if (!s_auto || g_st.link != Link::OK) s_hub_set = false;
    s_auto = true;
    if (!s_hub_set) strlcpy(g_st.hub_addr, "ищу в своей сети", sizeof(g_st.hub_addr));
    return;
  }
  s_auto = false;
  String host = s;
  uint16_t port = wt::DEFAULT_PORT;
  int c = s.lastIndexOf(':');
  if (c > 0) {
    host = s.substring(0, c);
    port = (uint16_t)s.substring(c + 1).toInt();
    if (!port) port = wt::DEFAULT_PORT;
  }
  IPAddress ip;
  if (!ip.fromString(host)) {
    if (!WiFi.hostByName(host.c_str(), ip)) {
      snprintf(g_st.last_error, sizeof(g_st.last_error), "адрес «%s» не найден", host.c_str());
      logf("%s", g_st.last_error);
      return;
    }
  }
  s_hub = {};
  s_hub.sin_family = AF_INET;
  s_hub.sin_port = htons(port);
  s_hub.sin_addr.s_addr = (uint32_t)ip;
  s_hub_set = true;
  snprintf(g_st.hub_addr, sizeof(g_st.hub_addr), "%s:%u", ip.toString().c_str(), port);
}

static bool open_socket() {
  s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s_sock < 0) return false;
  int yes = 1;
  setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
  sockaddr_in me = {};
  me.sin_family = AF_INET;
  me.sin_port = htons(LOCAL_PORT);
  me.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s_sock, (sockaddr*)&me, sizeof(me)) < 0) {
    close(s_sock);
    s_sock = -1;
    return false;
  }
  return true;
}

// ─────────────── обновление прошивки с моста ───────────────

static void ota_from_hub() {
  g_st.updating = true;
  g_st.update_pct = 0;
  char sha[17];
  for (int i = 0; i < 8; i++) snprintf(sha + 2 * i, 3, "%02x", s_fw_sha[i]);
  IPAddress ip(s_hub.sin_addr.s_addr);
  char url[96];
  snprintf(url, sizeof(url), "http://%s:%u/fw/%s.bin", ip.toString().c_str(), s_http_port, sha);
  logf("обновление: версия %u, %s", s_fw_ver, url);
  HTTPClient http;
  http.setTimeout(20000);
  bool ok = false;
  if (http.begin(url) && http.GET() == 200 && http.getSize() == (int)s_fw_size && Update.begin(s_fw_size)) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    auto* stream = http.getStreamPtr();
    static uint8_t buf[2048];
    int left = (int)s_fw_size;
    uint32_t last = millis();
    while (left > 0 && millis() - last < 20000) {
      esp_task_wdt_reset();               // загрузка идёт — не зависание
      int av = stream->available();
      if (av <= 0) {
        if (!http.connected()) break;
        delay(2);
        continue;
      }
      int want = av < (int)sizeof(buf) ? av : (int)sizeof(buf);
      if (want > left) want = left;
      int r = stream->readBytes(buf, want);
      if (r <= 0) continue;
      last = millis();
      mbedtls_sha256_update(&ctx, buf, r);
      if (Update.write(buf, r) != (size_t)r) break;
      left -= r;
      g_st.update_pct = (uint8_t)(100 - (int64_t)left * 100 / s_fw_size);
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    if (left == 0 && memcmp(digest, s_fw_sha, 32) == 0) {
      ok = Update.end();
    } else {
      logf("обновление: файл не сошёлся (осталось %d байт)", left);
      Update.abort();
    }
  }
  http.end();
  if (ok) {
    Preferences p;                      // запомнить, что эту сборку уже ставили
    p.begin("walkie", false);
    p.putString("otasha", sha);
    p.end();
    logf("обновление записано, перезапуск");
    delay(300);
    ESP.restart();
  }
  logf("обновление не удалось, повтор через час");
  s_ota_fail_ms = millis();
  g_st.updating = false;
}

// ─────────────── основной цикл сети ───────────────

static void net_task(void*) {
  esp_task_wdt_add(nullptr);            // сторож (см. setup): задача сети отмечается каждый круг
  for (;;) {
    esp_task_wdt_reset();
    if (WiFi.status() != WL_CONNECTED) {
      if (g_st.link != Link::NO_WIFI) {
        if (g_st.tx == Tx::REQUEST || g_st.tx == Tx::CHIRP || g_st.tx == Tx::TALK) {
          stop_tx();
          g_st.tx = s_ptt ? Tx::WAIT_RELEASE : Tx::IDLE;
        }
        g_st.link = Link::NO_WIFI;
        if (s_auto) s_hub_set = false;
      }
      bool down;
      while (xQueueReceive(s_ui_q, &down, 0) == pdTRUE) on_ptt(down);
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (s_sock < 0 && !open_socket()) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    uint32_t now = millis();
    if (g_st.link == Link::NO_WIFI) {       // Wi-Fi только что появился
      g_st.link = Link::NO_HUB;
      s_reconfigure = true;
    }
    if (s_reconfigure) {
      s_reconfigure = false;
      if (g_st.link == Link::OK || g_st.link == Link::WRONG_KEY || g_st.link == Link::NO_KEY) g_st.link = Link::NO_HUB;
      s_bad_before_ack = 0;
      s_hub_set = false;
      resolve_hub();
      s_hello_ms = 0;
    } else if (!s_auto && ((g_st.link != Link::OK && now - s_resolve_ms > 15000) || now - s_resolve_ms > 600000)) {
      resolve_hub();                        // имя моста могло сменить адрес
    }
    if (g_st.link == Link::NO_KEY) {
      vTaskDelay(pdMS_TO_TICKS(200));
      if (crypto_has_key()) g_st.link = Link::NO_HUB;
      continue;
    }

    eco_update(now);
    uint32_t every = g_st.link == Link::OK ? 2000 : 1000;
    if (now - s_hello_ms >= every) send_hello();

    poll_rx(5);

    bool down;
    while (xQueueReceive(s_ui_q, &down, 0) == pdTRUE) on_ptt(down);

    now = millis();
    if (s_end_left && (int32_t)(now - s_end_next) >= 0) {
      send_burst_pkt(wt::T_TALK_END, s_end_burst);
      s_end_left--;
      s_end_next = now + 30;
    }
    switch (g_st.tx) {
      case Tx::REQUEST:
        if (now - s_req_start > 700) {      // мост молчит
          audio_tone(Tone::ERROR);
          flash(Flash::ERROR);
          schedule_end();
          g_st.tx = s_ptt ? Tx::WAIT_RELEASE : Tx::IDLE;
        } else if (now - s_req_ms > 150) {
          send_req();
        }
        break;
      case Tx::CHIRP:
        if ((int32_t)(now - s_chirp_end) >= 0) {
          g_st.tx = Tx::TALK;
          s_talk_start = now;
          audio_set_streaming(true);
        }
        break;
      case Tx::TALK: {
        TxFrame f;
        while (audio_tx_pop(f)) send_audio(f);
        if (now - s_talk_start > TALK_LIMIT_MS) {
          stop_tx();
          schedule_end();
          audio_tone(Tone::ERROR);
          g_st.tx = Tx::WAIT_RELEASE;
        }
        break;
      }
      default: break;
    }

    if (g_st.link == Link::OK && now - s_ack_ms > 6500) {
      logf("мост не отвечает");
      g_st.link = Link::NO_HUB;
      s_lost_ms = now;
      if (s_auto) s_hub_set = false;
      if (g_st.tx == Tx::REQUEST || g_st.tx == Tx::CHIRP || g_st.tx == Tx::TALK) {
        stop_tx();
        g_st.tx = s_ptt ? Tx::WAIT_RELEASE : Tx::IDLE;
      }
      audio_tone(Tone::LOST);
      flash(Flash::ERROR);
    }
    if (g_st.link == Link::NO_HUB && s_bad_before_ack >= 3) {
      g_st.link = Link::WRONG_KEY;
      strlcpy(g_st.last_error, "мост отвечает, но ключ сети не совпадает", sizeof(g_st.last_error));
      logf("%s", g_st.last_error);
    }

    char adv[17];
    for (int i = 0; i < 8; i++) snprintf(adv + 2 * i, 3, "%02x", s_fw_sha[i]);
    if (g_st.link == Link::OK && s_fw_ver > FW_VERSION && s_fw_size > 0 && strcmp(adv, s_ota_done) != 0 &&
        g_st.tx == Tx::IDLE && !g_st.rx_active &&
        !g_st.updating && now - g_st.last_activity_ms > 20000 && (!s_ota_fail_ms || now - s_ota_fail_ms > 3600000)) {
      ota_from_hub();
    }
  }
}

void net_begin() {
  Preferences p;
  p.begin("walkie", true);
  strlcpy(s_ota_done, p.getString("otasha", "").c_str(), sizeof(s_ota_done));
  p.end();
  s_ui_q = xQueueCreate(8, sizeof(bool));
  xTaskCreatePinnedToCore(net_task, "net", 8192, nullptr, 5, nullptr, 0);
}
