#pragma once
// ESP32 port: the nRF version hooked the raw SoftDevice event stream to log
// writes and authorize requests. Here the compat layer logs those at the
// PP_LOG_LEVEL_RAW_BLE level itself, so these classes only carry the label.

#include "bluefruit.h"

#include "pp_log.h"

class DebugBLEService : public BLEService
{
public:
  DebugBLEService(BLEUuid bleuuid) : BLEService(bleuuid) {}
  DebugBLEService(uint16_t uuid16) : BLEService(uuid16) {}
  uint16_t handleValue() const { return _handle; }
};

class DebugBLECharacteristic : public BLECharacteristic
{
public:
  DebugBLECharacteristic(BLEUuid bleuuid, const char* label)
    : BLECharacteristic(bleuuid)
  {
    _debugLabel = label;
  }
  DebugBLECharacteristic(uint16_t uuid16, const char* label)
    : BLECharacteristic(uuid16)
  {
    _debugLabel = label;
  }
};
