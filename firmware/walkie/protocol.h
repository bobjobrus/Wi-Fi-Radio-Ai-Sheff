// Протокол раций «WT1». Держать в согласии с hub/wt_proto.py и docs/protocol.md!
// Всё little-endian. Пакет = заголовок 8 байт + данные + подпись 8 байт (HMAC-SHA256).
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace wt {

constexpr uint8_t VERSION = 1;
constexpr uint16_t DEFAULT_PORT = 47000;
constexpr size_t HDR_LEN = 8;          // 'W' 'T' ver type src(4)
constexpr size_t MAC_LEN = 8;

enum : uint8_t {
  T_HELLO = 0x01,       // рация → мост, раз в 2 с
  T_HELLO_ACK = 0x02,   // мост → рация
  T_TALK_REQ = 0x10,    // рация → мост: «можно говорить?»
  T_TALK_GRANT = 0x11,  // мост → рация: «говори»
  T_TALK_DENY = 0x12,   // мост → рация: отказ (причина)
  T_TALK_END = 0x13,    // конец передачи
  T_TALK_START = 0x14,  // мост → мосты
  T_AUDIO = 0x20,       // 20 мс речи
  T_PEER_HELLO = 0x30,  // мост ↔ мост
  T_BAD_KEY = 0x7F,     // мост → рация: подпись не сошлась (пакет БЕЗ подписи, только заголовок)
};

enum : uint8_t { DENY_BUSY = 1, DENY_REVOKED = 2, DENY_TIMEOUT = 3, DENY_UNKNOWN = 4 };

constexpr int SAMPLE_RATE = 16000;
constexpr int FRAME_SAMPLES = 320;     // 20 мс
constexpr int FRAME_MS = 20;
constexpr int ADPCM_BYTES = FRAME_SAMPLES / 2;
constexpr int AUDIO_ENC_LEN = 3 + ADPCM_BYTES;   // состояние кодера + данные (шифруется целиком)
constexpr uint8_t CODEC_ADPCM16 = 1;

constexpr uint8_t FLAG_MUTED = 0x01;
constexpr size_t NAME_MAX_BYTES = 31;

// Размеры фиксированных частей данных
constexpr size_t HELLO_FIX = 2 + 1 + 1 + 4 + 2;               // fw, flags, rssi, t_ms, rtt
constexpr size_t ACK_FIX = 4 + 1 + 1 + 1 + 1 + 2 + 4 + 32 + 2; // см. wt_proto.ACK_FIX
constexpr size_t AUDIO_FIX = 4 + 2 + 1 + 1;                    // burst, seq, codec, level

inline void put16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
inline void put32(uint8_t* p, uint32_t v) {
  p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24;
}
inline uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t get32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

}  // namespace wt
