# -*- coding: utf-8 -*-
"""
Проверка моста без железа: «программные рации» на UDP-сокетах гоняют настоящий мост.

    python3 -m unittest discover -s tests -v
"""
import json
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "hub"))
import walkie_hub  # noqa: E402
import wt_proto as P  # noqa: E402

KEY = "K7Q2-MX9P-4RTA-W8HD"
K_AUTH, K_ENC = P.derive_keys(KEY)


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class HubRunner:
    def __init__(self, name="Тест", peers=(), fw=None):
        self.dir = Path(tempfile.mkdtemp())
        self.port = free_port()
        self.http_port = free_port()
        cfg = {"name": name, "listen": "127.0.0.1", "port": self.port, "http_port": self.http_port,
               "key": KEY, "peers": list(peers), "log_file": "", "tot_seconds": 3}
        if fw:
            (self.dir / "firmware").mkdir()
            (self.dir / "firmware" / "walkie.bin").write_bytes(fw[1])
            (self.dir / "firmware" / "walkie.json").write_text(json.dumps({"version": fw[0]}))
        self.hub = walkie_hub.Hub(cfg, self.dir)
        self.http = walkie_hub.start_http(self.hub, "127.0.0.1")
        self.stop = threading.Event()
        self.th = threading.Thread(target=self.hub.run, args=(self.stop,), daemon=True)
        self.th.start()

    def close(self):
        self.stop.set()
        self.th.join(2)
        self.http.shutdown()
        self.http.server_close()
        self.hub.sock.close()


class SimRadio:
    """Минимальная рация: HELLO, запрос эфира, речь, приём."""

    def __init__(self, hub_port, rid, name):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.02)
        self.hub = ("127.0.0.1", hub_port)
        self.id = rid
        self.name = name
        self.inbox = []
        self.enc = P.AdpcmEncoder()
        self.seq = 0

    def send(self, ptype, payload=b""):
        self.sock.sendto(P.build(K_AUTH, ptype, self.id, payload), self.hub)

    def hello(self):
        self.send(P.T_HELLO, P.hello_payload(1, 0, -50, 1234, 5, self.name, battery=12 if self.id == 0x30 else 255))

    def pump(self, seconds=0.15):
        end = time.time() + seconds
        while time.time() < end:
            try:
                data, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            p = P.parse(K_AUTH, data)
            if p:
                self.inbox.append(p)

    def got(self, ptype):
        return [x for x in self.inbox if x[0] == ptype]

    def talk_req(self, burst):
        self.send(P.T_TALK_REQ, struct.pack("<I", burst))

    def audio(self, burst, level=40):
        samples = [int(8000 * ((i % 40) - 20) / 20) for i in range(P.FRAME_SAMPLES)]
        raw = self.enc.encode(samples)
        enc = P.AES128(K_ENC).ctr(P.audio_nonce(self.id, burst, self.seq), raw)
        self.send(P.T_AUDIO, P.audio_payload(burst, self.seq, level, enc))
        self.seq += 1

    def end(self, burst):
        self.send(P.T_TALK_END, struct.pack("<I", burst))

    def close(self):
        self.sock.close()


