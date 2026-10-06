#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Самопроверка моста: служебная «рация» шлёт HELLO и ждёт ответ. Код выхода 0 — мост жив, 1 — не ответил.

    python3 healthcheck.py [--config hub.json] [--host 127.0.0.1]
Таймер walkie-hub-check (install_linux.sh) запускает её раз в 2 минуты и перезапускает мост, если ответа нет.
"""
import argparse
import json
import socket
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wt_proto as P  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default=str(Path(__file__).resolve().parent / "hub.json"))
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--timeout", type=float, default=3.0)
    a = ap.parse_args()
    cfg = json.loads(Path(a.config).read_text(encoding="utf-8-sig"))
    k_auth, _ = P.derive_keys(cfg["key"])
    port = int(cfg.get("port", P.DEFAULT_PORT))
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.5)
    end = time.monotonic() + a.timeout
    while time.monotonic() < end:
        t_ms = int(time.monotonic() * 1000) & 0xFFFFFFFF
        s.sendto(P.build(k_auth, P.T_HELLO, P.PROBE_ID, P.hello_payload(0, 0, 0, t_ms, 0, "самопроверка")), (a.host, port))
        try:
            data, _ = s.recvfrom(2048)
        except socket.timeout:
            continue
        p = P.parse(k_auth, data)
        if p and p[0] == P.T_HELLO_ACK:
            print("мост отвечает")
            return 0
    print("мост НЕ отвечает")
    return 1


if __name__ == "__main__":
    sys.exit(main())
