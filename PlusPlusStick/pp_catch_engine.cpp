#include "pp_catch_engine.h"

#include <esp_random.h>

namespace {

// Written by the NimBLE host task (LED write callback), read by the loop task.
static volatile bool gPressPending = false;
static volatile uint32_t gPressAt = 0;
static bool gReleasePending = false;
static uint32_t gReleaseAt = 0;
static constexpr uint32_t kReleaseAfterMs = 500;

static void firePress(const CatchEngineRefs& refs)
{
  // Per u/EeveesGalore's GO Plus notes: the button is sampled every 50 ms,
  // byte 0 holds samples 0-1 (bit 1 = sample 0), byte 1 holds samples 2-9
  // (bit 0 = sample 9). Randomise where the press sits and how long it is,
  // keeping it at least 200 ms, exactly as pgpemu does.
  int pressStart = esp_random() % 6;
  int pressLast = pressStart + 4 + esp_random() % (10 - pressStart - 4);
  uint16_t pattern = 0;
  for (int i = 0; i < 10; ++i) {
    pattern <<= 1;
    if (i >= pressStart && i <= pressLast) {
      pattern |= 1;
    }
  }
  pattern &= 0x03FF;
  uint8_t b0 = static_cast<uint8_t>((pattern >> 8) & 0x03);
  uint8_t b1 = static_cast<uint8_t>(pattern & 0xFF);
  Serial.print("Catch engine: press bitmap ");
  Serial.print(b0, HEX);
  Serial.print(" ");
  Serial.print(b1, HEX);
  Serial.print(" duration_ms=");
  Serial.println((pressLast - pressStart + 1) * 50);
  refs.sendButtonState(b0, b1);
  gReleasePending = true;
  gReleaseAt = millis() + kReleaseAfterMs;
}

}  // namespace

const char* ppLedEventLabel(PPLedEvent evt)
{
  switch (evt) {
    case PP_LED_OFF: return "LEDs off";
    case PP_LED_POKEMON: return "Pokemon nearby";
    case PP_LED_NEW_POKEMON: return "New Pokemon!";
    case PP_LED_POKESTOP: return "Pokestop";
    case PP_LED_CAUGHT: return "Caught!";
    case PP_LED_FLED: return "Fled";
    case PP_LED_ITEMS: return "Got items";
    case PP_LED_BAG_FULL: return "Bag full";
    case PP_LED_BOX_FULL: return "Box full";
    case PP_LED_BALLS_EMPTY: return "No balls / out of range";
    case PP_LED_UNKNOWN: return "Unknown pattern";
    default: return "-";
  }
}

void ppCatchReset()
{
  gPressPending = false;
  gReleasePending = false;
}

bool ppCatchRequestPress(const CatchEngineRefs& refs, uint32_t delayMs, const char* reason)
{
  if (*refs.pairingStage != PAIR_STAGE_AUTHENTICATED || *refs.activeConnHandle == BLE_CONN_HANDLE_INVALID) {
    Serial.print("Catch engine: press ignored (not authenticated) via ");
    Serial.println(reason);
    return false;
  }
  if (gPressPending) {
    Serial.print("Catch engine: press already pending, dropped via ");
    Serial.println(reason);
    return false;
  }
  gPressAt = millis() + delayMs;  // deadline first, then publish the flag
  gPressPending = true;
  Serial.print("Catch engine: press queued in ");
  Serial.print(delayMs);
  Serial.print(" ms via ");
  Serial.println(reason);
  return true;
}

void ppCatchTick(const CatchEngineRefs& refs)
{
  uint32_t now = millis();
  if (gPressPending && static_cast<int32_t>(now - gPressAt) >= 0) {
    gPressPending = false;
    if (*refs.pairingStage == PAIR_STAGE_AUTHENTICATED && *refs.activeConnHandle != BLE_CONN_HANDLE_INVALID) {
      firePress(refs);
    }
  }
  if (gReleasePending && static_cast<int32_t>(now - gReleaseAt) >= 0) {
    gReleasePending = false;
    if (*refs.activeConnHandle != BLE_CONN_HANDLE_INVALID) {
      refs.sendButtonState(0x00, 0x00);
    }
  }
}

