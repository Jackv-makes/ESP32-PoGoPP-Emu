#include "pp_board.h"
#include "nrf_soc.h"

#include <M5Unified.h>
#include <Preferences.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <string.h>

namespace {

// ---------------- settings ----------------

static Preferences gPrefs;
static uint8_t gCatchMode = PP_CATCH_ALL;
static bool gAutoSpin = true;
static uint8_t gSpeed = PP_SPEED_NORMAL;
// Link bookkeeping: current session start, lifetime connected seconds
// (persisted), and the last disconnect reason.
static uint32_t gConnSince = 0;
static bool gWasConnected = false;
static uint32_t gLifetimeConnSec = 0;
static uint8_t gLastDropReason = 0;
static bool gAlerts = true;
static bool gAdvertise = true;
static uint8_t gBright = 1;  // index into kBrightLevels
static const uint8_t kBrightLevels[3] = { 40, 110, 200 };

static void loadSettings()
{
  if (!gPrefs.begin("ppboard", false)) {
    return;
  }
  if (gPrefs.isKey("automode") && !gPrefs.isKey("catchmode")) {
    // Migrate the earlier single mode byte: 0 off, 1 spin only, 2 spin+catch.
    uint8_t m = gPrefs.getUChar("automode", 2);
    gCatchMode = (m == 2) ? PP_CATCH_ALL : PP_CATCH_OFF;
    gAutoSpin = (m >= 1);
    gPrefs.remove("automode");
  } else {
    gCatchMode = gPrefs.getUChar("catchmode", PP_CATCH_ALL);
    gAutoSpin = gPrefs.getBool("autospin", true);
  }
  gSpeed = gPrefs.getUChar("speed", PP_SPEED_NORMAL);
  if (gSpeed > PP_SPEED_FAST) gSpeed = PP_SPEED_NORMAL;
  if (gCatchMode > PP_CATCH_NEW_ONLY) gCatchMode = PP_CATCH_ALL;
  gAlerts = gPrefs.getBool("alerts", true);
  gAdvertise = gPrefs.getBool("adv", true);
  gBright = gPrefs.getUChar("bright", 1);
  if (gBright > 2) gBright = 1;
  gLifetimeConnSec = gPrefs.getUInt("conn_s", 0);
  gLastDropReason = gPrefs.getUChar("dropr", 0);
}

static void fmtHms(uint32_t sec, char* out, size_t n)
{
  snprintf(out, n, "%lu:%02lu:%02lu", (unsigned long) (sec / 3600), (unsigned long) ((sec / 60) % 60), (unsigned long) (sec % 60));
}

// BLE HCI disconnect reasons, in the words that matter to the wearer.
static const char* dropReasonText(uint8_t reason)
{
  switch (reason) {
    case 0x00: return "none yet";
    case 0x13: return "phone hung up";
    case 0x08: return "out of range";
    case 0x16: return "by the Stick";
    case 0x3e: return "setup failed";
    case 0x22: return "link timeout";
    case 0x3b: return "bad params";
    case 0xff: return "stack reset";
    default: return "other";
  }
}

static void saveSettings()
{
  gPrefs.putUChar("catchmode", gCatchMode);
  gPrefs.putBool("autospin", gAutoSpin);
  gPrefs.putUChar("speed", gSpeed);
  gPrefs.putBool("alerts", gAlerts);
  gPrefs.putBool("adv", gAdvertise);
  gPrefs.putUChar("bright", gBright);
}

// ---------------- runtime state ----------------

enum Screen : uint8_t { SCREEN_STATUS, SCREEN_MENU, SCREEN_CONFIRM_RESET, SCREEN_INFO };

static Screen gScreen = SCREEN_STATUS;
static int gMenuSel = 0;
static int gMenuScroll = 0;
static uint32_t gMenuLastInput = 0;
static constexpr uint32_t kMenuTimeoutMs = 10000;

static char gEvent[32] = "-";
static char gNote[32] = "";
static uint32_t gNoteAt = 0;
static bool gDirty = true;
static uint32_t gLastDraw = 0;
static char gLastState[24] = "";
static int gLastBattery = -1;
static uint32_t gLastInputAt = 0;
static bool gDisplayDimmed = false;
static constexpr uint32_t kDimAfterMs = 60UL * 1000UL;

static uint16_t gCountCaught = 0;
static uint16_t gCountFled = 0;
static uint16_t gCountSpins = 0;
static uint16_t gCountNoBalls = 0;

static uint32_t gFlashUntil = 0;
static uint32_t gFlashLastToggle = 0;
static bool gFlashPhase = false;

// Non-blocking beep sequencer: pairs of (frequency, duration ms), 0 freq = rest.
struct BeepStep {
  uint16_t freq;
  uint16_t ms;
};
static const BeepStep* gBeepSeq = nullptr;
static uint8_t gBeepLen = 0;
static uint8_t gBeepIdx = 0;
static uint32_t gBeepNextAt = 0;

static const BeepStep kBeepPokemon[] = { { 2200, 60 } };
static const BeepStep kBeepNewPokemon[] = { { 2200, 60 }, { 0, 60 }, { 2800, 90 } };
static const BeepStep kBeepPokestop[] = { { 1600, 60 } };
static const BeepStep kBeepCaught[] = { { 1800, 70 }, { 0, 50 }, { 2400, 70 }, { 0, 50 }, { 3000, 120 } };
static const BeepStep kBeepFled[] = { { 900, 160 }, { 0, 80 }, { 600, 220 } };
static const BeepStep kBeepItems[] = { { 2000, 50 }, { 0, 40 }, { 2000, 50 } };
static const BeepStep kBeepWarning[] = { { 1000, 150 }, { 0, 100 }, { 1000, 150 }, { 0, 100 }, { 1000, 150 },
                                         { 0, 400 }, { 1000, 150 }, { 0, 100 }, { 1000, 150 }, { 0, 100 }, { 1000, 150 } };

static void startBeeps(const BeepStep* seq, uint8_t len)
{
  if (!gAlerts || !M5.Speaker.isEnabled()) {
    return;
  }
  gBeepSeq = seq;
  gBeepLen = len;
  gBeepIdx = 0;
  gBeepNextAt = millis();
}

static void serviceBeeps()
{
  if (gBeepSeq == nullptr) {
    return;
  }
  uint32_t now = millis();
  if (static_cast<int32_t>(now - gBeepNextAt) < 0) {
    return;
  }
  if (gBeepIdx >= gBeepLen) {
    gBeepSeq = nullptr;
    return;
  }
  const BeepStep& s = gBeepSeq[gBeepIdx++];
  if (s.freq) {
    M5.Speaker.tone(s.freq, s.ms);
  }
  gBeepNextAt = now + s.ms;
}

// ---------------- text helpers ----------------

static const char* catchModeLabel(uint8_t mode)
{
  switch (mode) {
    case PP_CATCH_OFF: return "Off";
    case PP_CATCH_ALL: return "All";
    default: return "New only";
  }
}

static const char* onOff(bool v)
{
  return v ? "On" : "Off";
}

static const char* brightLabel(uint8_t idx)
{
  switch (idx) {
    case 0: return "Low";
    case 1: return "Mid";
    default: return "High";
  }
}

static const char* stateText(const BoardRefs& refs)
{
  bool connected = (*refs.activeConnHandle != BLE_CONN_HANDLE_INVALID);
  if (connected) {
    return (*refs.pairingStage == PAIR_STAGE_AUTHENTICATED) ? "Connected" : "Pairing...";
  }
  if (!gAdvertise) {
    return "Hidden (adv off)";
  }
  return *refs.persistentReconnectReady ? "Paired, waiting" : "Pairable";
}

// ---------------- menu model ----------------

enum MenuItem : uint8_t {
  MI_CATCH,
  MI_SPIN,
  MI_SPEED,
  MI_ALERTS,
  MI_ADVERTISE,
  MI_RESET_PAIRING,
  MI_SLEEP,
  MI_BRIGHT,
  MI_INFO,
  MI_BACK,
  MI_COUNT
};

static const char* menuLabel(uint8_t item)
{
  switch (item) {
    case MI_CATCH: return "Auto catch";
    case MI_SPIN: return "Auto spin";
    case MI_SPEED: return "Press speed";
    case MI_ALERTS: return "Alerts";
    case MI_ADVERTISE: return "Advertise";
    case MI_RESET_PAIRING: return "Reset pairing";
    case MI_SLEEP: return "Sleep now";
    case MI_BRIGHT: return "Brightness";
    case MI_INFO: return "Info";
    default: return "Back";
  }
}

static const char* menuValue(uint8_t item)
{
  switch (item) {
    case MI_CATCH: return catchModeLabel(gCatchMode);
    case MI_SPIN: return onOff(gAutoSpin);
    case MI_SPEED: return gSpeed == PP_SPEED_FAST ? "Fast" : "Normal";
    case MI_ALERTS: return onOff(gAlerts);
    case MI_ADVERTISE: return onOff(gAdvertise);
    case MI_BRIGHT: return brightLabel(gBright);
    default: return "";
  }
}

static void applyBrightness()
{
  M5.Display.setBrightness(gDisplayDimmed ? 10 : kBrightLevels[gBright]);
}

static void openMenu()
{
  gScreen = SCREEN_MENU;
  gMenuSel = 0;
  gMenuScroll = 0;
  gMenuLastInput = millis();
  gDirty = true;
}

static void closeMenu()
{
  gScreen = SCREEN_STATUS;
  gDirty = true;
}

static void menuSelect(const BoardRefs& refs)
{
  switch (gMenuSel) {
    case MI_CATCH:
      gCatchMode = static_cast<uint8_t>((gCatchMode + 1) % 3);
      saveSettings();
      break;
    case MI_SPIN:
      gAutoSpin = !gAutoSpin;
      saveSettings();
      break;
    case MI_SPEED:
      gSpeed = (gSpeed == PP_SPEED_FAST) ? PP_SPEED_NORMAL : PP_SPEED_FAST;
      saveSettings();
      break;
    case MI_ALERTS:
      gAlerts = !gAlerts;
      saveSettings();
      if (gAlerts) startBeeps(kBeepPokestop, 1);
      break;
    case MI_ADVERTISE:
      gAdvertise = !gAdvertise;
      saveSettings();
      if (gAdvertise) {
        if (*refs.activeConnHandle == BLE_CONN_HANDLE_INVALID) refs.startAdvertising();
      } else {
        refs.stopAdvertising();
      }
      break;
    case MI_RESET_PAIRING:
      gScreen = SCREEN_CONFIRM_RESET;
      break;
    case MI_SLEEP:
      ppBoardEnterDeepSleep();
      break;
    case MI_BRIGHT:
      gBright = static_cast<uint8_t>((gBright + 1) % 3);
      saveSettings();
      applyBrightness();
      break;
    case MI_INFO:
      gScreen = SCREEN_INFO;
      break;
    default:
      closeMenu();
      return;
  }
  gDirty = true;
}

// ---------------- drawing ----------------

static void addrToText(const uint8_t* le, char* out, size_t n)
{
  if (le == nullptr) {
    snprintf(out, n, "--");
    return;
  }
  snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", le[5], le[4], le[3], le[2], le[1], le[0]);
}

static int gStateLineY = 0;
static uint16_t gStatusBg = TFT_BLACK;

// Right-aligned link clock on the state line; called from drawStatus and once
// a second from the tick so the rest of the screen is not repainted.
static void drawLinkClock(const BoardRefs& refs)
{
  if (*refs.activeConnHandle == BLE_CONN_HANDLE_INVALID || !gConnSince) {
    return;
  }
  auto& d = M5.Display;
  const int w = d.width();
  const int scale = (w >= 200) ? 2 : 1;
  const int line = 8 * scale + 2;
  char hms[16];
  fmtHms((millis() - gConnSince) / 1000, hms, sizeof(hms));
  d.startWrite();
  d.fillRect(w - 2 - 8 * scale * 8, gStateLineY, 8 * scale * 8, line, gStatusBg);
  d.setTextSize(scale);
  d.setTextColor(TFT_WHITE, gStatusBg);
  d.setTextDatum(textdatum_t::top_right);
  d.drawString(hms, w - 2, gStateLineY);
  d.setTextDatum(textdatum_t::top_left);
  d.endWrite();
}

static void drawStatus(const BoardRefs& refs)
{
  auto& d = M5.Display;
  const int w = d.width();
  const int h = d.height();
  const int scale = (w >= 200) ? 2 : 1;
  const int line = 8 * scale + 2;
  bool flashing = (millis() < gFlashUntil) && gFlashPhase;
  uint16_t bg = flashing ? TFT_RED : TFT_BLACK;

  d.startWrite();
  d.fillScreen(bg);
  d.setTextSize(scale);
  d.setTextDatum(textdatum_t::top_left);

  int y = 2;
  d.setTextColor(TFT_YELLOW, bg);
  d.setCursor(2, y);
  d.print("Plus+ Stick");
  char buf[24];
  snprintf(buf, sizeof(buf), "%d%%", M5.Power.getBatteryLevel());
  d.setTextDatum(textdatum_t::top_right);
  d.drawString(buf, w - 2, y);
  d.setTextDatum(textdatum_t::top_left);
  y += line + 2;

  d.setTextColor(TFT_WHITE, bg);
  d.setCursor(2, y);
  d.print(stateText(refs));
  gStateLineY = y;
  gStatusBg = bg;
  drawLinkClock(refs);
  y += line;

  d.setTextColor(TFT_CYAN, bg);
  d.setCursor(2, y);
  d.print("Catch:");
  d.print(catchModeLabel(gCatchMode));
  d.print(" Spin:");
  d.print(onOff(gAutoSpin));
  y += line + 2;

  d.setTextColor(flashing ? TFT_WHITE : TFT_GREEN, bg);
  d.setCursor(2, y);
  d.print(gEvent);
  y += line;

  d.setTextColor(TFT_LIGHTGREY, bg);
  d.setCursor(2, y);
  snprintf(buf, sizeof(buf), "C:%u F:%u S:%u", gCountCaught, gCountFled, gCountSpins);
  d.print(buf);
  y += line;

  if (gNote[0]) {
    d.setTextColor(TFT_ORANGE, bg);
    d.setCursor(2, y);
    d.print(gNote);
  }

  d.setTextSize(1);
  d.setTextColor(TFT_DARKGREY, bg);
  d.setCursor(2, h - 10);
  d.print("A: press    B: menu");
  d.endWrite();
}

static void drawMenu()
{
  auto& d = M5.Display;
  const int w = d.width();
  const int h = d.height();
  const int scale = (w >= 200) ? 2 : 1;
  const int line = 8 * scale + 3;
  const int visible = (h - line - 12) / line;

  if (gMenuSel < gMenuScroll) gMenuScroll = gMenuSel;
  if (gMenuSel >= gMenuScroll + visible) gMenuScroll = gMenuSel - visible + 1;

  d.startWrite();
  d.fillScreen(TFT_BLACK);
  d.setTextSize(scale);
  d.setTextDatum(textdatum_t::top_left);
  d.setTextColor(TFT_YELLOW, TFT_BLACK);
  d.setCursor(2, 2);
  d.print("Menu");
  int y = 2 + line;
  for (int i = gMenuScroll; i < MI_COUNT && i < gMenuScroll + visible; ++i) {
    bool sel = (i == gMenuSel);
    uint16_t fg = sel ? TFT_BLACK : TFT_WHITE;
    uint16_t bg = sel ? TFT_YELLOW : TFT_BLACK;
    if (sel) d.fillRect(0, y - 1, w, line, bg);
    d.setTextColor(fg, bg);
    d.setCursor(2, y);
    d.print(sel ? ">" : " ");
    d.print(menuLabel(i));
    const char* v = menuValue(i);
    if (v[0]) {
      d.setTextDatum(textdatum_t::top_right);
      d.drawString(v, w - 4, y);
      d.setTextDatum(textdatum_t::top_left);
    }
    y += line;
  }
  d.setTextSize(1);
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  d.setCursor(2, h - 10);
  d.print("B: next   A: select   hold: back");
  d.endWrite();
}

static void drawConfirmReset()
{
  auto& d = M5.Display;
  const int w = d.width();
  const int scale = (w >= 200) ? 2 : 1;
  const int line = 8 * scale + 2;
  d.startWrite();
  d.fillScreen(TFT_BLACK);
  d.setTextSize(scale);
  d.setTextDatum(textdatum_t::top_left);
  d.setTextColor(TFT_RED, TFT_BLACK);
  d.setCursor(2, 2);
  d.print("Reset pairing?");
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setCursor(2, 2 + line + 4);
  d.print("Wipes saved pair");
  d.setCursor(2, 2 + 2 * line + 4);
  d.print("state, re-pair");
  d.setCursor(2, 2 + 3 * line + 4);
  d.print("from the app.");
  d.setTextColor(TFT_YELLOW, TFT_BLACK);
  d.setCursor(2, 2 + 5 * line);
  d.print("A: yes   B: no");
  d.endWrite();
}

static void drawInfo(const BoardRefs& refs)
{
  auto& d = M5.Display;
  const int w = d.width();
  const int scale = (w >= 200) ? 2 : 1;
  const int line = 8 * scale + 2;
  char addr[24];
  char own[24];
  addrToText(refs.donorAddrLe, addr, sizeof(addr));
  ppCompatOwnAddressText(own, sizeof(own));

  d.startWrite();
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(textdatum_t::top_left);
  d.setTextSize(scale);
  d.setTextColor(TFT_YELLOW, TFT_BLACK);
  d.setCursor(2, 2);
  d.print("Info");
  d.setTextSize(1);
  int y = 2 + line + 2;
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setCursor(2, y); d.print("Donor  "); d.print(addr); y += 10;
  d.setCursor(2, y); d.print("Radio  "); d.print(own); y += 10;
  d.setCursor(2, y); d.print("Stage  "); d.print(refs.pairingStageLabel(*refs.pairingStage)); y += 10;
  d.setCursor(2, y); d.print("Paired "); d.print(*refs.persistentReconnectReady ? "yes" : "no"); y += 10;
  char buf[48];
  snprintf(buf, sizeof(buf), "Caught %u  Fled %u  Spins %u  NoBalls %u", gCountCaught, gCountFled, gCountSpins, gCountNoBalls);
  d.setCursor(2, y); d.print(buf); y += 10;
  snprintf(buf, sizeof(buf), "Batt %d%%  Up %lus", M5.Power.getBatteryLevel(), (unsigned long) (millis() / 1000));
  d.setCursor(2, y); d.print(buf); y += 10;
  {
    char now[16], total[16];
    uint32_t cur = (*refs.activeConnHandle != BLE_CONN_HANDLE_INVALID && gConnSince) ? (millis() - gConnSince) / 1000 : 0;
    fmtHms(cur, now, sizeof(now));
    fmtHms(gLifetimeConnSec + cur, total, sizeof(total));
    snprintf(buf, sizeof(buf), "Link %s  Total %s", now, total);
    d.setCursor(2, y); d.print(buf); y += 10;
    snprintf(buf, sizeof(buf), "Last drop: %s (0x%02X)", dropReasonText(gLastDropReason), gLastDropReason);
    d.setCursor(2, y); d.print(buf); y += 10;
  }
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  d.setCursor(2, y); d.print(refs.buildTag ? refs.buildTag : ""); y += 10;
  d.setCursor(2, d.height() - 10);
  d.print("any button: back");
  d.endWrite();
}

}  // namespace

