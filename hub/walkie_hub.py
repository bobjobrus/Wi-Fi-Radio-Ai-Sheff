#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Мост WiFi-раций Ай-Шефф.

Рации (ESP32) шлют сюда пакеты по UDP. Мост решает, кто сейчас говорит («эфир»),
и рассылает его речь всем остальным рациям — своим и тем, что висят на других мостах
(поле peers, например дом ↔ магазин через Tailscale).

Только стандартная библиотека Python 3.8+. Одинаково работает на Windows, Linux, macOS.

    python walkie_hub.py --config hub.json      запуск
    python walkie_hub.py --genkey               выдумать новый ключ сети

Страница состояния: http://<адрес моста>:47080/  (если задан status_token — ?t=<токен>)
"""
import argparse
import collections
import hashlib
import html
import json
import logging
import logging.handlers
import os
import select
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wt_proto as P  # noqa: E402

HUB_VERSION = "1.0"

RADIO_OFFLINE_S = 7.0      # нет HELLO дольше — рация «не в сети»
PEER_OFFLINE_S = 7.0
PEER_HELLO_EVERY_S = 2.0
FLOOR_IDLE_S = 0.8         # нет речи дольше — эфир свободен
DENY_EVERY_S = 0.2         # не чаще — повторные отказы одной рации
FW_RESCAN_S = 10.0

log = logging.getLogger("hub")


def now():
    return time.monotonic()


def ms32():
    return int(time.monotonic() * 1000) & 0xFFFFFFFF


def parse_addr(text, default_port):
    text = text.strip()
    if ":" in text:
        host, port = text.rsplit(":", 1)
        return host.strip(), int(port)
    return text, default_port


class Radio:
    def __init__(self, rid, addr):
        self.id = rid
        self.addr = addr
        self.name = "рация %08X" % rid
        self.fw = 0
        self.flags = 0
        self.rssi = 0
        self.rtt = 0
        self.battery = 255
        self.low_reported = False
        self.last = 0.0
        self.first = now()
        self.last_deny = 0.0
        self.bad_level_bursts = 0
        self.reported_offline = True

    def online(self, t):
        return t - self.last < RADIO_OFFLINE_S


class Peer:
    def __init__(self, host, port, configured):
        self.host = host
        self.port = port
        self.configured = configured
        self.addr = None           # (ip, port) после разрешения имени
        self.last = 0.0
        self.name = host
        self.radios = []           # [(id, flags, name)]
        self.rtt = None
        self.last_t_from_peer = 0  # их t_ms для эха
        self.last_resolve = -1e9
        self.reported_down = True

    def alive(self, t):
        return self.addr is not None and t - self.last < PEER_OFFLINE_S


class Floor:
    """Кто сейчас в эфире."""

    def __init__(self, talker, burst, origin, t):
        self.talker = talker       # id рации
        self.burst = burst         # номер передачи
        self.origin = origin       # "local" или Peer
        self.started = t
        self.last = t
        self.max_level = 0
        self.frames = 0


class Hub:
    def __init__(self, cfg, cfg_dir):
        self.cfg = cfg
        self.name = cfg.get("name", "Мост")
        self.port = int(cfg.get("port", P.DEFAULT_PORT))
        self.http_port = int(cfg.get("http_port", P.DEFAULT_HTTP_PORT))
        self.k_auth, _ = P.derive_keys(cfg["key"])
        self.tot = float(cfg.get("tot_seconds", 90))
        self.hub_id = int.from_bytes(hashlib.sha256(("hub:" + self.name).encode()).digest()[:4], "little")
        self.status_token = cfg.get("status_token", "")
        self.fw_dir = (cfg_dir / cfg.get("firmware_dir", "firmware")).resolve()
        self.radios = {}           # id → Radio
        self.peers = []
        for text in cfg.get("peers", []):
            host, port = parse_addr(text, P.DEFAULT_PORT)
            self.peers.append(Peer(host, port, True))
        self.floor = None
        self.events = collections.deque(maxlen=200)
        self.lock = threading.Lock()
        self.bad_packets = collections.Counter()     # адрес → сколько пакетов с неверной подписью
        self.bad_key_sent = {}
        self.fw = {"version": 0, "size": 0, "sha": b"", "path": None, "mtime": 0}
        self.last_fw_scan = 0.0
        self.last_peer_hello = 0.0
        self.last_tick = 0.0
        self.started = time.time()
        self.stats = collections.Counter()

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((cfg.get("listen", "0.0.0.0"), self.port))
        self.sock.setblocking(False)

    # ─────────────── служебное ───────────────

    def event(self, text):
        stamp = time.strftime("%d.%m %H:%M:%S")
        with self.lock:
            self.events.appendleft("%s  %s" % (stamp, text))
        log.info(text)

    def send(self, data, addr):
        try:
            self.sock.sendto(data, addr)
            self.stats["tx"] += 1
        except OSError as e:
            self.stats["tx_err"] += 1
            log.debug("sendto %s: %s", addr, e)

    def build(self, ptype, src, payload=b""):
        return P.build(self.k_auth, ptype, src, payload)

    def radio_name(self, rid):
        r = self.radios.get(rid)
        if r:
            return r.name
        for p in self.peers:
            for pid, _, pname in p.radios:
                if pid == rid:
                    return "%s (%s)" % (pname, p.name)
        return "%08X" % rid

    def peer_by_addr(self, addr):
        for p in self.peers:
            if p.addr == addr:
                return p
        return None

    def alive_peers(self, t):
        return [p for p in self.peers if p.alive(t)]

    def local_online(self, t):
        return [r for r in self.radios.values() if r.online(t)]

    # ─────────────── прошивка для раций ───────────────

    def scan_firmware(self):
        """firmware/walkie.bin + firmware/walkie.json {"version": N} → раздаём рациям."""
        binf = self.fw_dir / "walkie.bin"
        meta = self.fw_dir / "walkie.json"
        try:
            st = binf.stat()
            if st.st_mtime == self.fw["mtime"] and self.fw["path"]:
                return
            version = int(json.loads(meta.read_text(encoding="utf-8-sig"))["version"])
            data = binf.read_bytes()
            self.fw = {"version": version, "size": len(data), "sha": hashlib.sha256(data).digest(),
                       "path": binf, "mtime": st.st_mtime}
            self.event("прошивка для раций: версия %d, %d КБ" % (version, len(data) // 1024))
        except FileNotFoundError:
            self.fw = {"version": 0, "size": 0, "sha": b"", "path": None, "mtime": 0}
        except Exception as e:  # битый json и т. п.
            log.warning("прошивка не прочитана: %s", e)

    # ─────────────── эфир ───────────────

    def floor_expired(self, t):
        return self.floor is None or t - self.floor.last > FLOOR_IDLE_S

    def release_floor(self, why, notify_local=True, notify_peers=None):
        f = self.floor
        if f is None:
            return
        self.floor = None
        dur = now() - f.started
        who = self.radio_name(f.talker)
        where = "здесь" if f.origin == "local" else "через %s" % f.origin.name
        self.event("эфир: %s закончил%s (%.1f с, %s)" % (who, "" if why == "end" else " — " + why, dur, where))
        if f.origin == "local" and f.frames > 25 and f.max_level < 3:
            r = self.radios.get(f.talker)
            if r:
                r.bad_level_bursts += 1
                self.event("⚠ %s: в передаче одна тишина — проверьте микрофон" % who)
        end = self.build(P.T_TALK_END, f.talker, struct.pack("<I", f.burst))
        if notify_local:
            t = now()
            for r in self.local_online(t):
                if r.id != f.talker:
                    self.send(end, r.addr)
        if notify_peers is None:
            notify_peers = f.origin == "local"
        if notify_peers:
            for p in self.alive_peers(now()):
                self.send(end, p.addr)

    def grant_local(self, r, burst, t):
        self.floor = Floor(r.id, burst, "local", t)
        self.send(self.build(P.T_TALK_GRANT, self.hub_id, struct.pack("<I", burst)), r.addr)
        start = self.build(P.T_TALK_START, r.id, struct.pack("<I", burst))
        for p in self.alive_peers(t):
            self.send(start, p.addr)
        self.event("эфир: говорит %s" % r.name)

    def deny(self, r, burst, reason, t, force=False):
        if not force and t - r.last_deny < DENY_EVERY_S:
            return
        r.last_deny = t
        self.send(self.build(P.T_TALK_DENY, self.hub_id, struct.pack("<IB", burst, reason)), r.addr)

    def claim_from_peer(self, peer, talker, burst, t):
        """Пришла речь/начало передачи с другого моста. True — эфир за этим говорящим."""
        f = self.floor
        if f is not None and not self.floor_expired(t):
            if f.talker == talker:
                if f.burst != burst:        # тот же человек нажал заново
                    f.burst = burst
                    f.started = t
                f.origin = peer
                return True
            # одновременно нажали в двух местах: побеждает меньший id — одинаково на всех мостах
            if talker < f.talker:
                loser = f.talker
                if f.origin == "local":
                    r = self.radios.get(loser)
                    if r:
                        self.deny(r, f.burst, P.DENY_REVOKED, t, force=True)
                    # рациям, что уже слышали проигравшего, — конец его передачи
                    self.release_floor("уступил %s" % self.radio_name(talker), notify_local=True,
                                       notify_peers=False)
                else:
                    self.floor = None
            else:
                return False
        self.floor = Floor(talker, burst, peer, t)
        self.event("эфир: говорит %s" % self.radio_name(talker))
        return True

    # ─────────────── приём ───────────────

    def on_packet(self, data, addr):
        t = now()
        parsed = P.parse(self.k_auth, data)
        if parsed is None:
            self.stats["bad"] += 1
            if data[:2] == P.MAGIC and len(data) > P.HDR.size:
                with self.lock:
                    self.bad_packets[addr[0]] += 1
                    first = self.bad_packets[addr[0]] == 1
                if first:
                    self.event("⚠ пакет раций с неверной подписью от %s — не тот ключ сети?" % addr[0])
                # подсказать рации, что дело в ключе (не чаще раза в 5 с на адрес)
                if t - self.bad_key_sent.get(addr, -1e9) > 5.0:
                    self.bad_key_sent[addr] = t
                    self.send(P.HDR.pack(P.MAGIC, P.VERSION, P.T_BAD_KEY, self.hub_id), addr)
            return
        ptype, src, pl = parsed
        self.stats["rx"] += 1
        peer = self.peer_by_addr(addr)
        if ptype == P.T_PEER_HELLO:
            self.on_peer_hello(src, pl, addr, t)
            return
        if peer is not None:
            self.on_from_peer(peer, ptype, src, pl, t)
        else:
            self.on_from_radio(ptype, src, pl, addr, t)

    def on_from_radio(self, ptype, src, pl, addr, t):
        r = self.radios.get(src)
        if ptype == P.T_HELLO and src == P.PROBE_ID:
            h = P.parse_hello(pl)
            if h is not None:                 # самопроверка: ответить и забыть
                self.stats["probe"] += 1
                self.reply_ack(Radio(src, addr), h["t_ms"], t)
            return
        if ptype == P.T_HELLO:
            h = P.parse_hello(pl)
            if h is None:
                return
            if r is None:
                r = self.radios[src] = Radio(src, addr)
            was_online = not r.reported_offline
            if r.addr[0] != addr[0] and was_online:
                self.event("%s сменила адрес: %s → %s" % (r.name, r.addr[0], addr[0]))
            r.addr = addr
            r.last = t
            r.name = h["name"] or r.name
            r.fw, r.flags, r.rssi, r.rtt, r.battery = h["fw"], h["flags"], h["rssi"], h["rtt"], h["battery"]
            if r.battery <= 15 and not r.low_reported:
                r.low_reported = True
                self.event("🔋 %s: аккумулятор %d%% — пора на зарядку" % (r.name, r.battery))
            elif r.battery != 255 and r.battery >= 30:
                r.low_reported = False
            if not was_online:
                r.reported_offline = False
                self.event("в сети: %s (%s, прошивка %d)" % (r.name, addr[0], r.fw))
            self.reply_ack(r, h["t_ms"], t)
            return
        if r is None:
            # рация не представилась (мост перезапускался) — просим HELLO отказом
            if ptype == P.T_TALK_REQ and len(pl) >= 4:
                tmp = Radio(src, addr)
                self.deny(tmp, struct.unpack_from("<I", pl)[0], P.DENY_UNKNOWN, t, force=True)
            return
        r.addr = addr
        if not r.reported_offline:
            r.last = t                  # любой подписанный пакет — признак жизни
        if ptype == P.T_TALK_REQ and len(pl) >= 4:
            burst = struct.unpack_from("<I", pl)[0]
            f = self.floor
            if f is not None and f.talker == src and f.burst == burst:
                self.send(self.build(P.T_TALK_GRANT, self.hub_id, pl[:4]), addr)   # повтор запроса
            elif self.floor_expired(t) or (f.talker == src and f.origin == "local"):
                if f is not None:
                    self.release_floor("новое нажатие" if f.talker == src else "тишина")
                self.grant_local(r, burst, t)
            else:
                self.deny(r, burst, P.DENY_BUSY, t)
        elif ptype == P.T_AUDIO:
            a = P.parse_audio(pl)
            if a is None:
                return
            f = self.floor
            if f is None or f.talker != src or f.burst != a["burst"] or f.origin != "local":
                self.deny(r, a["burst"], P.DENY_REVOKED, t)
                return
            if t - f.started > self.tot:
                self.deny(r, a["burst"], P.DENY_TIMEOUT, t, force=True)
                self.release_floor("держал кнопку дольше %d с" % self.tot)
                return
            f.last = t
            f.frames += 1
            f.max_level = max(f.max_level, a["level"])
            raw = P.build(self.k_auth, P.T_AUDIO, src, pl)   # пересобрать = та же подпись
            for o in self.local_online(t):
                if o.id != src:
                    self.send(raw, o.addr)
            for p in self.alive_peers(t):
                self.send(raw, p.addr)
            self.stats["audio"] += 1
        elif ptype == P.T_TALK_END and len(pl) >= 4:
            burst = struct.unpack_from("<I", pl)[0]
            f = self.floor
            if f is not None and f.talker == src and f.burst == burst:
                self.release_floor("end")

    def reply_ack(self, r, t_echo, t):
        fw = self.fw
        pl = P.ack_payload(
            t_echo, self.count_online(t), len(self.peers), len(self.alive_peers(t)),
            0 if self.floor_expired(t) else 1,
            fw["version"], fw["size"], fw["sha"], self.http_port, self.name)
        self.send(self.build(P.T_HELLO_ACK, self.hub_id, pl), r.addr)

    def count_online(self, t):
        n = len(self.local_online(t))
        for p in self.alive_peers(t):
            n += len(p.radios)
        return min(n, 255)

    def on_peer_hello(self, src, pl, addr, t):
        info = P.parse_peer(pl)
        if info is None or src == self.hub_id:
            return
        peer = self.peer_by_addr(addr)
        if peer is None:
            for p in self.peers:        # мост из списка, чьё имя ещё не разрешили
                if p.configured and p.addr is None and p.host == addr[0] and p.port == addr[1]:
                    peer = p
                    peer.addr = addr
                    break
        if peer is None:
            # мост, которого нет в нашем списке, но ключ верный — запоминаем (настройка с одной стороны)
            peer = Peer(addr[0], addr[1], False)
            peer.addr = addr
            self.peers.append(peer)
        was = not peer.reported_down
        peer.reported_down = False
        peer.last = t
        peer.name = info["name"] or peer.host
        peer.radios = info["radios"]
        peer.last_t_from_peer = info["t_ms"]
        if info["t_echo"]:
            peer.rtt = (ms32() - info["t_echo"]) & 0xFFFFFFFF
        if not was:
            self.event("мост на связи: %s (%s)" % (peer.name, addr[0]))

    def on_from_peer(self, peer, ptype, src, pl, t):
        if peer.alive(t):
            peer.last = t
        if ptype in (P.T_TALK_START, P.T_AUDIO):
            if ptype == P.T_AUDIO:
                a = P.parse_audio(pl)
                if a is None:
                    return
                burst = a["burst"]
            else:
                if len(pl) < 4:
                    return
                burst = struct.unpack_from("<I", pl)[0]
            if not self.claim_from_peer(peer, src, burst, t):
                return
            f = self.floor
            f.last = t
            if ptype == P.T_AUDIO:
                f.frames += 1
                raw = P.build(self.k_auth, P.T_AUDIO, src, pl)
                for o in self.local_online(t):
                    self.send(raw, o.addr)
        elif ptype == P.T_TALK_END and len(pl) >= 4:
            burst = struct.unpack_from("<I", pl)[0]
            f = self.floor
            if f is not None and f.talker == src and f.burst == burst and f.origin is peer:
                self.release_floor("end", notify_local=True, notify_peers=False)

    # ─────────────── периодическое ───────────────

    def tick(self, t):
        f = self.floor
        if f is not None and t - f.last > FLOOR_IDLE_S:
            self.release_floor("пропала связь")
        for r in list(self.radios.values()):
            if not r.reported_offline and not r.online(t):
                r.reported_offline = True
                self.event("пропала: %s" % r.name)
        if t - self.last_peer_hello >= PEER_HELLO_EVERY_S:
            self.last_peer_hello = t
            self.hello_peers(t)
        if t - self.last_fw_scan >= FW_RESCAN_S:
            self.last_fw_scan = t
            self.scan_firmware()

    def hello_peers(self, t):
        radios = []
        f = self.floor
        for r in self.local_online(t):
            flags = r.flags & P.FLAG_MUTED
            if f is not None and f.talker == r.id:
                flags |= P.FLAG_TALKING
            radios.append((r.id, flags, r.name))
        for p in self.peers:
            if p.configured and (p.addr is None or t - p.last_resolve > 300) and t - p.last_resolve > 10:
                p.last_resolve = t
                try:
                    ip = socket.gethostbyname(p.host)
                    p.addr = (ip, p.port)
                except OSError as e:
                    log.warning("мост %s: адрес не найден (%s)", p.host, e)
                    continue
            if p.addr is None:
                continue
            if not p.configured and not p.alive(t):
                continue
            pl = P.peer_payload(ms32(), p.last_t_from_peer if p.alive(t) else 0, radios, self.name)
            self.send(self.build(P.T_PEER_HELLO, self.hub_id, pl), p.addr)
        for p in self.peers:
            if not p.reported_down and not p.alive(t):
                p.reported_down = True
                self.event("⚠ мост пропал: %s" % p.name)
        # забытые «незнакомые» мосты убрать
        self.peers = [p for p in self.peers if p.configured or t - p.last < 60]

    def run(self, stop=None):
        self.scan_firmware()
        self.event("мост «%s» запущен: UDP %d, страница :%d, мостов в списке %d"
                   % (self.name, self.port, self.http_port, len(self.peers)))
        while stop is None or not stop.is_set():
            try:
                ready, _, _ = select.select([self.sock], [], [], 0.05)
            except (OSError, ValueError):
                break
            if ready:
                for _ in range(64):
                    try:
                        data, addr = self.sock.recvfrom(2048)
                    except (BlockingIOError, InterruptedError):
                        break
                    except ConnectionResetError:
                        continue    # Windows: ICMP «порт недоступен» от прошлой отправки
                    except OSError:
                        break
                    try:
                        self.on_packet(data, addr)
                    except Exception:
                        log.exception("ошибка разбора пакета от %s", addr)
            t = now()
            if t - self.last_tick >= 0.1:
                self.last_tick = t
                try:
                    self.tick(t)
                except Exception:
                    log.exception("ошибка в tick")

    # ─────────────── страница состояния ───────────────

    def snapshot(self):
        t = now()
        with self.lock:
            events = list(self.events)[:60]
            bad = dict(self.bad_packets)
        f = self.floor
        radios = []
        for r in sorted(list(self.radios.values()), key=lambda x: x.first):
            radios.append({
                "id": "%08X" % r.id, "name": r.name, "ip": r.addr[0], "online": r.online(t),
                "seen_s": round(t - r.last, 1) if r.last else None, "rssi": r.rssi, "rtt_ms": r.rtt,
                "fw": r.fw, "battery": None if r.battery == 255 else r.battery, "muted": bool(r.flags & P.FLAG_MUTED),
                "talking": bool(f and f.talker == r.id and not self.floor_expired(t)),
                "silent_bursts": r.bad_level_bursts})
        peers = []
        for p in list(self.peers):
            peers.append({"host": p.host, "name": p.name, "alive": p.alive(t), "configured": p.configured,
                          "rtt_ms": p.rtt, "radios": [{"id": "%08X" % i, "name": n,
                                                       "talking": bool(fl & P.FLAG_TALKING)}
                                                      for i, fl, n in p.radios]})
        return {
            "hub": self.name, "version": HUB_VERSION, "uptime_s": int(time.time() - self.started),
            "port": self.port, "floor": None if self.floor_expired(t) else {
                "talker": self.radio_name(f.talker), "seconds": round(t - f.started, 1),
                "where": "здесь" if f.origin == "local" else f.origin.name},
            "radios": radios, "peers": peers, "events": events, "bad_packets_from": bad,
            "firmware": {"version": self.fw["version"], "size": self.fw["size"],
                         "sha256": self.fw["sha"].hex() if self.fw["sha"] else ""},
            "stats": dict(self.stats)}


PAGE_CSS = """
body{font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;margin:0;background:#f4f4f2;color:#1b1b1b}
main{max-width:860px;margin:0 auto;padding:16px}
h1{font-size:22px;margin:4px 0 2px}.sub{color:#666;margin-bottom:14px}
section{background:#fff;border-radius:12px;padding:12px 14px;margin:12px 0;box-shadow:0 1px 2px #0001}
h2{font-size:16px;margin:0 0 8px}table{border-collapse:collapse;width:100%}
td,th{padding:5px 6px;border-bottom:1px solid #eee;text-align:left;vertical-align:top}th{color:#777;font-weight:500}
.ok{color:#137a2f}.bad{color:#b3261e}.muted{color:#888}.pill{display:inline-block;padding:1px 8px;border-radius:9px;background:#e8f5e9}
.talk{background:#ffe0dc;color:#b3261e}pre{white-space:pre-wrap;font:13px/1.4 ui-monospace,Menlo,Consolas,monospace;margin:0}
@media (prefers-color-scheme:dark){body{background:#151515;color:#e6e6e6}section{background:#212121;box-shadow:none}
td,th{border-color:#333}.pill{background:#1f3a24}.talk{background:#4a1f1b;color:#ffb4ab}.ok{color:#7dd98f}.bad{color:#ff8a80}}
"""


def render_page(s):
    e = html.escape
    rows = []
    for r in s["radios"]:
        st = '<span class="pill talk">говорит</span>' if r["talking"] else (
            '<span class="ok">в сети</span>' if r["online"] else '<span class="bad">нет связи</span>')
        extra = []
        if r["muted"]:
            extra.append("звук выключен")
        if r["silent_bursts"]:
            extra.append("тишина в передачах: %d" % r["silent_bursts"])
        bat = "от сети" if r["battery"] is None else ("%d%%" % r["battery"])
        if r["battery"] is not None and r["battery"] <= 15:
            bat = '<span class="bad">%s</span>' % bat
        rows.append("<tr><td><b>%s</b><br><span class=muted>%s · %s</span></td><td>%s</td><td>%s</td>"
                    "<td>%s дБм</td><td>%s мс</td><td>%s</td><td>%s</td></tr>" % (
                        e(r["name"]), r["id"], e(r["ip"]), st, bat, r["rssi"], r["rtt_ms"], r["fw"],
                        e(", ".join(extra))))
    radios = ("<table><tr><th>Рация</th><th>Состояние</th><th>Заряд</th><th>Wi-Fi</th><th>Задержка</th>"
              "<th>Прошивка</th><th></th></tr>%s</table>" % "".join(rows)) if rows else \
        "<p class=muted>Ни одна рация сюда ещё не подключалась.</p>"
    prow = []
    for p in s["peers"]:
        names = ", ".join(("🔴 " if x["talking"] else "") + x["name"] for x in p["radios"]) or "—"
        prow.append("<tr><td><b>%s</b><br><span class=muted>%s</span></td><td>%s</td><td>%s</td><td>%s</td></tr>" % (
            e(p["name"]), e(p["host"]),
            '<span class="ok">на связи</span>' if p["alive"] else '<span class="bad">нет связи</span>',
            ("%d мс" % p["rtt_ms"]) if p["rtt_ms"] is not None else "—", e(names)))
    peers = ("<section><h2>Другие мосты</h2><table><tr><th>Мост</th><th>Связь</th><th>Задержка</th>"
             "<th>Рации там</th></tr>%s</table></section>" % "".join(prow)) if prow else ""
    fl = s["floor"]
    floor = ('<span class="pill talk">сейчас говорит %s (%s, %.0f с)</span>' % (
        e(fl["talker"]), e(fl["where"]), fl["seconds"])) if fl else '<span class="pill">эфир свободен</span>'
    fw = s["firmware"]
    fwline = ("прошивка для раций: версия %d (%d КБ)" % (fw["version"], fw["size"] // 1024)) if fw["version"] \
        else "прошивки для обновления раций в папке нет"
    up = s["uptime_s"]
    return """<!doctype html><html lang=ru><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><meta http-equiv=refresh content=5>
<title>Мост раций</title><style>%s</style></head><body><main>
<h1>Мост раций «%s»</h1><div class=sub>работает %d ч %02d мин · %s · обновляется каждые 5 с</div>
<section>%s</section><section><h2>Рации на этом мосту</h2>%s</section>%s
<section><h2>Журнал</h2><pre>%s</pre></section></main></body></html>""" % (
        PAGE_CSS, e(s["hub"]), up // 3600, (up // 60) % 60, e(fwline), floor, radios, peers,
        e("\n".join(s["events"])) or "—")


def make_http_handler(hub):
    class Handler(BaseHTTPRequestHandler):
        server_version = "walkie-hub/" + HUB_VERSION

        def log_message(self, fmt, *args):
            log.debug("http %s " + fmt, self.client_address[0], *args)

        def _authorized(self, q):
            return not hub.status_token or q.get("t", [""])[0] == hub.status_token

        def do_GET(self):
            u = urlparse(self.path)
            q = parse_qs(u.query)
            if u.path.startswith("/fw/"):
                return self.send_fw()
            if not self._authorized(q):
                return self.reply(403, "text/plain; charset=utf-8", "нужен ?t=<токен>".encode())
            if u.path == "/status.json":
                body = json.dumps(hub.snapshot(), ensure_ascii=False, indent=1).encode()
                return self.reply(200, "application/json; charset=utf-8", body)
            if u.path == "/":
                return self.reply(200, "text/html; charset=utf-8", render_page(hub.snapshot()).encode())
            self.reply(404, "text/plain", b"not found")

        def send_fw(self):
            fw = hub.fw
            want = self.path[len("/fw/"):].split(".")[0].lower()
            if not fw["path"] or not want or not fw["sha"].hex().startswith(want):
                return self.reply(404, "text/plain", b"no firmware")
            data = fw["path"].read_bytes()
            if hashlib.sha256(data).digest() != fw["sha"]:
                hub.fw["mtime"] = 0            # файл подменили на ходу — перечитаем
                return self.reply(503, "text/plain", b"firmware changing, retry")
            hub.event("рация %s скачивает прошивку %d" % (self.client_address[0], fw["version"]))
            self.reply(200, "application/octet-stream", data)

        def reply(self, code, ctype, body):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

    return Handler


def start_http(hub, listen):
    srv = ThreadingHTTPServer((listen, hub.http_port), make_http_handler(hub))
    srv.daemon_threads = True
    th = threading.Thread(target=srv.serve_forever, name="http", daemon=True)
    th.start()
    return srv


def setup_logging(cfg_dir, cfg, verbose):
    fmt = logging.Formatter("%(asctime)s %(levelname)s %(message)s", "%Y-%m-%d %H:%M:%S")
    root = logging.getLogger()
    root.setLevel(logging.DEBUG if verbose else logging.INFO)
    if sys.stdout is not None:           # pythonw.exe на Windows запускается без консоли
        try:
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
        h = logging.StreamHandler(sys.stdout)
        h.setFormatter(fmt)
        root.addHandler(h)
    log_file = cfg.get("log_file", "hub.log")
    if log_file:
        fh = logging.handlers.RotatingFileHandler(cfg_dir / log_file, maxBytes=1_000_000, backupCount=3,
                                                  encoding="utf-8")
        fh.setFormatter(fmt)
        root.addHandler(fh)


def main():
    ap = argparse.ArgumentParser(description="Мост WiFi-раций")
    ap.add_argument("--config", default=str(Path(__file__).resolve().parent / "hub.json"))
    ap.add_argument("--genkey", action="store_true", help="напечатать новый случайный ключ сети")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    if a.genkey:
        print(P.generate_key())
        return
    cfg_path = Path(a.config).resolve()
    cfg = json.loads(cfg_path.read_text(encoding="utf-8-sig"))
    setup_logging(cfg_path.parent, cfg, a.verbose)
    hub = Hub(cfg, cfg_path.parent)
    start_http(hub, cfg.get("listen", "0.0.0.0"))
    try:
        hub.run()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
