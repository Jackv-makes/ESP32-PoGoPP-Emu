// ESP32 port: Adafruit_LittleFS-compatible file API over NVS Preferences.

#include "InternalFileSystem.h"

#include <Preferences.h>

Adafruit_LittleFS_Namespace::PPInternalFS InternalFS;

namespace {

static Preferences gPrefs;
static const char kNamespace[] = "ppemu";

static void pathToKey(const char* path, char* key, size_t keyLen)
{
  // NVS keys are at most 15 characters. Strip the leading slash and the
  // extension: "/pair_state.bin" -> "pair_state".
  const char* p = path;
  while (*p == '/') {
    ++p;
  }
  size_t n = 0;
  while (*p && *p != '.' && n + 1 < keyLen) {
    key[n++] = *p++;
  }
  key[n] = 0;
}

}  // namespace

namespace Adafruit_LittleFS_Namespace {

bool PPInternalFS::begin()
{
  if (_ready) {
    return true;
  }
  _ready = gPrefs.begin(kNamespace, false);
  return _ready;
}

bool PPInternalFS::remove(const char* path)
{
  if (!_ready) {
    return false;
  }
  char key[16];
  pathToKey(path, key, sizeof(key));
  if (!gPrefs.isKey(key)) {
    return false;
  }
  return gPrefs.remove(key);
}

File::File(PPInternalFS& fs) : _fs(fs), _open(false), _mode(FILE_O_READ), _len(0), _pos(0)
{
  _key[0] = 0;
}

File::~File()
{
  close();
}

bool File::open(const char* path, uint8_t mode)
{
  if (!_fs.ready()) {
    return false;
  }
  pathToKey(path, _key, sizeof(_key));
  _mode = mode;
  _len = 0;
  _pos = 0;
  if (mode == FILE_O_READ) {
    if (!gPrefs.isKey(_key)) {
      return false;
    }
    size_t stored = gPrefs.getBytesLength(_key);
    if (stored > sizeof(_buf)) {
      stored = sizeof(_buf);
    }
    _len = gPrefs.getBytes(_key, _buf, stored);
  }
  _open = true;
  return true;
}

uint32_t File::write(const uint8_t* data, uint32_t len)
{
  if (!_open || _mode != FILE_O_WRITE) {
    return 0;
  }
  uint32_t room = sizeof(_buf) - _len;
  if (len > room) {
    len = room;
  }
  memcpy(_buf + _len, data, len);
  _len += len;
  return len;
}

uint32_t File::read(uint8_t* buffer, uint32_t len)
{
  if (!_open || _mode != FILE_O_READ) {
    return 0;
  }
  uint32_t avail = _len - _pos;
  if (len > avail) {
    len = avail;
  }
  memcpy(buffer, _buf + _pos, len);
  _pos += len;
  return len;
}

void File::close()
{
  if (!_open) {
    return;
  }
  if (_mode == FILE_O_WRITE) {
    gPrefs.putBytes(_key, _buf, _len);
  }
  _open = false;
}

}  // namespace Adafruit_LittleFS_Namespace
