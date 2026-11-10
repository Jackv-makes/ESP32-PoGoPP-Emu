#pragma once
// Catch / spin engine, ported from pgpemu (shortcuts/pgpemu, pgp_led_handler.c
// and pgp_autobutton.c). The Plus+ Service E is the original GO Plus
// LED/button service: the app writes colour patterns to the LED
// characteristic and the device answers with a 10-sample button bitmap on
// the button characteristic.
//
//   LED write:  3 header bytes, then count|priority<<5, then 3 bytes per
//               pattern: duration (50 ms units), green<<4|red, flags<<4|blue.
//   Button:     2 bytes, 10 one-bit samples at 50 ms, LSB of byte 1 is the
//               last sample. 00 07 is a 150 ms press at the end of the window.

#include <Arduino.h>
#include <stdint.h>

#include "bluefruit.h"
#include "pp_types.h"

enum PPLedEvent : uint8_t {
  PP_LED_NONE = 0,
  PP_LED_OFF,
  PP_LED_POKEMON,
  PP_LED_NEW_POKEMON,
  PP_LED_POKESTOP,
  PP_LED_CAUGHT,
  PP_LED_FLED,
  PP_LED_ITEMS,
  PP_LED_BAG_FULL,
  PP_LED_BOX_FULL,
  PP_LED_BALLS_EMPTY,
  PP_LED_UNKNOWN,
};

struct CatchEngineRefs {
  PairingStage* pairingStage;
  uint16_t* activeConnHandle;
  void (*sendButtonState)(uint8_t b0, uint8_t b1);
  void (*stopPostAuthRuntimeScaffolding)();
  bool (*autoSpin)();
  uint8_t (*catchMode)();  // 0 off, 1 all, 2 new only
  uint8_t (*pressSpeed)();  // 0 normal (1.0-2.5 s), 1 fast (0.3-0.8 s)
  void (*onEvent)(PPLedEvent evt, const char* label);
};

// Classify an LED write and, depending on auto mode, queue a button press.
PPLedEvent ppCatchHandleLedWrite(const CatchEngineRefs& refs, const uint8_t* data, uint16_t len);
// Queue a press after delayMs (0 = next tick). Ignored if one is pending.
bool ppCatchRequestPress(const CatchEngineRefs& refs, uint32_t delayMs, const char* reason);
// Run from the main loop: fires queued presses and the follow-up release.
void ppCatchTick(const CatchEngineRefs& refs);
void ppCatchReset();
const char* ppLedEventLabel(PPLedEvent evt);
