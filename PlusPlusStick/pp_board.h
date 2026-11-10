#pragma once
// M5Stack Stick S3 board support: buttons, status screen, settings menu,
// alerts, session counters and deep sleep.
//
// Status screen:
//   Front button (A, G11) click   -> manual catch/spin press
//   Side button  (B, G12) click   -> open the menu
//   Side button  (B, G12) hold 5s -> wipe pairing (legacy shortcut, pp_app_lifecycle)
// Menu:
//   Side click = next item, front click = select, hold either = back,
//   closes by itself after 10 s without input.
// Either button wakes the board from deep sleep.

#include <Arduino.h>
#include <stdint.h>

#include "bluefruit.h"
#include "pp_types.h"
#include "pp_catch_engine.h"

static constexpr uint8_t PP_BOARD_PIN_BTN_FRONT = 11;
static constexpr uint8_t PP_BOARD_PIN_BTN_SIDE = 12;

enum PPCatchMode : uint8_t {
  PP_CATCH_OFF = 0,
  PP_CATCH_ALL = 1,
  PP_CATCH_NEW_ONLY = 2,
};

struct BoardRefs {
  uint16_t* activeConnHandle;
  PairingStage* pairingStage;
  bool* persistentReconnectReady;
  const char* buildTag;
  const uint8_t* donorAddrLe;
  void (*manualButtonPress)();
  void (*startAdvertising)();
  void (*stopAdvertising)();
  void (*resetPairing)(const char* reason);
  const char* (*pairingStageLabel)(PairingStage stage);
};

void ppBoardInit();
void ppBoardTick(const BoardRefs& refs);

// Settings read by the catch engine and the advertising wrapper.
bool ppBoardAutoSpin();
uint8_t ppBoardCatchMode();

// Press speed: delay between the app's prompt and the emulated button press.
// Normal = 1.0-2.5 s (pgpemu's human-like default), Fast = 0.3-0.8 s. The
// game still allows one throw per encounter and paces encounters itself.
enum PPPressSpeed : uint8_t { PP_SPEED_NORMAL = 0, PP_SPEED_FAST = 1 };
uint8_t ppBoardPressSpeed();
bool ppBoardAdvertiseEnabled();

// Events from the catch engine: updates the status line, counters, alerts.
void ppBoardOnEvent(PPLedEvent evt, const char* label);
void ppBoardSetNote(const char* text);
// Link dropped: remembers the BLE reason for the status note and Info page.
void ppBoardOnDisconnect(uint8_t reason);
void ppBoardEnterDeepSleep();
