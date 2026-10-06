# -*- coding: utf-8 -*-
"""
Протокол WiFi-раций «WT1» — общий модуль для моста, тестов и программной рации.

Всё в «little-endian». Пакет = заголовок (8 байт) + данные + подпись (8 байт).

    заголовок: 'W' 'T' | версия (1) | тип (1) | src (4)
    подпись:   первые 8 байт HMAC-SHA256(k_auth, всё до подписи)

Ключ сети — строка вида «K7Q2-MX9P-4RTA-W8HD» (регистр и дефисы не важны).
Из неё выводятся два ключа: k_auth (подпись всех пакетов) и k_enc (шифрование речи
AES-128-CTR). Мосту для работы нужен только k_auth: речь он пересылает как есть.

Описание полей каждого типа — в docs/protocol.md и в firmware/walkie/protocol.h
(держать синхронно!).
"""
import hashlib
import hmac
import os
import struct

MAGIC = b"WT"
VERSION = 1
HDR = struct.Struct("<2sBBI")          # magic, version, type, src
MAC_LEN = 8
DEFAULT_PORT = 47000
DEFAULT_HTTP_PORT = 47080

# Типы пакетов
T_HELLO = 0x01        # рация → мост, раз в 2 с: «я жива»
T_HELLO_ACK = 0x02    # мост → рация: ответ + состояние сети + сведения о прошивке
T_TALK_REQ = 0x10     # рация → мост: «можно говорить?»
T_TALK_GRANT = 0x11   # мост → рация: «говори»
T_TALK_DENY = 0x12    # мост → рация: «нельзя» (причина)
T_TALK_END = 0x13     # конец передачи (рация → мост, мост → рации и мосты)
T_TALK_START = 0x14   # мост → мосты: «у меня начал говорить такой-то»
T_AUDIO = 0x20        # 20 мс речи
T_PEER_HELLO = 0x30   # мост ↔ мост, раз в 2 с
T_BAD_KEY = 0x7F      # мост → рация: «подпись не сошлась» (без подписи: только заголовок)

TYPE_NAMES = {
    T_HELLO: "HELLO", T_HELLO_ACK: "HELLO_ACK", T_TALK_REQ: "TALK_REQ",
    T_TALK_GRANT: "TALK_GRANT", T_TALK_DENY: "TALK_DENY", T_TALK_END: "TALK_END",
    T_TALK_START: "TALK_START", T_AUDIO: "AUDIO", T_PEER_HELLO: "PEER_HELLO",
    T_BAD_KEY: "BAD_KEY",
}

# Причины отказа
DENY_BUSY = 1          # эфир занят
DENY_REVOKED = 2       # одновременно нажали в двух местах — уступаем
DENY_TIMEOUT = 3       # слишком долго держит кнопку (защита от залипания)
DENY_UNKNOWN = 4       # мост не знает эту рацию (не было HELLO)

# Речь
SAMPLE_RATE = 16000
FRAME_SAMPLES = 320                    # 20 мс
CODEC_ADPCM16 = 1                      # IMA ADPCM 4 бит, 16 кГц
ADPCM_BYTES = FRAME_SAMPLES // 2       # 160
AUDIO_ENC_LEN = 3 + ADPCM_BYTES        # состояние кодера (3) + данные (160), шифруется целиком

AUDIO_FIX = struct.Struct("<IHBB")     # burst, seq, codec, level
HELLO_FIX = struct.Struct("<HBbIH")    # fw, flags, rssi, t_ms, rtt_ms
ACK_FIX = struct.Struct("<IBBBBHI32sH")  # t_echo, radios_online, peers_cfg, peers_up, floor_busy,
                                          # fw_version, fw_size, fw_sha256, http_port
PEER_FIX = struct.Struct("<IIB")       # t_ms, t_echo, n_radios
PEER_RADIO = struct.Struct("<IB")      # id, flags