// ---------------- public API ----------------


void ppBoardInit()
{
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.output_power = false;
  M5.begin(cfg);
  if (M5.Display.height() > M5.Display.width()) {
    M5.Display.setRotation(1);
  }
  pinMode(PP_BOARD_PIN_BTN_FRONT, INPUT_PULLUP);
  pinMode(PP_BOARD_PIN_BTN_SIDE, INPUT_PULLUP);
  loadSettings();
  applyBrightness();
  if (M5.Speaker.isEnabled()) {
    M5.Speaker.setVolume(96);
  }
  gLastInputAt = millis();
  gDirty = true;
}

bool ppBoardAutoSpin() { return gAutoSpin; }
uint8_t ppBoardPressSpeed() { return gSpeed; }
uint8_t ppBoardCatchMode() { return gCatchMode; }
bool ppBoardAdvertiseEnabled() { return gAdvertise; }

void ppBoardOnEvent(PPLedEvent evt, const char* label)
{
  strncpy(gEvent, label ? label : "-", sizeof(gEvent) - 1);
  gEvent[sizeof(gEvent) - 1] = 0;
  gDirty = true;
  Serial.print("Board event: ");
  Serial.println(gEvent);

  switch (evt) {
    case PP_LED_POKEMON: startBeeps(kBeepPokemon, 1); break;
    case PP_LED_NEW_POKEMON: startBeeps(kBeepNewPokemon, 3); break;
    case PP_LED_POKESTOP: startBeeps(kBeepPokestop, 1); break;
    case PP_LED_CAUGHT: ++gCountCaught; startBeeps(kBeepCaught, 5); break;
    case PP_LED_FLED: ++gCountFled; startBeeps(kBeepFled, 3); break;
    case PP_LED_ITEMS: ++gCountSpins; startBeeps(kBeepItems, 3); break;
    case PP_LED_BALLS_EMPTY:
      ++gCountNoBalls;
      startBeeps(kBeepWarning, 11);
      gFlashUntil = millis() + 6000;
      break;
    case PP_LED_BAG_FULL:
    case PP_LED_BOX_FULL:
      startBeeps(kBeepWarning, 11);
      gFlashUntil = millis() + 6000;
      break;
    default: break;
  }
}

