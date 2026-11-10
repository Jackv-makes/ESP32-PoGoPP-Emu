#pragma once
// ESP32 port: nrf_gpio.h stand-ins over the Arduino GPIO API.

#include <Arduino.h>
#include <stdint.h>

#define NRF_GPIO_PIN_MAP(port, pin) (((port) * 32) + (pin))

enum nrf_gpio_pin_pull_t {
  NRF_GPIO_PIN_NOPULL = 0,
  NRF_GPIO_PIN_PULLDOWN = 1,
  NRF_GPIO_PIN_PULLUP = 3,
};

enum nrf_gpio_pin_sense_t {
  NRF_GPIO_PIN_NOSENSE = 0,
  NRF_GPIO_PIN_SENSE_LOW = 3,
  NRF_GPIO_PIN_SENSE_HIGH = 2,
};

static inline void nrf_gpio_cfg_input(uint32_t pin, nrf_gpio_pin_pull_t pull)
{
  if (pull == NRF_GPIO_PIN_PULLUP) {
    pinMode(pin, INPUT_PULLUP);
  } else if (pull == NRF_GPIO_PIN_PULLDOWN) {
    pinMode(pin, INPUT_PULLDOWN);
  } else {
    pinMode(pin, INPUT);
  }
}

static inline uint32_t nrf_gpio_pin_read(uint32_t pin)
{
  return digitalRead(pin) ? 1 : 0;
}

static inline void nrf_gpio_cfg_sense_input(uint32_t pin, nrf_gpio_pin_pull_t pull, nrf_gpio_pin_sense_t sense)
{
  (void) sense;
  nrf_gpio_cfg_input(pin, pull);
}