FLAG_MUTED = 0x01      # у рации выключен звук (кнопкой регулятора)
FLAG_TALKING = 0x02    # в HELLO моста к мосту: эта рация сейчас говорит

NAME_MAX = 31          # байт UTF-8
PROBE_ID = 0xFFFFFFFF  # служебная «рация» самопроверки моста: ответ получает, в список не попадает


# ─────────────────────────── ключи ───────────────────────────

def normalize_key(key):
    return "".join(ch for ch in key.upper() if ch.isalnum())


def derive_keys(key):
    """Строка ключа → (k_auth 32 байта, k_enc 16 байт)."""
    master = normalize_key(key).encode("utf-8")
    if len(master) < 12:
        raise ValueError("ключ сети слишком короткий (нужно не меньше 12 букв/цифр)")
    k_auth = hmac.new(master, b"wt1-auth", hashlib.sha256).digest()
    k_enc = hmac.new(master, b"wt1-enc", hashlib.sha256).digest()[:16]
    return k_auth, k_enc


def generate_key():
    """Случайный ключ сети: 4 группы по 4 знака без похожих букв (0/O, 1/I)."""
    alphabet = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"
    raw = os.urandom(16)
    chars = [alphabet[b % len(alphabet)] for b in raw]
    return "-".join("".join(chars[i:i + 4]) for i in range(0, 16, 4))


# ─────────────────────────── пакеты ───────────────────────────

def sign(k_auth, body):
    return body + hmac.new(k_auth, body, hashlib.sha256).digest()[:MAC_LEN]


def build(k_auth, ptype, src, payload=b""):
    return sign(k_auth, HDR.pack(MAGIC, VERSION, ptype, src & 0xFFFFFFFF) + payload)


def parse(k_auth, data):
    """Проверить и разобрать пакет. Возвращает (type, src, payload) или None.

    None = чужой/битый пакет или неверный ключ (подпись не сошлась)."""
    if len(data) < HDR.size + MAC_LEN or data[:2] != MAGIC:
        return None
    body, mac = data[:-MAC_LEN], data[-MAC_LEN:]
    good = hmac.new(k_auth, body, hashlib.sha256).digest()[:MAC_LEN]
    if not hmac.compare_digest(mac, good):
        return None
    magic, ver, ptype, src = HDR.unpack_from(body)
    if ver != VERSION:
        return None
    return ptype, src, body[HDR.size:]


def pack_name(name):
    raw = name.encode("utf-8")[:NAME_MAX]
    # не резать посреди многобайтной буквы
    while raw:
        try:
            raw.decode("utf-8")
            break
        except UnicodeDecodeError:
            raw = raw[:-1]
    return bytes([len(raw)]) + raw


def unpack_name(buf, off):
    if off >= len(buf):
        return "", off
    n = buf[off]
    raw = buf[off + 1: off + 1 + n]
    return raw.decode("utf-8", "replace"), off + 1 + n


def hello_payload(fw, flags, rssi, t_ms, rtt_ms, name, battery=255):
    return HELLO_FIX.pack(fw, flags, rssi, t_ms & 0xFFFFFFFF, min(rtt_ms, 0xFFFF)) + pack_name(name) + bytes([battery])


def parse_hello(p):
    if len(p) < HELLO_FIX.size:
        return None
    fw, flags, rssi, t_ms, rtt = HELLO_FIX.unpack_from(p)
    name, off = unpack_name(p, HELLO_FIX.size)
    battery = p[off] if off < len(p) else 255      # 0..100 %, 255 — не от аккумулятора / старая прошивка
    return {"fw": fw, "flags": flags, "rssi": rssi, "t_ms": t_ms, "rtt": rtt, "name": name, "battery": battery}


def ack_payload(t_echo, radios_online, peers_cfg, peers_up, floor_busy,
                fw_version, fw_size, fw_sha, http_port, hub_name):
    return ACK_FIX.pack(t_echo & 0xFFFFFFFF, radios_online, peers_cfg, peers_up, floor_busy,
                        fw_version, fw_size, fw_sha or b"\0" * 32, http_port) + pack_name(hub_name)