PPLedEvent ppCatchHandleLedWrite(const CatchEngineRefs& refs, const uint8_t* buffer, uint16_t len)
{
  if (len < 4) {
    Serial.print("Catch engine: LED write too short len=");
    Serial.println(len);
    return PP_LED_NONE;
  }
  int patterns = buffer[3] & 0x1F;
  if (len < 4 + 3 * patterns) {
    Serial.print("Catch engine: LED write truncated len=");
    Serial.print(len);
    Serial.print(" patterns=");
    Serial.println(patterns);
    return PP_LED_NONE;
  }
  int priority = (buffer[3] >> 5) & 0x07;

  int durationUnits = 0;
  int countBallShake = 0;
  int countRed = 0, countGreen = 0, countBlue = 0, countYellow = 0, countWhite = 0;
  int countOff = 0, countNotOff = 0;
  bool anyVibration = false;

  for (int i = 0; i < patterns; ++i) {
    const uint8_t* pat = &buffer[4 + 3 * i];
    uint8_t duration = pat[0];
    uint8_t red = pat[1] & 0x0F;
    uint8_t green = (pat[1] >> 4) & 0x0F;
    uint8_t blue = pat[2] & 0x0F;
    bool vibration = (pat[2] & 0x70) != 0;
    anyVibration |= vibration;
    durationUnits += duration;

    if (!red && !green && !blue) {
      ++countOff;
      continue;
    }
    ++countNotOff;
    if (i <= 9 && red && green && blue) {
      ++countBallShake;
    }
    if (red && !green && !blue) ++countRed;
    else if (!red && green && !blue) ++countGreen;
    else if (!red && !green && blue) ++countBlue;
    else if (red && green && !blue) ++countYellow;
    else if (red && green && blue) ++countWhite;
  }

  Serial.print("Catch engine: LED patterns=");
  Serial.print(patterns);
  Serial.print(" prio=");
  Serial.print(priority);
  Serial.print(" total_ms=");
  Serial.print(durationUnits * 50);
  Serial.print(" r/g/b/y/w/off=");
  Serial.print(countRed); Serial.print('/');
  Serial.print(countGreen); Serial.print('/');
  Serial.print(countBlue); Serial.print('/');
  Serial.print(countYellow); Serial.print('/');
  Serial.print(countWhite); Serial.print('/');
  Serial.print(countOff);
  Serial.print(anyVibration ? " vib" : "");
  Serial.println();

  PPLedEvent evt = PP_LED_UNKNOWN;
  bool press = false;
  bool spin = refs.autoSpin();
  uint8_t catchMode = refs.catchMode();

  if (countOff && !countNotOff) {
    evt = PP_LED_OFF;
  } else if (countWhite && countWhite == countNotOff) {
    evt = PP_LED_BAG_FULL;
  } else if (countRed && countOff && countRed == countNotOff) {
    evt = PP_LED_BALLS_EMPTY;
  } else if (countRed && !countOff && countRed == countNotOff) {
    evt = PP_LED_BOX_FULL;
  } else if (countGreen && countGreen == countNotOff) {
    evt = PP_LED_POKEMON;
    press = (catchMode == 1);
  } else if (countYellow && countYellow == countNotOff) {
    evt = PP_LED_NEW_POKEMON;
    press = (catchMode != 0);
  } else if (countBlue && countBlue == countNotOff) {
    evt = PP_LED_POKESTOP;
    press = spin;
  } else if (countBallShake) {
    if (countBlue && countGreen) {
      evt = PP_LED_CAUGHT;
    } else if (countRed) {
      evt = PP_LED_FLED;
    } else {
      evt = PP_LED_UNKNOWN;
    }
  } else if (countRed && countGreen && countBlue && !countOff) {
    evt = PP_LED_ITEMS;
  } else {
    evt = PP_LED_UNKNOWN;
    // pgpemu presses on anything it does not recognise when auto is on.
    press = spin || (catchMode != 0);
  }

  if (evt != PP_LED_OFF && refs.onEvent) {
    refs.onEvent(evt, ppLedEventLabel(evt));
  }

  if (press) {
    int patternMs = durationUnits * 50;
    // Normal keeps pgpemu's human-like 1.0-2.5 s; Fast answers in 0.3-0.8 s.
    // Fast only applies to recognised prompts; unknown patterns keep pgpemu's pacing.
    bool fast = refs.pressSpeed && refs.pressSpeed() == 1 && evt != PP_LED_UNKNOWN;
    uint32_t delayMs = fast ? (300 + esp_random() % 501) : (1000 + esp_random() % 1501);
    if (static_cast<int>(delayMs) < patternMs) {
      ppCatchRequestPress(refs, delayMs, ppLedEventLabel(evt));
    } else {
      Serial.print("Catch engine: pattern too short for a press (");
      Serial.print(patternMs);
      Serial.println(" ms)");
    }
  }
  return evt;
}
