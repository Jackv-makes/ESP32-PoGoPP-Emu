#pragma once
// ESP32 port: stand-ins for the few nrf_soc.h entry points the firmware uses.

#include <stdint.h>

#define NRF_SUCCESS 0
#define NRF_ERROR_INTERNAL 3
#define NRF_ERROR_INVALID_STATE 8

typedef struct {
  uint8_t key[16];
  uint8_t cleartext[16];
  uint8_t ciphertext[16];
} nrf_ecb_hal_data_t;

// AES-128 ECB single block encrypt (mbedTLS underneath).
uint32_t sd_ecb_block_encrypt(nrf_ecb_hal_data_t* p_ecb_data);

// Deepest sleep the board supports; wakes on the board buttons. Does not
// return on success.
uint32_t sd_power_system_off(void);
