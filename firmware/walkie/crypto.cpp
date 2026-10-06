#include "crypto.h"
#include <ctype.h>
#include <string.h>
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "protocol.h"

static uint8_t s_auth[32];
static mbedtls_aes_context s_aes;
static bool s_have = false;
static bool s_aes_init = false;

static void hmac256(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen, uint8_t out[32]) {
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, klen, msg, mlen, out);
}

bool crypto_set_key(const char* key) {
  uint8_t norm[64];
  size_t n = 0;
  for (const char* p = key; p && *p && n < sizeof(norm); p++) {
    if (isalnum((unsigned char)*p)) norm[n++] = (uint8_t)toupper((unsigned char)*p);
  }
  if (n < 12) {
    s_have = false;
    return false;
  }
  hmac256(norm, n, (const uint8_t*)"wt1-auth", 8, s_auth);
  uint8_t enc[32];
  hmac256(norm, n, (const uint8_t*)"wt1-enc", 7, enc);
  if (!s_aes_init) {
    mbedtls_aes_init(&s_aes);
    s_aes_init = true;
  }
  mbedtls_aes_setkey_enc(&s_aes, enc, 128);
  memset(enc, 0, sizeof(enc));
  s_have = true;
  return true;
}

bool crypto_has_key() { return s_have; }

bool crypto_key_valid(const char* key) {
  size_t n = 0;
  for (const char* p = key; p && *p; p++)
    if (isalnum((unsigned char)*p)) n++;
  return n >= 12;
}

size_t crypto_sign(uint8_t* buf, size_t len) {
  uint8_t mac[32];
  hmac256(s_auth, sizeof(s_auth), buf, len, mac);
  memcpy(buf + len, mac, wt::MAC_LEN);
  return len + wt::MAC_LEN;
}

bool crypto_verify(const uint8_t* buf, size_t len) {
  if (!s_have || len < wt::HDR_LEN + wt::MAC_LEN) return false;
  uint8_t mac[32];
  hmac256(s_auth, sizeof(s_auth), buf, len - wt::MAC_LEN, mac);
  uint8_t diff = 0;
  for (size_t i = 0; i < wt::MAC_LEN; i++) diff |= mac[i] ^ buf[len - wt::MAC_LEN + i];
  return diff == 0;
}

void crypto_ctr(uint32_t src, uint32_t burst, uint16_t seq, uint8_t* data, size_t len) {
  uint8_t nonce[16] = {0};
  uint8_t block[16];
  size_t off = 0;
  wt::put32(nonce, src);
  wt::put32(nonce + 4, burst);
  wt::put16(nonce + 8, seq);
  mbedtls_aes_crypt_ctr(&s_aes, len, &off, nonce, block, data, data);
}
