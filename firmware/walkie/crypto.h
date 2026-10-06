// Подпись пакетов (HMAC-SHA256, 8 байт) и шифрование речи (AES-128-CTR) ключом сети.
// Ключи выводятся из строки ключа так же, как в hub/wt_proto.py (derive_keys).
#pragma once
#include <stddef.h>
#include <stdint.h>

bool crypto_set_key(const char* key);   // false — ключ пустой/короткий (< 12 знаков)
bool crypto_has_key();
bool crypto_key_valid(const char* key);            // только проверка длины, ключ не меняет
size_t crypto_sign(uint8_t* buf, size_t len);          // дописывает 8 байт, возвращает новую длину
bool crypto_verify(const uint8_t* buf, size_t len);    // len — вместе с подписью
void crypto_ctr(uint32_t src, uint32_t burst, uint16_t seq, uint8_t* data, size_t len);
