[Русский](../protocol.md) · **English**

# The "WT1" radio protocol

A single UDP port on the bridge (default **47000**). The radio sends from its own port 47001.
Everything is little-endian. Code: `hub/wt_proto.py` (the reference) and `firmware/walkie/protocol.h` — **change them together**.

## Packet

```
'W' 'T' | version=1 (1) | type (1) | src (4) | data … | signature (8)
```

* `src` — the radio id (FNV-1a of the MAC) or the bridge id (sha256 of the bridge name).
* Signature — the first 8 bytes of HMAC-SHA256(k_auth, everything before the signature).
* Network key — a string like `K7Q2-MX9P-4RTA-W8HD`; case, spaces and hyphens don't matter.
  `k_auth = HMAC-SHA256(key, "wt1-auth")`, `k_enc = HMAC-SHA256(key, "wt1-enc")[:16]`.
* Exception: `BAD_KEY` (0x7F) — header only, no signature: this is how the bridge tells a radio
  that its signature didn't match ("wrong network key").

## Types

| Type | From → to | Data |
|---|---|---|
| `HELLO` 0x01 | radio → bridge, every 2 s (with no bridge — every 1 s, broadcast) | fw u16, flags u8 (1 = sound muted), rssi i8, t_ms u32, rtt u16, name (length u8 + UTF-8 ≤ 31), charge u8 (0–100 %, 255 — not on battery; optional) |
| `HELLO_ACK` 0x02 | bridge → radio | t_echo u32, radios on the network u8, bridges in the list u8, bridges online u8, channel busy u8, firmware version u16, size u32, sha256 32 bytes, http port u16, bridge name |
| `TALK_REQ` 0x10 | radio → bridge (repeated every 150 ms, for up to 700 ms) | burst u32 (random) |
| `TALK_GRANT` 0x11 | bridge → radio | burst u32 |
| `TALK_DENY` 0x12 | bridge → radio | burst u32, reason u8: 1 busy, 2 yield (simultaneous press), 3 held longer than 90 s, 4 the bridge doesn't know this radio |
| `TALK_END` 0x13 | radio → bridge (3 times); bridge → radios and bridges | burst u32; `src` = who was talking |
| `TALK_START` 0x14 | bridge → bridges | burst u32; `src` = who is talking |
| `AUDIO` 0x20 | radio → bridge → everyone | burst u32, seq u16, codec u8 (1), level u8, 163 bytes AES-128-CTR(k_enc) |
| `PEER_HELLO` 0x30 | bridge ↔ bridge, every 2 s | t_ms u32, t_echo u32, n u8, n × (id u32, flags u8, name), bridge name |

**Speech:** 16 kHz, 20 ms frame = 320 samples → 4-bit IMA ADPCM: `pred i16 | index u8 | 160 bytes`.
The encoder state is carried in every frame, so a lost packet doesn't corrupt the ones that follow.
AES-CTR counter: `src u32 | burst u32 | seq u16 | 6 zero bytes` (+1 for every 16 bytes, as in mbedtls).
The bridge doesn't decrypt speech: it forwards the packet as is.

## The channel (who is talking)

* The bridge gives the channel to whoever asked first; everyone else gets `DENY 1` and the busy tone.
* The channel is released on `TALK_END`, after 0.8 s without speech, or after 90 s (protection against a stuck button).
* Bridges pass `TALK_START`/`AUDIO` on to each other. If people in two places press at the same time,
  the **lower id** wins — identically on every bridge; the loser gets `DENY 2`.
* A bridge doesn't forward packets from one bridge to another (link the bridges as a "star" or as a full mesh, "each with each").

## Radio firmware updates

Put `firmware/walkie.bin` and `firmware/walkie.json` (`{"version": N}`) into the bridge's folder.
The bridge announces the version, size and sha256 in `HELLO_ACK`; the radio, after 20 s without any talking,
downloads `http://bridge:47080/fw/<first 16 characters of sha>.bin`, checks the sha256 and only then
switches to the new firmware. If it doesn't match — it retries in an hour.
