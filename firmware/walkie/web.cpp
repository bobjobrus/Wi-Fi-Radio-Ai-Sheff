#include "web.h"
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include "audio.h"
#include "config.h"
#include "crypto.h"
#include "net.h"
#include "settings.h"
#include "state.h"
#include "wifi_mgr.h"

static WebServer s_http(80);
static uint32_t s_reboot_at = 0;
static bool s_scan_started = false;

static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Рация</title><style>
:root{--bg:#f3f3f1;--card:#fff;--ink:#1b1b1b;--mute:#6d6d6d;--line:#e6e6e6;--acc:#1a73e8;--ok:#137a2f;--bad:#b3261e}
@media (prefers-color-scheme:dark){:root{--bg:#141414;--card:#202020;--ink:#ececec;--mute:#9a9a9a;--line:#333;--acc:#8ab4f8;--ok:#7dd98f;--bad:#ff8a80}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
main{max-width:560px;margin:0 auto;padding:14px 16px 40px}h1{font-size:22px;margin:6px 0 2px}
.sub{color:var(--mute);font-size:14px;margin-bottom:10px}section{background:var(--card);border-radius:14px;padding:14px;margin:12px 0}
h2{font-size:16px;margin:0 0 10px}label{display:block;font-size:14px;color:var(--mute);margin:10px 0 4px}
input,select{width:100%;font:inherit;padding:9px 10px;border:1px solid var(--line);border-radius:10px;background:transparent;color:inherit}
input[type=checkbox]{width:auto;margin-right:8px}input[type=range]{padding:0}.row{display:flex;gap:8px;align-items:center}
button{font:inherit;padding:10px 14px;border:0;border-radius:10px;background:var(--acc);color:#fff;cursor:pointer}
button.sec{background:transparent;color:var(--acc);border:1px solid var(--line)}button.del{background:transparent;color:var(--bad);padding:4px 8px}
table{width:100%;border-collapse:collapse;font-size:15px}td{padding:5px 0;border-bottom:1px solid var(--line)}td:last-child{text-align:right}
.pill{display:inline-block;padding:2px 10px;border-radius:99px;font-size:14px}.ok{background:#e3f4e6;color:var(--ok)}.bad{background:#fbe4e1;color:var(--bad)}
@media (prefers-color-scheme:dark){.ok{background:#1e3a24}.bad{background:#46201d}}
.net{padding:8px 0;border-bottom:1px solid var(--line);cursor:pointer}.hint{font-size:13px;color:var(--mute)}#msg{position:fixed;left:16px;right:16px;bottom:16px;
background:#222;color:#fff;padding:12px;border-radius:10px;display:none;text-align:center}
</style></head><body><main>
<h1 id="title">Рация</h1><div class="sub" id="subtitle"></div>
<section><h2>Состояние <span id="pill" class="pill"></span></h2><table id="st"></table></section>
<section><h2>Wi-Fi</h2><div id="known"></div>
<label>Сеть</label><div class="row"><input id="ssid" placeholder="имя сети"><button class="sec" onclick="scan()">Найти</button></div>
<div id="nets"></div><label>Пароль</label><input id="pass" type="password" placeholder="пароль сети">
<p><button onclick="addWifi()">Подключиться к этой сети</button></p>
<div class="hint">Можно запомнить до трёх сетей (например, дом и магазин) — рация выберет ту, что ловит лучше.</div></section>
<section><h2>Настройки</h2>
<label>Имя рации (так её видно на мосту)</label><input id="name" maxlength="31">
<label>Адрес моста (пусто — искать в своей сети)</label><input id="server" placeholder="например 203.0.113.10 или radio.example.ru">
<label>Ключ сети <span id="keyset" class="hint"></span></label><input id="netkey" placeholder="XXXX-XXXX-XXXX-XXXX" autocomplete="off">
<label>Громкость: <b id="volv"></b></label><input id="volume" type="range" min="0" max="20" oninput="volv.textContent=this.value">
<label>Яркость подсветки: <b id="ledv"></b></label><input id="led" type="range" min="0" max="100" oninput="ledv.textContent=this.value">
<label><input type="checkbox" id="agc">Автоусиление микрофона</label>
<label>Усиление, если автоусиление выключено, дБ</label><input id="mic_gain" type="number" min="0" max="42">
<label><input type="checkbox" id="roger">Сигнал в конце чужой передачи</label>
<label><input type="checkbox" id="mic_right">Микрофон на правом канале (вывод L/R на 3,3 В)</label>
<label><input type="checkbox" id="enc_invert">Регулятор крутится наоборот</label>
<label><input type="checkbox" id="eco">Экономия аккумулятора (Wi-Fi дремлет, пока в эфире тихо). Осторожно: модуль заряда может принять это за «нагрузку отключили» и выключить рацию</label>
<p><button onclick="save()">Сохранить</button></p></section>
<section><h2>Прошивка</h2><div class="hint" id="fw"></div>
<p><input type="file" id="file" accept=".bin"></p><p><button class="sec" onclick="upload()">Загрузить прошивку</button>
<button class="sec" onclick="post('/api/reboot',{}).then(()=>toast('Перезапуск…'))">Перезапустить</button></p></section>
</main><div id="msg"></div><script>
const $=id=>document.getElementById(id);let loaded=false;
function toast(t){const m=$('msg');m.textContent=t;m.style.display='block';clearTimeout(m._t);m._t=setTimeout(()=>m.style.display='none',3500)}
function post(u,d){return fetch(u,{method:'POST',body:new URLSearchParams(d)}).then(r=>r.text())}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
const LINK={NO_WIFI:['нет Wi-Fi','bad'],NO_KEY:['нет ключа сети','bad'],NO_HUB:['ищу мост','bad'],WRONG_KEY:['не тот ключ сети','bad'],OK:['на связи','ok']};
function load(){fetch('/api/status').then(r=>r.json()).then(s=>{
$('title').textContent='Рация «'+s.name+'»';$('subtitle').textContent=s.host+'.local · id '+s.id;
const l=LINK[s.link]||[s.link,'bad'];$('pill').textContent=l[0];$('pill').className='pill '+l[1];
const rows=[['Аккумулятор',s.bat==255?(s.bat_measuring?'измеряется…':'нет (питание от USB)'):s.bat+'% · '+(s.bat_mv/1000).toFixed(2)+' В'],['Wi-Fi',s.ssid?esc(s.ssid)+' ('+s.rssi+' дБм)':'—'],['Адрес рации',s.ip||'—'],['Мост',esc(s.hub_name||'—')+' <span class=hint>'+esc(s.hub_addr||'')+'</span>'],
['Задержка до моста',s.link=='OK'?s.rtt+' мс':'—'],['Раций в сети',s.link=='OK'?s.radios:'—'],['Другие мосты',s.link!='OK'?'—':(s.peers_ok?'все на связи':'не все на связи')],
['Приём: сыграно / потеряно',s.rx_played+' / '+s.rx_lost],['Чужие пакеты',s.bad],
['Работает',Math.floor(s.uptime/3600)+' ч '+Math.floor(s.uptime%3600/60)+' мин · запуск: '+esc(s.boot)],
['Свист рядом с другой рацией',s.howl?('гасили '+s.howl+' раз, последний на '+s.howl_hz+' Гц'):'не было']];
if(s.error)rows.push(['Ошибка','<span style="color:var(--bad)">'+esc(s.error)+'</span>']);
$('st').innerHTML=rows.map(r=>'<tr><td>'+r[0]+'</td><td>'+r[1]+'</td></tr>').join('');
$('known').innerHTML=s.wifi.length?s.wifi.map((w,i)=>'<div class="row net"><span style="flex:1">'+esc(w)+'</span><button class="del" onclick="delWifi('+i+')">убрать</button></div>').join(''):'<div class="hint">Сетей пока нет.</div>';
$('fw').textContent='Версия '+s.fw+(s.updating?' · обновляется: '+s.update_pct+'%':'')+'. Обычно обновляется сама с моста.';
if(!loaded){loaded=true;for(const k of['name','server','volume','led','mic_gain'])$(k).value=s[k];
for(const k of['agc','roger','mic_right','enc_invert','eco'])$(k).checked=s[k];$('volv').textContent=s.volume;$('ledv').textContent=s.led;
$('keyset').textContent=s.key_set?'(задан; оставьте пустым, чтобы не менять)':'(не задан)';}
}).catch(()=>{})}
function scan(){$('nets').innerHTML='<div class=hint>Ищу сети…</div>';const go=()=>fetch('/api/scan').then(r=>r.json()).then(j=>{
if(j.scanning){setTimeout(go,1200);return}$('nets').innerHTML=j.nets.map(n=>'<div class="net" data-s="'+esc(n.ssid)+'">'+esc(n.ssid)+' <span class=hint>'+n.rssi+' дБм'+(n.open?', без пароля':'')+'</span></div>').join('')||'<div class=hint>Ничего не нашлось</div>';
document.querySelectorAll('#nets .net').forEach(d=>d.onclick=()=>{$('ssid').value=d.dataset.s;$('pass').focus()})});go()}
function addWifi(){const ss=$('ssid').value.trim();if(!ss){toast('Выберите сеть');return}
post('/api/wifi',{ssid:ss,pass:$('pass').value}).then(t=>{toast(t);setTimeout(load,1500)})}
function delWifi(i){post('/api/wifi/del',{i}).then(t=>{toast(t);load()})}
function save(){const d={};for(const k of['name','server','netkey','volume','led','mic_gain'])d[k]=$(k).value;
for(const k of['agc','roger','mic_right','enc_invert','eco'])d[k]=$(k).checked?1:0;post('/api/settings',d).then(t=>{toast(t);$('netkey').value='';loaded=false;load()})}
function upload(){const f=$('file').files[0];if(!f){toast('Выберите файл .bin');return}const fd=new FormData();fd.append('fw',f,f.name);
toast('Загружаю… не выключайте рацию');fetch('/update',{method:'POST',body:fd}).then(r=>r.text()).then(toast)}
load();setInterval(load,2000);
</script></body></html>)HTML";

static String jesc(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') {
      o += '\\';
      o += c;
    } else if ((uint8_t)c < 0x20) {
      char b[8];
      snprintf(b, sizeof(b), "\\u%04x", c);
      o += b;
    } else {
      o += c;
    }
  }
  return o;
}

static const char* link_name(Link l) {
  switch (l) {
    case Link::NO_WIFI: return "NO_WIFI";
    case Link::NO_KEY: return "NO_KEY";
    case Link::NO_HUB: return "NO_HUB";
    case Link::WRONG_KEY: return "WRONG_KEY";
    case Link::OK: return "OK";
  }
  return "?";
}

static void handle_root() { s_http.send_P(200, "text/html; charset=utf-8", PAGE); }

static void handle_status() {
  String j;
  j.reserve(900);
  char id[12];
  snprintf(id, sizeof(id), "%08X", (unsigned)g_st.id);
  bool conn = WiFi.status() == WL_CONNECTED;
  settings_lock();
  j += "{\"name\":\"" + jesc(g_set.name) + "\",\"id\":\"" + id + "\",\"host\":\"" + g_st.host + "\"";
  j += ",\"link\":\"" + String(link_name(g_st.link)) + "\"";
  j += ",\"ssid\":\"" + (conn ? jesc(WiFi.SSID()) : String("")) + "\",\"rssi\":" + String(conn ? WiFi.RSSI() : 0);
  j += ",\"ip\":\"" + (conn ? WiFi.localIP().toString() : String("")) + "\"";
  j += ",\"hub_name\":\"" + jesc(g_st.hub_name) + "\",\"hub_addr\":\"" + jesc(g_st.hub_addr) + "\"";
  j += ",\"rtt\":" + String(g_st.rtt_ms) + ",\"radios\":" + String(g_st.radios_online);
  j += ",\"peers_ok\":" + String(g_st.peers_ok ? "true" : "false");
  j += ",\"rx_played\":" + String(g_st.rx_played) + ",\"rx_lost\":" + String(g_st.rx_lost);
  j += ",\"bad\":" + String(g_st.bad_packets) + ",\"error\":\"" + jesc(g_st.last_error) + "\"";
  j += ",\"fw\":" + String(FW_VERSION) + ",\"updating\":" + String(g_st.updating ? "true" : "false");
  j += ",\"update_pct\":" + String(g_st.update_pct);
  j += ",\"boot\":\"" + String(g_st.boot_reason) + "\",\"uptime\":" + String(millis() / 1000);
  uint32_t howl_n;
  uint16_t howl_hz;
  audio_howl_info(howl_n, howl_hz);
  j += ",\"howl\":" + String(howl_n) + ",\"howl_hz\":" + String(howl_hz);
  j += ",\"bat\":" + String(g_st.bat_pct) + ",\"bat_mv\":" + String(g_st.bat_mv) + ",\"bat_raw\":" + String(g_st.bat_raw_mv) +
       ",\"bat_measuring\":" + String(g_st.bat_measuring ? "true" : "false");
  j += ",\"amp_pdm\":" + String(g_set.amp_pdm ? "true" : "false");
  j += ",\"eco\":" + String(g_set.eco ? "true" : "false") + ",\"eco_now\":" + String(g_st.eco ? "true" : "false");
  j += ",\"server\":\"" + jesc(g_set.server) + "\",\"key_set\":" + String(g_set.netkey[0] ? "true" : "false");
  j += ",\"volume\":" + String(g_set.volume) + ",\"led\":" + String(g_set.led);
  j += ",\"mic_gain\":" + String(g_set.mic_gain);
  j += ",\"agc\":" + String(g_set.agc ? "true" : "false") + ",\"roger\":" + String(g_set.roger ? "true" : "false");
  j += ",\"mic_right\":" + String(g_set.mic_right ? "true" : "false");
  j += ",\"enc_invert\":" + String(g_set.enc_invert ? "true" : "false");
  j += ",\"wifi\":[";
  bool first = true;
  for (int i = 0; i < WIFI_SLOTS; i++) {
    if (!g_set.wifi[i].ssid[0]) continue;
    if (!first) j += ",";
    first = false;
    j += "\"" + jesc(g_set.wifi[i].ssid) + "\"";
  }
  j += "]}";
  settings_unlock();
  s_http.send(200, "application/json; charset=utf-8", j);
}

static void handle_scan() {
  int r = WiFi.scanComplete();
  if (!s_scan_started || r == WIFI_SCAN_FAILED) {
    WiFi.scanNetworks(true);
    s_scan_started = true;
    s_http.send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  if (r == WIFI_SCAN_RUNNING) {
    s_http.send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  String j = "{\"nets\":[";
  bool first = true;
  for (int i = 0; i < r; i++) {
    String ss = WiFi.SSID(i);
    if (!ss.length()) continue;
    bool dup = false;              // одна сеть с нескольких точек — показать один раз
    for (int k = 0; k < i; k++)
      if (WiFi.SSID(k) == ss) dup = true;
    if (dup) continue;
    if (!first) j += ",";
    first = false;
    j += "{\"ssid\":\"" + jesc(ss) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
         ",\"open\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  j += "]}";
  WiFi.scanDelete();
  s_scan_started = false;
  s_http.send(200, "application/json; charset=utf-8", j);
}

static void handle_wifi_add() {
  String ssid = s_http.arg("ssid"), pass = s_http.arg("pass");
  if (!wifi_add(ssid.c_str(), pass.c_str())) {
    s_http.send(400, "text/plain; charset=utf-8", "Не получилось: проверьте имя сети");
    return;
  }
  logf("добавлена сеть «%s»", ssid.c_str());
  // точка открыта — перезапуск: подключение «на ходу» рация сама откладывает, пока к точке подключён телефон
  if (wifi_portal_active()) {
    s_http.send(200, "text/plain; charset=utf-8", "Сеть запомнена. Рация перезапускается и подключится к ней.");
    s_reboot_at = millis() + 1500;
  } else {
    s_http.send(200, "text/plain; charset=utf-8", "Сеть запомнена, подключаюсь…");
    wifi_reconnect();
  }
}

static void handle_wifi_del() {
  wifi_remove(s_http.arg("i").toInt());
  s_http.send(200, "text/plain; charset=utf-8", "Сеть убрана");
}

static void handle_settings() {
  bool net_changed = false;
  settings_lock();
  if (s_http.hasArg("name")) {
    String v = s_http.arg("name");
    v.trim();
    if (v.length()) strlcpy(g_set.name, v.c_str(), sizeof(g_set.name));
  }
  if (s_http.hasArg("server")) {
    String v = s_http.arg("server");
    v.trim();
    if (v != g_set.server) net_changed = true;
    strlcpy(g_set.server, v.c_str(), sizeof(g_set.server));
  }
  String key = s_http.arg("netkey");
  key.trim();
  bool key_bad = false;
  if (key.length()) {
    if (crypto_key_valid(key.c_str())) {       // сам ключ включит задача сети (net_server_changed)
      strlcpy(g_set.netkey, key.c_str(), sizeof(g_set.netkey));
      net_changed = true;
    } else {
      key_bad = true;
    }
  }
  if (s_http.hasArg("volume")) g_set.volume = constrain(s_http.arg("volume").toInt(), 0, 20);
  if (s_http.hasArg("led")) g_set.led = constrain(s_http.arg("led").toInt(), 0, 100);
  if (s_http.hasArg("mic_gain")) g_set.mic_gain = constrain(s_http.arg("mic_gain").toInt(), 0, 42);
  if (s_http.hasArg("agc")) g_set.agc = s_http.arg("agc") == "1";
  if (s_http.hasArg("roger")) g_set.roger = s_http.arg("roger") == "1";
  if (s_http.hasArg("mic_right")) g_set.mic_right = s_http.arg("mic_right") == "1";
  if (s_http.hasArg("enc_invert")) g_set.enc_invert = s_http.arg("enc_invert") == "1";
  if (s_http.hasArg("eco")) g_set.eco = s_http.arg("eco") == "1";
  bool amp_changed = false;
  if (s_http.hasArg("amp_pdm")) {
    bool v = s_http.arg("amp_pdm") == "1";
    amp_changed = v != g_set.amp_pdm;
    g_set.amp_pdm = v;
  }
  settings_save();
  settings_unlock();
  if (net_changed) net_server_changed();
  if (amp_changed) s_reboot_at = millis() + 1200;     // звук перенастраивается только с перезапуска
  s_http.send(200, "text/plain; charset=utf-8",
              key_bad ? "Сохранено, но ключ не принят: нужно не меньше 12 букв и цифр"
                      : (amp_changed ? "Сохранено, рация перезапускается" : "Сохранено"));
}

static void handle_update_done() {
  bool ok = !Update.hasError();
  s_http.send(200, "text/plain; charset=utf-8", ok ? "Прошивка записана, перезапуск…" : "Ошибка записи прошивки");
  if (ok) s_reboot_at = millis() + 1000;
  g_st.updating = false;
}

static void handle_update_upload() {
  HTTPUpload& up = s_http.upload();
  if (up.status == UPLOAD_FILE_START) {
    logf("загрузка прошивки: %s", up.filename.c_str());
    g_st.updating = true;
    g_st.update_pct = 0;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) logf("Update.begin: %s", Update.errorString());
  } else if (up.status == UPLOAD_FILE_WRITE) {
    esp_task_wdt_reset();               // загрузка прошивки со страницы идёт внутри одного прохода цикла
    if (Update.write(up.buf, up.currentSize) != up.currentSize) logf("Update.write: %s", Update.errorString());
    g_st.update_pct = (uint8_t)min((size_t)99, Update.progress() * 100 / 1200000);
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true))
      logf("прошивка загружена: %u байт", up.totalSize);
    else
      logf("Update.end: %s", Update.errorString());
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    g_st.updating = false;
  }
}

static void handle_not_found() {
  if (wifi_portal_active()) {       // «страница входа в сеть» на телефоне → сразу настройки рации
    s_http.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
    s_http.send(302, "text/plain", "");
    return;
  }
  s_http.send(404, "text/plain", "not found");
}

void web_begin() {
  s_http.on("/", HTTP_GET, handle_root);
  s_http.on("/api/status", HTTP_GET, handle_status);
  s_http.on("/api/scan", HTTP_GET, handle_scan);
  s_http.on("/api/wifi", HTTP_POST, handle_wifi_add);
  s_http.on("/api/wifi/del", HTTP_POST, handle_wifi_del);
  s_http.on("/api/settings", HTTP_POST, handle_settings);
  s_http.on("/api/reboot", HTTP_POST, []() {
    s_http.send(200, "text/plain; charset=utf-8", "Перезапуск");
    s_reboot_at = millis() + 500;
  });
  s_http.on("/update", HTTP_POST, handle_update_done, handle_update_upload);
  s_http.onNotFound(handle_not_found);
  s_http.begin();
}

void web_loop() {
  s_http.handleClient();
  if (s_reboot_at && (int32_t)(millis() - s_reboot_at) >= 0) {
    delay(100);
    ESP.restart();
  }
}
