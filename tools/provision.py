#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Настроить рацию по USB-кабелю (после первой прошивки):

    python3 tools/provision.py --name Касса --server 203.0.113.10 --key XXXX-XXXX-XXXX-XXXX
    python3 tools/provision.py --show

Порт платы находится сам (/dev/cu.usbmodem* или /dev/cu.wchusbserial*), можно указать --port.
Wi-Fi удобнее вводить с телефона на странице рации (точка доступа RADIO-XXXX), но можно и здесь:
    --wifi "ИмяСети|пароль"
Только стандартная библиотека (termios) — macOS/Linux.
"""
import argparse
import glob
import os
import sys
import termios
import time


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0
    a[1] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[3] = 0
    a[4] = a[5] = termios.B115200
    a[6][termios.VMIN] = 0
    a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


def read_for(fd, seconds, until=None):
    buf = b""
    end = time.time() + seconds
    while time.time() < end:
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            chunk = b""
        if chunk:
            buf += chunk
            if until and until.encode() in buf:
                break
        else:
            time.sleep(0.02)
    return buf.decode("utf-8", "replace")


def cmd(fd, line, expect=None, wait=2.0):
    os.write(fd, (line + "\n").encode("utf-8"))
    out = read_for(fd, wait, expect)
    return out


def main():
    ap = argparse.ArgumentParser(description="Настройка рации по USB")
    ap.add_argument("--port")
    ap.add_argument("--name")
    ap.add_argument("--server", help="адрес моста host[:порт] или auto")
    ap.add_argument("--key", help="ключ сети")
    ap.add_argument("--wifi", action="append", help="«Сеть|пароль», можно несколько раз")
    ap.add_argument("--volume", type=int)
    ap.add_argument("--amp", choices=["i2s", "pdm"], help="i2s — MAX98357A, pdm — PAM8403 через фильтр (рация перезапустится)")
    ap.add_argument("--show", action="store_true")
    ap.add_argument("--reboot", action="store_true")
    a = ap.parse_args()
    port = a.port
    if not port:
        ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.wchusbserial*") + glob.glob("/dev/cu.usbserial*"))
        if not ports:
            sys.exit("плата не найдена — подключите USB-кабелем с данными")
        port = ports[0]
    fd = open_port(port)
    read_for(fd, 0.5)
    print("порт", port)
    steps = []
    if a.name:
        steps.append(("name " + a.name, "OK name"))
    if a.server:
        steps.append(("server " + a.server, "OK server"))
    if a.key:
        steps.append(("key " + a.key, "OK key"))
    for w in a.wifi or []:
        steps.append(("wifi " + w, "OK wifi"))
    if a.volume is not None:
        steps.append(("volume %d" % a.volume, "OK volume"))
    if a.amp:
        steps.append(("amp " + a.amp, "OK amp"))      # последним: после него рация перезапускается
    ok = True
    for line, expect in steps:
        out = cmd(fd, line, expect)
        good = expect in out
        ok &= good
        shown = line if not line.startswith(("key ", "wifi ")) else line.split(" ")[0] + " ***"
        print(("✓ " if good else "✗ ") + shown + ("" if good else "  → " + out.strip()[-200:]))
    if a.show or not steps:
        print(cmd(fd, "show", "чужих пакетов", 3.0))
    if a.reboot:
        cmd(fd, "reboot", "OK reboot")
    os.close(fd)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
