#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Программная рация: проверить мост и настоящие рации с компьютера, без железа.

    python3 sim_radio.py --hub 1.2.3.4 --key XXXX-XXXX-XXXX-XXXX status
        подключиться, показать ответ моста (задержка, сколько раций в сети)
    python3 sim_radio.py ... tone --seconds 3
        «нажать кнопку» и 3 секунды передавать тон 800 Гц — все рации его услышат
    python3 sim_radio.py ... say файл.wav
        передать WAV (16 кГц, моно, 16 бит)
    python3 sim_radio.py ... listen --seconds 60 --out запись.wav
        слушать эфир и записать всё, что говорят рации

Только стандартная библиотека Python.
"""
import argparse
import math
import socket
import struct
import sys
import time
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wt_proto as P  # noqa: E402


class SimRadio:
    def __init__(self, hub, key, name, rid):
        host, _, port = hub.partition(":")
        self.hub = (socket.gethostbyname(host), int(port or P.DEFAULT_PORT))
        self.k_auth, self.k_enc = P.derive_keys(key)
        self.aes = P.AES128(self.k_enc)
        self.id = rid
        self.name = name
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", 0))
        self.sock.settimeout(0.01)
        self.t0 = time.monotonic()
        self.last_hello = 0.0
        self.ack = None
        self.rtt = 0
        self.inbox = []

    def ms(self):
        return int((time.monotonic() - self.t0) * 1000) & 0xFFFFFFFF

    def send(self, ptype, payload=b""):
        self.sock.sendto(P.build(self.k_auth, ptype, self.id, payload), self.hub)

    def hello(self):
        self.send(P.T_HELLO, P.hello_payload(0, 0, 0, self.ms(), self.rtt, self.name))
        self.last_hello = time.monotonic()

    def pump(self, seconds):
        end = time.monotonic() + seconds
        while True:
            if time.monotonic() - self.last_hello > 2:
                self.hello()
            try:
                data, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                if time.monotonic() >= end:
                    return
                continue
            if len(data) == P.HDR.size and data[3] == P.T_BAD_KEY:
                print("⚠ мост ответил: подпись не сошлась — не тот ключ сети")
                continue
            p = P.parse(self.k_auth, data)
            if p is None:
                continue
            ptype, src, pl = p
            if ptype == P.T_HELLO_ACK:
                self.ack = P.parse_ack(pl)
                self.rtt = (self.ms() - self.ack["t_echo"]) & 0xFFFFFFFF
            else:
                self.inbox.append((time.monotonic(), ptype, src, pl))
            if time.monotonic() >= end:
                return

    def connect(self, timeout=5.0):
        self.hello()
        end = time.monotonic() + timeout
        while self.ack is None and time.monotonic() < end:
            self.pump(0.2)
        return self.ack is not None

    def talk(self, samples):
        """Передать отсчёты int16 (16 кГц): запрос эфира → речь по 20 мс → конец."""
        burst = struct.unpack("<I", __import__("os").urandom(4))[0] or 1
        self.inbox.clear()
        granted = False
        for _ in range(5):
            self.send(P.T_TALK_REQ, struct.pack("<I", burst))
            self.pump(0.15)
            for _, ptype, _, pl in self.inbox:
                if ptype == P.T_TALK_GRANT and struct.unpack_from("<I", pl)[0] == burst:
                    granted = True
                if ptype == P.T_TALK_DENY and struct.unpack_from("<I", pl)[0] == burst:
                    print("эфир не дали: причина", pl[4])
                    return False
            if granted:
                break
        if not granted:
            print("мост не ответил на запрос эфира")
            return False
        enc = P.AdpcmEncoder()
        start = time.monotonic()
        n = P.FRAME_SAMPLES
        frames = (len(samples) + n - 1) // n
        for seq in range(frames):
            chunk = samples[seq * n:(seq + 1) * n]
            chunk = chunk + [0] * (n - len(chunk))
            raw = enc.encode(chunk)
            level = min(255, max(abs(x) for x in chunk) // 128)
            payload = P.audio_payload(burst, seq, level, self.aes.ctr(P.audio_nonce(self.id, burst, seq), raw))
            self.send(P.T_AUDIO, payload)
            # держать темп реального времени
            delay = start + (seq + 1) * 0.02 - time.monotonic()
            if delay > 0:
                self.pump(delay)
        for _ in range(3):
            self.send(P.T_TALK_END, struct.pack("<I", burst))
            time.sleep(0.03)
        return True

    def listen(self, seconds, out_path):
        """Записать эфир в WAV. Кадры раскладываются по номерам (как в рации), а не по времени прихода:
        сетевой разброс не превращается в «дырки». Печатает потери и разброс доставки."""
        start = time.monotonic()
        bursts = {}          # (src, burst) → {seq: (время прихода, pcm)}
        order = []
        while time.monotonic() - start < seconds:
            self.pump(0.1)
            for t, ptype, src, pl in self.inbox:
                if ptype != P.T_AUDIO:
                    continue
                a = P.parse_audio(pl)
                if a is None:
                    continue
                key = (src, a["burst"])
                if key not in bursts:
                    bursts[key] = {}
                    order.append(key)
                    print("%s  говорит рация %08X" % (time.strftime("%H:%M:%S"), src))
                if a["seq"] not in bursts[key]:
                    raw = self.aes.ctr(P.audio_nonce(src, a["burst"], a["seq"]), a["enc"])
                    bursts[key][a["seq"]] = (t, P.adpcm_decode(raw))
            self.inbox.clear()
        pcm = []
        for key in order:
            frames = bursts[key]
            s0, s1 = min(frames), max(frames)
            lost = 0
            for seq in range(s0, s1 + 1):
                if seq in frames:
                    pcm.extend(frames[seq][1])
                else:
                    pcm.extend([0] * P.FRAME_SAMPLES)
                    lost += 1
            lat = [frames[q][0] - (q - s0) * P.FRAME_SAMPLES / P.SAMPLE_RATE for q in frames]
            base = min(lat)
            jit = sorted((x - base) * 1000 for x in lat)
            p95 = jit[int(len(jit) * 0.95) - 1] if jit else 0
            print("передача %08X: кадров %d из %d, потеряно %d; разброс доставки: медиана %.0f мс, 95%% — %.0f мс, "
                  "максимум %.0f мс; позже 160 мс пришло %d" % (key[0], len(frames), s1 - s0 + 1, lost,
                  jit[len(jit) // 2] if jit else 0, p95, jit[-1] if jit else 0, sum(1 for j in jit if j > 160)))
            pcm.extend([0] * (P.SAMPLE_RATE // 3))       # пауза между передачами
        with wave.open(str(out_path), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(P.SAMPLE_RATE)
            w.writeframes(struct.pack("<%dh" % len(pcm), *pcm))
        print("записано передач: %d, %.1f с звука → %s" % (len(order), len(pcm) / P.SAMPLE_RATE, out_path))
    def echo(self, minutes, pause=0.6, max_sec=60, save=None):
        """«Попугай»: всё, что сказала любая рация, сразу после отпускания кнопки
        передаётся обратно в эфир — проверка «связь с самим собой»."""
        end = time.monotonic() + minutes * 60
        bursts = {}              # (src, burst) → {"frames": {seq: pcm}, "last": t, "ended": bool}
        print("эхо включено на %d мин: говорите в рацию, отпустите кнопку — услышите себя" % minutes)
        while time.monotonic() < end:
            self.pump(0.05)
            for t, ptype, src, pl in self.inbox:
                if src == self.id:
                    continue
                if ptype == P.T_AUDIO:
                    a = P.parse_audio(pl)
                    if a is None:
                        continue
                    b = bursts.setdefault((src, a["burst"]), {"frames": {}, "last": t, "ended": False})
                    if a["seq"] not in b["frames"]:
                        raw = self.aes.ctr(P.audio_nonce(src, a["burst"], a["seq"]), a["enc"])
                        b["frames"][a["seq"]] = P.adpcm_decode(raw)
                    b["last"] = t
                elif ptype == P.T_TALK_END and len(pl) >= 4:
                    key = (src, struct.unpack_from("<I", pl)[0])
                    if key in bursts:
                        bursts[key]["ended"] = True
            self.inbox.clear()
            now = time.monotonic()
            for key in list(bursts):
                b = bursts[key]
                if not (b["ended"] or now - b["last"] > 0.8):
                    continue
                del bursts[key]
                fr = b["frames"]
                if not fr:
                    continue
                s0, s1 = min(fr), max(fr)
                pcm = []
                for q in range(s0, min(s1, s0 + max_sec * 50) + 1):
                    pcm.extend(fr.get(q, [0] * P.FRAME_SAMPLES))
                n = min(s1, s0 + max_sec * 50) - s0 + 1
                lost = n - sum(1 for q in fr if q <= s0 + n - 1)
                peak = max(abs(x) for x in pcm) or 1
                rms = math.sqrt(sum(x * x for x in pcm) / len(pcm)) or 1
                print("%s  рация %08X сказала %.1f с (кадров %d, потеряно %d; громкость: пик %.0f дБ, средняя %.0f дБ) — возвращаю" % (
                    time.strftime("%H:%M:%S"), key[0], len(pcm) / P.SAMPLE_RATE, n, lost,
                    20 * math.log10(peak / 32768), 20 * math.log10(rms / 32768)), flush=True)
                if save:
                    Path(save).mkdir(parents=True, exist_ok=True)
                    with wave.open(str(Path(save) / ("%s_%08X.wav" % (time.strftime("%H%M%S"), key[0]))), "wb") as w:
                        w.setnchannels(1)
                        w.setsampwidth(2)
                        w.setframerate(P.SAMPLE_RATE)
                        w.writeframes(struct.pack("<%dh" % len(pcm), *pcm))
                self.pump(pause)                 # дать рации доиграть «конец связи» и освободить эфир
                self.inbox.clear()
                if not self.talk(pcm):
                    print("   не получилось вернуть (эфир занят)")


def tone(seconds, hz=800.0, amp=9000):
    n = int(seconds * P.SAMPLE_RATE)
    return [int(amp * math.sin(2 * math.pi * hz * i / P.SAMPLE_RATE)) for i in range(n)]


def read_wav(path):
    with wave.open(str(path), "rb") as w:
        if w.getframerate() != P.SAMPLE_RATE or w.getnchannels() != 1 or w.getsampwidth() != 2:
            sys.exit("нужен WAV 16 кГц, моно, 16 бит (например: ffmpeg -i in.mp3 -ar 16000 -ac 1 out.wav)")
        data = w.readframes(w.getnframes())
    return list(struct.unpack("<%dh" % (len(data) // 2), data))


def main():
    ap = argparse.ArgumentParser(description="Программная рация для проверки моста")
    ap.add_argument("--hub", required=True, help="адрес моста host[:порт]")
    ap.add_argument("--key", required=True, help="ключ сети")
    ap.add_argument("--name", default="Компьютер")
    ap.add_argument("--id", type=lambda s: int(s, 16), default=0xC0FFEE01, help="id в hex")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    t = sub.add_parser("tone")
    t.add_argument("--seconds", type=float, default=3.0)
    t.add_argument("--hz", type=float, default=800.0)
    s = sub.add_parser("say")
    s.add_argument("wav")
    e = sub.add_parser("echo", help="«попугай»: возвращает в эфир всё сказанное — связь с самим собой")
    e.add_argument("--minutes", type=float, default=30)
    e.add_argument("--save", help="папка: сохранять каждую передачу в WAV")
    li = sub.add_parser("listen")
    li.add_argument("--seconds", type=float, default=60)
    li.add_argument("--out", default="efir.wav")
    a = ap.parse_args()

    r = SimRadio(a.hub, a.key, a.name, a.id)
    t0 = time.monotonic()
    if not r.connect():
        sys.exit("мост %s:%d не ответил за 5 с (адрес, порт UDP, брандмауэр, ключ?)" % r.hub)
    ack = r.ack
    print("мост «%s» ответил за %.0f мс: раций в сети %d, мостов %d/%d, прошивка для раций v%d" % (
        ack["name"], r.rtt, ack["radios_online"], ack["peers_up"], ack["peers_cfg"], ack["fw_version"]))
    if a.cmd == "tone":
        ok = r.talk(tone(a.seconds, a.hz))
        print("тон передан" if ok else "не передан")
    elif a.cmd == "say":
        ok = r.talk(read_wav(a.wav))
        print("файл передан" if ok else "не передан")
    elif a.cmd == "listen":
        r.listen(a.seconds, a.out)
    elif a.cmd == "echo":
        r.echo(a.minutes, save=a.save)
    print("всего %.1f с" % (time.monotonic() - t0))


if __name__ == "__main__":
    main()
