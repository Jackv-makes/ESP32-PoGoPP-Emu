#pragma once
// Software AES-128 single-block ECB encrypt for the ESP32 port.
//
// The S3's mbedTLS build routes AES through the hardware engine over the
// shared GDMA channel, which faults (Cache error / MMU entry fault in
// gdma_disconnect) when the first-pair auth code calls it from inside a
// NimBLE write callback. The protocol only needs one 16-byte block at a time,
// so a plain table-based implementation is used instead.

#include <stdint.h>

void pp_aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
bool pp_aes128_self_test();