def parse_ack(p):
    if len(p) < ACK_FIX.size:
        return None
    (t_echo, online, pcfg, pup, busy, fwv, fws, sha, http_port) = ACK_FIX.unpack_from(p)
    name, _ = unpack_name(p, ACK_FIX.size)
    return {"t_echo": t_echo, "radios_online": online, "peers_cfg": pcfg, "peers_up": pup,
            "floor_busy": busy, "fw_version": fwv, "fw_size": fws, "fw_sha": sha,
            "http_port": http_port, "name": name}


def peer_payload(t_ms, t_echo, radios, hub_name):
    """radios: список (id, flags, name)."""
    out = [PEER_FIX.pack(t_ms & 0xFFFFFFFF, t_echo & 0xFFFFFFFF, len(radios))]
    for rid, flags, name in radios[:32]:
        out.append(PEER_RADIO.pack(rid, flags) + pack_name(name))
    out.append(pack_name(hub_name))
    return b"".join(out)


def parse_peer(p):
    if len(p) < PEER_FIX.size:
        return None
    t_ms, t_echo, n = PEER_FIX.unpack_from(p)
    off = PEER_FIX.size
    radios = []
    for _ in range(n):
        if off + PEER_RADIO.size > len(p):
            return None
        rid, flags = PEER_RADIO.unpack_from(p, off)
        name, off = unpack_name(p, off + PEER_RADIO.size)
        radios.append((rid, flags, name))
    hub_name, off = unpack_name(p, off)
    return {"t_ms": t_ms, "t_echo": t_echo, "radios": radios, "name": hub_name}


def audio_payload(burst, seq, level, enc163, codec=CODEC_ADPCM16):
    return AUDIO_FIX.pack(burst, seq & 0xFFFF, codec, level) + enc163


def parse_audio(p):
    if len(p) != AUDIO_FIX.size + AUDIO_ENC_LEN:
        return None
    burst, seq, codec, level = AUDIO_FIX.unpack_from(p)
    return {"burst": burst, "seq": seq, "codec": codec, "level": level, "enc": p[AUDIO_FIX.size:]}


def audio_nonce(src, burst, seq):
    """Счётчик AES-CTR: src | burst | seq | нули. Уникален для каждого пакета речи."""
    return struct.pack("<IIH", src, burst, seq & 0xFFFF) + b"\0" * 6


# ─────────────────────────── AES-128-CTR ───────────────────────────
# Только для программной рации и тестов (мосту шифр не нужен). Чистый Python,
# сверено с FIPS-197; скорости хватает на один поток речи.

_SBOX = bytes.fromhex(
    "637c777bf26b6fc53001672bfed7ab76ca82c97dfa5947f0add4a2af9ca472c0"
    "b7fd9326363ff7cc34a5e5f171d8311504c723c31896059a071280e2eb27b275"
    "09832c1a1b6e5aa0523bd6b329e32f8453d100ed20fcb15b6acbbe394a4c58cf"
    "d0efaafb434d338545f9027f503c9fa851a3408f929d38f5bcb6da2110fff3d2"
    "cd0c13ec5f974417c4a77e3d645d197360814fdc222a908846eeb814de5e0bdb"
    "e0323a0a4906245cc2d3ac629195e479e7c8376d8dd54ea96c56f4ea657aae08"
    "ba78252e1ca6b4c6e8dd741f4bbd8b8a703eb5664803f60e613557b986c11d9e"
    "e1f8981169d98e949b1e87e9ce5528df8ca1890dbfe6426841992d0fb054bb16")
_RCON = (0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36)


def _xtime(a):
    a <<= 1
    return (a ^ 0x11B) if a & 0x100 else a