class ProtoTests(unittest.TestCase):
    def test_aes_fips197(self):
        aes = P.AES128(bytes(range(16)))
        out = aes.encrypt_block(bytes.fromhex("00112233445566778899aabbccddeeff"))
        self.assertEqual(out.hex(), "69c4e0d86a7b0430d8cdb78070b4c55a")

    def test_aes_ctr_sp800_38a(self):
        # NIST SP 800-38A F.5.1 CTR-AES128.Encrypt, первые два блока
        aes = P.AES128(bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c"))
        ctr = bytes.fromhex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff")
        pt = bytes.fromhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51")
        self.assertEqual(aes.ctr(ctr, pt).hex(),
                         "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff")

    def test_adpcm_roundtrip_quality(self):
        import math
        enc = P.AdpcmEncoder()
        total_err = total_sig = 0
        for f in range(10):
            samples = [int(9000 * math.sin(2 * math.pi * 440 * (f * 320 + i) / 16000)
                           + 3000 * math.sin(2 * math.pi * 1300 * (f * 320 + i) / 16000)) for i in range(320)]
            dec = P.adpcm_decode(enc.encode(samples))
            if f:   # первый кадр — разгон кодера
                total_err += sum((a - b) ** 2 for a, b in zip(samples, dec))
                total_sig += sum(a * a for a in samples)
        snr = 10 * math.log10(total_sig / total_err)
        self.assertGreater(snr, 20, "ADPCM: отношение сигнал/шум %.1f дБ" % snr)

    def test_signature(self):
        pkt = P.build(K_AUTH, P.T_HELLO, 7, b"abc")
        self.assertEqual(P.parse(K_AUTH, pkt), (P.T_HELLO, 7, b"abc"))
        bad = bytearray(pkt)
        bad[9] ^= 1
        self.assertIsNone(P.parse(K_AUTH, bytes(bad)))
        other, _ = P.derive_keys("AAAA-BBBB-CCCC-DDDD")
        self.assertIsNone(P.parse(other, pkt))

    def test_key_normalization(self):
        self.assertEqual(P.derive_keys("k7q2 mx9p 4rta w8hd"), P.derive_keys(KEY))

    def test_name_cut_utf8(self):
        packed = P.pack_name("Ж" * 40)
        self.assertLessEqual(packed[0], P.NAME_MAX)
        packed[1:].decode("utf-8")   # не падает — не разрезали букву


class HubTests(unittest.TestCase):
    def setUp(self):
        self.h = HubRunner(fw=(5, b"\xe9firmware-bytes" * 100))
        self.a = SimRadio(self.h.port, 0x30, "Касса")
        self.b = SimRadio(self.h.port, 0x20, "Склад")
        self.c = SimRadio(self.h.port, 0x10, "Дом")
        for r in (self.a, self.b, self.c):
            r.hello()
        for r in (self.a, self.b, self.c):
            r.pump(0.1)

    def tearDown(self):
        for r in (self.a, self.b, self.c):
            r.close()
        self.h.close()

    def test_hello_ack(self):
        acks = self.c.got(P.T_HELLO_ACK)
        self.assertTrue(acks)
        info = P.parse_ack(acks[-1][2])
        self.assertEqual(info["t_echo"], 1234)
        self.assertEqual(info["fw_version"], 5)
        self.assertEqual(info["name"], "Тест")
        self.assertEqual(info["http_port"], self.h.http_port)
        self.c.hello()
        self.c.pump()
        self.assertEqual(P.parse_ack(self.c.got(P.T_HELLO_ACK)[-1][2])["radios_online"], 3)

    def test_talk_forward_and_busy(self):
        self.a.talk_req(111)
        self.a.pump()
        self.assertEqual(self.a.got(P.T_TALK_GRANT)[-1][2], struct.pack("<I", 111))
        for _ in range(5):
            self.a.audio(111)
        self.b.pump()
        self.c.pump()
        got_b = self.b.got(P.T_AUDIO)
        self.assertEqual(len(got_b), 5)
        self.assertEqual(got_b[0][1], 0x30)       # речь подписана id говорящего
        self.assertEqual(len(self.c.got(P.T_AUDIO)), 5)
        # говорящий сам себя не слышит
        self.a.pump()
        self.assertEqual(self.a.got(P.T_AUDIO), [])
        # занято
        self.b.talk_req(222)
        self.b.pump()
        deny = self.b.got(P.T_TALK_DENY)
        self.assertTrue(deny)
        self.assertEqual(struct.unpack("<IB", deny[-1][2]), (222, P.DENY_BUSY))
        # речь без разрешения не пересылается
        self.b.audio(222)
        self.c.inbox.clear()
        self.c.pump()
        self.assertEqual(self.c.got(P.T_AUDIO), [])
        # конец передачи → остальным END, эфир свободен
        self.a.end(111)
        self.b.inbox.clear()
        self.b.pump()
        self.assertTrue(self.b.got(P.T_TALK_END))
        self.b.talk_req(223)
        self.b.pump()
        self.assertTrue(self.b.got(P.T_TALK_GRANT))

    def test_floor_expires_on_silence(self):
        self.a.talk_req(1)
        self.a.pump()
        self.a.audio(1)
        time.sleep(walkie_hub.FLOOR_IDLE_S + 0.3)
        self.b.inbox.clear()
        self.b.pump()
        self.assertTrue(self.b.got(P.T_TALK_END))
        self.c.talk_req(2)
        self.c.pump()
        self.assertTrue(self.c.got(P.T_TALK_GRANT))

    def test_time_out_timer(self):
        self.a.talk_req(5)
        t0 = time.time()
        while time.time() - t0 < 3.3:       # tot_seconds = 3 в тесте
            self.a.audio(5)
            self.a.pump(0.02)
            for r in (self.a, self.b, self.c):
                r.hello()
        self.a.audio(5)
        self.a.pump(0.2)
        reasons = [struct.unpack("<IB", x[2])[1] for x in self.a.got(P.T_TALK_DENY)]
        self.assertIn(P.DENY_TIMEOUT, reasons)

    def test_probe_answered_but_not_listed(self):
        probe = SimRadio(self.h.port, P.PROBE_ID, "самопроверка")
        probe.hello()
        probe.pump(0.3)
        self.assertTrue(probe.got(P.T_HELLO_ACK))
        self.assertNotIn(P.PROBE_ID, self.h.hub.radios)
        probe.close()

    def test_unknown_radio_asked_to_hello(self):
        x = SimRadio(self.h.port, 0x99, "Новая")
        x.talk_req(9)
        x.pump()
        self.assertEqual(struct.unpack("<IB", x.got(P.T_TALK_DENY)[-1][2])[1], P.DENY_UNKNOWN)
        x.close()

    def test_wrong_key_ignored_and_reported(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        other, _ = P.derive_keys("ZZZZ-ZZZZ-ZZZZ-ZZZZ")
        s.sendto(P.build(other, P.T_HELLO, 0x55, P.hello_payload(1, 0, 0, 0, 0, "Чужая")),
                 ("127.0.0.1", self.h.port))
        s.settimeout(1.0)
        nak, _ = s.recvfrom(64)          # мост подсказывает: «не тот ключ»
        self.assertEqual(nak[:4], b"WT" + bytes([P.VERSION, P.T_BAD_KEY]))
        self.assertEqual(len(nak), P.HDR.size)
        snap = self.h.hub.snapshot()
        self.assertIn("127.0.0.1", snap["bad_packets_from"])
        self.assertNotIn("00000055", [r["id"] for r in snap["radios"]])
        s.close()

    def test_status_page_and_firmware(self):
        import urllib.request
        base = "http://127.0.0.1:%d" % self.h.http_port
        with urllib.request.urlopen(base + "/") as resp:
            page = resp.read().decode()
        self.assertIn("Касса", page)
        with urllib.request.urlopen(base + "/status.json") as resp:
            js = json.loads(resp.read())
        self.assertEqual(len(js["radios"]), 3)
        bats = {r["name"]: r["battery"] for r in js["radios"]}
        self.assertEqual(bats["Касса"], 12)
        self.assertIsNone(bats["Дом"])
        self.assertTrue(any("аккумулятор 12%" in ev for ev in js["events"]))
        sha = self.h.hub.fw["sha"].hex()
        with urllib.request.urlopen(base + "/fw/%s.bin" % sha[:16]) as resp:
            data = resp.read()
        self.assertEqual(data, b"\xe9firmware-bytes" * 100)
        with self.assertRaises(Exception):
            urllib.request.urlopen(base + "/fw/deadbeef.bin")


class PeerTests(unittest.TestCase):
    """Два моста (дом ↔ магазин): речь проходит, одновременное нажатие решается одинаково."""

    def setUp(self):
        self.store = HubRunner("Магазин")
        self.home = HubRunner("Дом", peers=["127.0.0.1:%d" % self.store.port])
        # магазин узнаёт дом по его приветствию (настройка только с одной стороны)
        self.s1 = SimRadio(self.store.port, 0x0A, "Касса")
        self.s2 = SimRadio(self.store.port, 0x0B, "Склад")
        self.h1 = SimRadio(self.home.port, 0x0C, "Дом")
        deadline = time.time() + 5
        while time.time() < deadline:
            for r in (self.s1, self.s2, self.h1):
                r.hello()
                r.pump(0.05)
            if self.home.hub.alive_peers(walkie_hub.now()) and self.store.hub.alive_peers(walkie_hub.now()):
                break
        for r in (self.s1, self.s2, self.h1):
            r.hello()
            r.pump(0.1)
            r.inbox.clear()

    def tearDown(self):
        for r in (self.s1, self.s2, self.h1):
            r.close()
        self.home.close()
        self.store.close()

    def test_link_up(self):
        self.assertTrue(self.home.hub.alive_peers(walkie_hub.now()))
        self.assertTrue(self.store.hub.alive_peers(walkie_hub.now()))

    def test_audio_crosses_bridge(self):
        self.h1.talk_req(77)
        self.h1.pump()
        self.assertTrue(self.h1.got(P.T_TALK_GRANT))
        for _ in range(4):
            self.h1.audio(77)
        self.s1.pump()
        self.s2.pump()
        self.assertEqual(len(self.s1.got(P.T_AUDIO)), 4)
        self.assertEqual(len(self.s2.got(P.T_AUDIO)), 4)
        # в магазине эфир занят домом
        self.s1.talk_req(78)
        self.s1.pump()
        self.assertEqual(struct.unpack("<IB", self.s1.got(P.T_TALK_DENY)[-1][2])[1], P.DENY_BUSY)
        self.h1.end(77)
        self.s2.pump()
        self.assertTrue(self.s2.got(P.T_TALK_END))

    def test_simultaneous_press_same_winner(self):
        # одновременно: дом (id 0x0C) и склад (id 0x0B). Меньший id — склад — должен победить везде.
        self.h1.talk_req(1)
        self.s2.talk_req(2)
        self.h1.pump(0.05)
        self.s2.pump(0.05)
        for _ in range(6):
            self.h1.audio(1)
            self.s2.audio(2)
            time.sleep(0.02)
        self.h1.pump(0.2)
        self.s1.pump(0.2)
        home_f = self.home.hub.floor
        store_f = self.store.hub.floor
        self.assertIsNotNone(home_f)
        self.assertIsNotNone(store_f)
        self.assertEqual(home_f.talker, 0x0B)
        self.assertEqual(store_f.talker, 0x0B)
        reasons = [struct.unpack("<IB", x[2])[1] for x in self.h1.got(P.T_TALK_DENY)]
        self.assertIn(P.DENY_REVOKED, reasons)
        # дом слышит склад
        self.assertTrue(any(src == 0x0B for t, src, _ in self.h1.got(P.T_AUDIO)))


if __name__ == "__main__":
    unittest.main()
