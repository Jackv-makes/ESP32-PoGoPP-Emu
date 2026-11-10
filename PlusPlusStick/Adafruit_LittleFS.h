#pragma once
// ESP32 port: Adafruit_LittleFS-compatible file API backed by NVS Preferences.
//
// The firmware persists two small records ("/pair_state.bin" and
// "/sleep_regen.bin") with open/write/read/close/remove. Preferences keeps them
// in the NVS partition, so no filesystem partition layout is needed and the
// Stickwalker save area on this device is never touched.

#include <Arduino.h>
#include <stdint.h>

namespace Adafruit_LittleFS_Namespace {

enum {
  FILE_O_READ = 0,
  FILE_O_WRITE = 1,
};

class PPInternalFS
{
public:
  bool begin();
  bool remove(const char* path);
  bool ready() const { return _ready; }

private:
  bool _ready = false;
};

class File
{
public:
  explicit File(PPInternalFS& fs);
  ~File();

  bool open(const char* path, uint8_t mode);
  uint32_t write(const uint8_t* data, uint32_t len);
  uint32_t read(uint8_t* buffer, uint32_t len);
  void close();
  operator bool() const { return _open; }

private:
  PPInternalFS& _fs;
  bool _open;
  uint8_t _mode;
  char _key[16];
  uint8_t _buf[256];
  uint32_t _len;
  uint32_t _pos;
};

}  // namespace Adafruit_LittleFS_Namespace