class AES128:
    def __init__(self, key):
        if len(key) != 16:
            raise ValueError("нужен 16-байтный ключ")
        w = [list(key[i:i + 4]) for i in range(0, 16, 4)]
        for i in range(4, 44):
            t = list(w[i - 1])
            if i % 4 == 0:
                t = t[1:] + t[:1]
                t = [_SBOX[b] for b in t]
                t[0] ^= _RCON[i // 4 - 1]
            w.append([w[i - 4][j] ^ t[j] for j in range(4)])
        self.rk = [sum(w[r * 4:r * 4 + 4], []) for r in range(11)]

    def encrypt_block(self, block):
        s = [b ^ k for b, k in zip(block, self.rk[0])]
        for rnd in range(1, 11):
            s = [_SBOX[b] for b in s]
            # ShiftRows (столбцы по 4 байта)
            s = [s[(i + 4 * (i % 4)) % 16] for i in range(16)]
            if rnd != 10:
                out = []
                for c in range(4):
                    a = s[c * 4:c * 4 + 4]
                    t = a[0] ^ a[1] ^ a[2] ^ a[3]
                    out += [a[j] ^ t ^ _xtime(a[j] ^ a[(j + 1) % 4]) for j in range(4)]
                s = out
            s = [b ^ k for b, k in zip(s, self.rk[rnd])]
        return bytes(s)

    def ctr(self, nonce16, data):
        """AES-CTR как в mbedtls: счётчик — 128-битное big-endian число."""
        ctr = int.from_bytes(nonce16, "big")
        out = bytearray()
        for i in range(0, len(data), 16):
            ks = self.encrypt_block(ctr.to_bytes(16, "big"))
            chunk = data[i:i + 16]
            out += bytes(a ^ b for a, b in zip(chunk, ks))
            ctr = (ctr + 1) & ((1 << 128) - 1)
        return bytes(out)


# ─────────────────────────── IMA ADPCM ───────────────────────────
# Эталон для прошивки (firmware/walkie/adpcm.cpp): битовые потоки должны совпадать.

_INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)
_STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767)


class AdpcmEncoder:
    def __init__(self):
        self.pred = 0
        self.index = 0

    def encode(self, samples):
        """320 отсчётов int16 → 163 байта (состояние + данные)."""
        head = struct.pack("<hB", self.pred, self.index)
        out = bytearray(len(samples) // 2)
        pred, index = self.pred, self.index
        for i, x in enumerate(samples):
            step = _STEP_TABLE[index]
            diff = x - pred
            nib = 0
            if diff < 0:
                nib = 8
                diff = -diff
            delta = step >> 3
            if diff >= step:
                nib |= 4
                diff -= step
                delta += step
            step >>= 1
            if diff >= step:
                nib |= 2
                diff -= step
                delta += step
            step >>= 1
            if diff >= step:
                nib |= 1
                delta += step
            pred = pred - delta if nib & 8 else pred + delta
            pred = -32768 if pred < -32768 else (32767 if pred > 32767 else pred)
            index += _INDEX_TABLE[nib]
            index = 0 if index < 0 else (88 if index > 88 else index)
            if i & 1:
                out[i >> 1] |= nib << 4
            else:
                out[i >> 1] = nib
        self.pred, self.index = pred, index
        return head + bytes(out)


def adpcm_decode(enc163):
    pred, index = struct.unpack_from("<hB", enc163)
    index = min(max(index, 0), 88)
    out = []
    for byte in enc163[3:]:
        for nib in (byte & 0x0F, byte >> 4):
            step = _STEP_TABLE[index]
            delta = step >> 3
            if nib & 4:
                delta += step
            if nib & 2:
                delta += step >> 1
            if nib & 1:
                delta += step >> 2
            pred = pred - delta if nib & 8 else pred + delta
            pred = -32768 if pred < -32768 else (32767 if pred > 32767 else pred)
            index += _INDEX_TABLE[nib]
            index = 0 if index < 0 else (88 if index > 88 else index)
            out.append(pred)
    return out