void ppBoardOnDisconnect(uint8_t reason)
{
  gLastDropReason = reason;
  gPrefs.putUChar("dropr", reason);
  if (reason != 0x16) {  // a drop we asked for (pairing reset) keeps its own note
    char note[32];
    snprintf(note, sizeof(note), "Drop: %s", dropReasonText(reason));
    ppBoardSetNote(note);
  }
  Serial.print("Link dropped: ");
  Serial.print(dropReasonText(reason));
  Serial.print(" (0x");
  Serial.print(reason, HEX);
  Serial.println(")");
}

void ppBoardSetNote(const char* text)
{
  strncpy(gNote, text ? text : "", sizeof(gNote) - 1);
  gNote[sizeof(gNote) - 1] = 0;
  gNoteAt = millis();
  gDirty = true;
}

void ppBoardTick(const BoardRefs& refs)
{
  M5.update();
  {
    bool connected = (*refs.activeConnHandle != BLE_CONN_HANDLE_INVALID);
    if (connected && !gWasConnected) {
      gConnSince = millis() | 1;  // never the 0 sentinel
    } else if (!connected && gWasConnected && gConnSince) {
      gLifetimeConnSec += (millis() - gConnSince) / 1000;
      gPrefs.putUInt("conn_s", gLifetimeConnSec);
      gConnSince = 0;
    }
    gWasConnected = connected;
    static uint32_t lastClock = 0;
    if (connected && gScreen == SCREEN_STATUS && !gDirty && !gDisplayDimmed && (millis() - lastClock) >= 1000) {
      lastClock = millis();
      drawLinkClock(refs);  // repaint only the clock cell, no full redraw
    }
  }
  serviceBeeps();
  uint32_t now = millis();

  bool aClick = M5.BtnA.wasClicked();
  bool bClick = M5.BtnB.wasClicked();
  bool aHold = M5.BtnA.wasHold();
  bool bHold = M5.BtnB.wasHold();
  bool anyInput = aClick || bClick || aHold || bHold || M5.BtnA.isPressed() || M5.BtnB.isPressed();
  if (anyInput) {
    gLastInputAt = now;
    if (gScreen != SCREEN_STATUS) gMenuLastInput = now;
  }

  switch (gScreen) {
    case SCREEN_STATUS:
      if (aClick) {
        if (*refs.pairingStage == PAIR_STAGE_AUTHENTICATED && *refs.activeConnHandle != BLE_CONN_HANDLE_INVALID) {
          Serial.println("Front button -> manual press");
          ppBoardSetNote("Manual press");
          refs.manualButtonPress();
        } else {
          ppBoardSetNote("Not connected");
        }
      }
      if (bClick) {
        openMenu();
      }
      break;

    case SCREEN_MENU:
      if (bClick) {
        gMenuSel = (gMenuSel + 1) % MI_COUNT;
        gDirty = true;
      } else if (aClick) {
        menuSelect(refs);
      } else if (aHold || bHold) {
        closeMenu();
      } else if ((now - gMenuLastInput) > kMenuTimeoutMs) {
        closeMenu();
      }
      break;

    case SCREEN_CONFIRM_RESET:
      if (aClick) {
        Serial.println("Menu -> reset pairing confirmed");
        refs.resetPairing("menu");
        ppBoardSetNote("Pairing reset");
        closeMenu();
      } else if (bClick || aHold || bHold || (now - gMenuLastInput) > kMenuTimeoutMs) {
        gScreen = SCREEN_MENU;
        gDirty = true;
      }
      break;

    case SCREEN_INFO:
      if (aClick || bClick || aHold || bHold || (now - gMenuLastInput) > 30000) {
        gScreen = SCREEN_MENU;
        gDirty = true;
      } else if ((now - gLastDraw) > 1000) {
        gDirty = true;  // live uptime/battery
      }
      break;
  }

  // Dim the backlight after a minute of no input; any press brings it back.
  bool wantDim = (now - gLastInputAt) >= kDimAfterMs;
  if (wantDim != gDisplayDimmed) {
    gDisplayDimmed = wantDim;
    applyBrightness();
  }

  // Clear a stale note after a few seconds.
  if (gNote[0] && (now - gNoteAt) > 4000) {
    gNote[0] = 0;
    gDirty = true;
  }

  // Warning flash toggles the status background.
  if (now < gFlashUntil) {
    if ((now - gFlashLastToggle) > 250) {
      gFlashLastToggle = now;
      gFlashPhase = !gFlashPhase;
      gDirty = true;
    }
  } else if (gFlashPhase) {
    gFlashPhase = false;
    gDirty = true;
  }

  if (gScreen == SCREEN_STATUS) {
    const char* st = stateText(refs);
    if (strcmp(st, gLastState) != 0) {
      strncpy(gLastState, st, sizeof(gLastState) - 1);
      gDirty = true;
    }
    if ((now - gLastDraw) > 10000) {
      int batt = M5.Power.getBatteryLevel();
      if (batt != gLastBattery) {
        gLastBattery = batt;
        gDirty = true;
      }
    }
  }

  if (gDirty && (now - gLastDraw) > 120) {
    switch (gScreen) {
      case SCREEN_MENU: drawMenu(); break;
      case SCREEN_CONFIRM_RESET: drawConfirmReset(); break;
      case SCREEN_INFO: drawInfo(refs); break;
      default: drawStatus(refs); break;
    }
    gDirty = false;
    gLastDraw = now;
  }
}

void ppBoardEnterDeepSleep()
{
  if (gConnSince) {  // fold the in-flight session into the lifetime total
    gLifetimeConnSec += (millis() - gConnSince) / 1000;
    gPrefs.putUInt("conn_s", gLifetimeConnSec);
    gConnSince = 0;
    gWasConnected = false;
  }
  Serial.println("Entering deep sleep; wake with either button");
  Serial.flush();
  M5.Display.setBrightness(0);
  M5.Display.sleep();
  delay(20);

  // Both buttons are active-low with pull-ups and both pins are RTC capable.
  rtc_gpio_pullup_en(static_cast<gpio_num_t>(PP_BOARD_PIN_BTN_FRONT));
  rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(PP_BOARD_PIN_BTN_FRONT));
  rtc_gpio_pullup_en(static_cast<gpio_num_t>(PP_BOARD_PIN_BTN_SIDE));
  rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(PP_BOARD_PIN_BTN_SIDE));
  uint64_t mask = (1ULL << PP_BOARD_PIN_BTN_FRONT) | (1ULL << PP_BOARD_PIN_BTN_SIDE);
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
}

// nrf_soc.h stand-in: the firmware's "System OFF" is the board's deep sleep.
uint32_t sd_power_system_off(void)
{
  ppBoardEnterDeepSleep();
  return 1;  // only reached if deep sleep failed
}
