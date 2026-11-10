// ESP32 port: Bluefruit facade implementation over NimBLE-Arduino 2.x.
//
// Threading note: NimBLE invokes the characteristic/server callbacks on its
// host task, the same arrangement Bluefruit had with its BLE event task, so
// the protocol modules see the same concurrency model they were written for.

// NimBLEDevice.h defines Arduino-BLE compatibility macros (BLEService ->
// NimBLEService, ...) that collide with the facade class names, so include it
// first and drop those macros before pulling in the facade header.
#include <NimBLEDevice.h>
#undef BLEService
#undef BLECharacteristic
#undef BLEServer
#undef BLEAdvertising
#undef BLEServerCallbacks
#undef BLECharacteristicCallbacks
#undef BLEAdvertisementData
#undef BLEDescriptor
#undef BLEConnInfo
#undef BLEUUID
#undef BLEAddress
#undef BLEDevice

#include "bluefruit.h"
#include <esp_mac.h>
#include "pp_aes128_soft.h"
#ifndef PP_USE_SOFT_AES
#define PP_USE_SOFT_AES 1
#endif
#include <nimble/nimble/host/services/gatt/include/services/gatt/ble_svc_gatt.h>
#include <mbedtls/aes.h>
#include <vector>

#include "pp_log.h"

BluefruitCompat Bluefruit;

namespace {

struct AdvField {
  std::vector<uint8_t> bytes;
};

static bool gInitRequested = false;
static bool gInitDone = false;
static bool gServicesFinalized = false;
static char gDeviceName[32] = "PlusPlus";
static uint8_t gTargetAddr[6] = { 0 };
static bool gTargetAddrSet = false;
static uint16_t gMtu = 517;

static NimBLEServer* gServer = nullptr;
static BLEService* gCurrentService = nullptr;
static std::vector<BLEService*> gServices;
static std::vector<BLECharacteristic*> gChars;

static uint16_t gConnHandle = BLE_CONN_HANDLE_INVALID;
static BluefruitCompat::connect_cb_t gConnectCb = nullptr;
static BluefruitCompat::disconnect_cb_t gDisconnectCb = nullptr;
static uint16_t gConnIntervalMin = 6;
static uint16_t gConnIntervalMax = 24;
static uint16_t gConnTimeout = 500;

static std::vector<uint8_t> gAdvPayload;
static bool gScanRespName = false;
static uint16_t gAdvInterval = 160;
static bool gRestartOnDisconnect = false;
static uint8_t gAdvFlags = 0;
static bool gAdvFlagsSet = false;

static BLECharacteristic* findCompat(NimBLECharacteristic* chr)
{
  for (BLECharacteristic* c : gChars) {
    if (c->nim() == chr) {
      return c;
    }
  }
  return nullptr;
}

static BLECharacteristic* findByValueHandle(uint16_t handle)
{
  for (BLECharacteristic* c : gChars) {
    if (c->nim() != nullptr && c->handles().value_handle == handle) {
      return c;
    }
  }
  return nullptr;
}

class CompatCharCallbacks : public NimBLECharacteristicCallbacks
{
public:
  void onRead(NimBLECharacteristic* chr, NimBLEConnInfo& connInfo) override
  {
    BLECharacteristic* c = findCompat(chr);
    if (c) {
      c->_compatOnRead(connInfo.getConnHandle());
    }
  }

  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& connInfo) override
  {
    BLECharacteristic* c = findCompat(chr);
    if (c) {
      NimBLEAttValue v = chr->getValue();
      c->_compatOnWrite(connInfo.getConnHandle(), v.data(), v.length());
    }
  }

  void onSubscribe(NimBLECharacteristic* chr, NimBLEConnInfo& connInfo, uint16_t subValue) override
  {
    BLECharacteristic* c = findCompat(chr);
    if (c) {
      c->_compatOnSubscribe(connInfo.getConnHandle(), subValue);
    }
  }
};

class CompatServerCallbacks : public NimBLEServerCallbacks
{
public:
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override
  {
    gConnHandle = connInfo.getConnHandle();
    for (BLECharacteristic* c : gChars) {
      c->_compatSetSubscribed(false);
      c->_compatSetSeeded(false);
    }
    // Plus+ connection preferences: 7.5-30 ms interval, 5 s supervision timeout.
    server->updateConnParams(gConnHandle, gConnIntervalMin, gConnIntervalMax, 0, gConnTimeout);
    if (gConnectCb) {
      gConnectCb(gConnHandle);
    }
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override
  {
    (void) server;
    uint16_t handle = connInfo.getConnHandle();
    gConnHandle = BLE_CONN_HANDLE_INVALID;
    for (BLECharacteristic* c : gChars) {
      c->_compatSetSubscribed(false);
      c->_compatSetSeeded(false);
    }
    if (gDisconnectCb) {
      // NimBLE reports HCI reasons as BLE_HS_ERR_HCI_BASE + code; anything else
      // (host/stack reset) is not an HCI reason and is reported as 0xFF.
      uint8_t r = (reason >= BLE_HS_ERR_HCI_BASE && reason < BLE_HS_ERR_HCI_BASE + 0x100)
                      ? static_cast<uint8_t>(reason - BLE_HS_ERR_HCI_BASE) : 0xFF;
      gDisconnectCb(handle, r);
    }
    if (gRestartOnDisconnect) {
      NimBLEDevice::getAdvertising()->start(0);
    }
  }

  void onMTUChange(uint16_t mtu, NimBLEConnInfo& connInfo) override
  {
    (void) connInfo;
    Serial.print("MTU changed -> ");
    Serial.println(mtu);
  }
};

static CompatCharCallbacks gCharCallbacks;
static CompatServerCallbacks gServerCallbacks;

static void applyCloneAddress()
{
  if (!gTargetAddrSet) {
    return;
  }
  // Set the Bluetooth interface address directly (IDF 5 per-interface
  // override). This bypasses the base-MAC + chip offset scheme and the MAC
  // cache: reading the BT address before setting the base MAC caches the
  // factory value, which the controller then keeps. gTargetAddr is
  // little-endian (SoftDevice order); the IDF API takes the printed order.
  uint8_t be[6];
  for (int i = 0; i < 6; i++) {
    be[i] = gTargetAddr[5 - i];
  }
  esp_err_t err = esp_iface_mac_addr_set(be, ESP_MAC_BT);
  Serial.print("Clone address: BT interface MAC set -> err=");
  Serial.println(static_cast<int>(err));
}


// GAP-level hook: NimBLE only routes subscribe events for characteristics it
// created for us. Log every CCCD the phone touches (a phone replaying a cached
// attribute table from the real Plus+ subscribes to handles we do not own) and
// answer a subscription to the GATT Service Changed characteristic with a
// Service Changed indication so the phone drops that cache and re-discovers.
static int compatGapEventHook(ble_gap_event* event, void* arg)
{
  (void) arg;
  if (event->type == BLE_GAP_EVENT_SUBSCRIBE) {
    uint16_t h = event->subscribe.attr_handle;
    static const ble_uuid16_t kGattSvcUuid = BLE_UUID16_INIT(0x1801);
    static const ble_uuid16_t kSvcChangedUuid = BLE_UUID16_INIT(0x2A05);
    uint16_t scDef = 0, scVal = 0;
    ble_gatts_find_chr(&kGattSvcUuid.u, &kSvcChangedUuid.u, &scDef, &scVal);
    if (scVal != 0 && h == scVal && event->subscribe.cur_indicate) {
      ble_svc_gatt_changed(0x0001, 0xffff);
      Serial.println("[gap] Service Changed indication sent (full range)");
    }
  }
  return 0;
}

static void ensureInit()
{
  if (gInitDone) {
    return;
  }
  applyCloneAddress();
  NimBLEDevice::init(gDeviceName);
  NimBLEDevice::setDeviceName(gDeviceName);
  NimBLEDevice::setMTU(gMtu);
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);
  gServer = NimBLEDevice::createServer();
  gServer->setCallbacks(&gServerCallbacks);
  gServer->advertiseOnDisconnect(false);
  NimBLEDevice::setCustomGapHandler(compatGapEventHook);
  gInitDone = true;

  NimBLEAddress own = NimBLEDevice::getAddress();
  Serial.print("NimBLE own address: ");
  Serial.print(own.toString().c_str());
  Serial.print(" type=");
  Serial.println(own.getType());
  if (gTargetAddrSet) {
    const uint8_t* got = own.getBase()->val;  // NimBLE stores little-endian
    bool match = (memcmp(got, gTargetAddr, 6) == 0);
    Serial.print("Clone address check: ");
    Serial.println(match ? "MATCH" : "MISMATCH (app will not recognise this device)");
  }
}

static NimBLEUUID toNim(const BLEUuid& u)
{
  if (u.is16()) {
    return NimBLEUUID(u.uuid16());
  }
  return NimBLEUUID(u.str());
}

static uint32_t nimProps(uint8_t props)
{
  uint32_t p = 0;
  if (props & CHR_PROPS_READ) p |= NIMBLE_PROPERTY::READ;
  if (props & CHR_PROPS_WRITE) p |= NIMBLE_PROPERTY::WRITE;
  if (props & CHR_PROPS_WRITE_WO_RESP) p |= NIMBLE_PROPERTY::WRITE_NR;
  if (props & CHR_PROPS_NOTIFY) p |= NIMBLE_PROPERTY::NOTIFY;
  if (props & CHR_PROPS_INDICATE) p |= NIMBLE_PROPERTY::INDICATE;
  if (props & CHR_PROPS_BROADCAST) p |= NIMBLE_PROPERTY::BROADCAST;
  return p;
}

}  // namespace

// ---------------- BLEService ----------------

BLEService::BLEService(BLEUuid bleuuid) : _handle(0), _uuid(bleuuid), _svc(nullptr) {}
BLEService::BLEService(uint16_t uuid16) : _handle(0), _uuid(uuid16), _svc(nullptr) {}

void BLEService::begin()
{
  ensureInit();
  if (_svc == nullptr) {
    _svc = gServer->createService(toNim(_uuid));
    gServices.push_back(this);
  }
  gCurrentService = this;
}

// ---------------- BLECharacteristic ----------------

BLECharacteristic::BLECharacteristic(BLEUuid bleuuid)
  : _debugLabel(nullptr), _uuid(bleuuid), _props(CHR_PROPS_READ), _maxLen(20), _fixedLen(false),
    _subscribed(false), _seeded(false), _value(nullptr), _valueLen(0), _valueCap(0), _chr(nullptr),
    _writeCb(nullptr), _cccdCb(nullptr), _readAuthCb(nullptr) {}

BLECharacteristic::BLECharacteristic(uint16_t uuid16)
  : _debugLabel(nullptr), _uuid(uuid16), _props(CHR_PROPS_READ), _maxLen(20), _fixedLen(false),
    _subscribed(false), _seeded(false), _value(nullptr), _valueLen(0), _valueCap(0), _chr(nullptr),
    _writeCb(nullptr), _cccdCb(nullptr), _readAuthCb(nullptr) {}

BLECharacteristic::~BLECharacteristic()
{
  free(_value);
}

void BLECharacteristic::setProperties(uint8_t prop) { _props = prop; }
void BLECharacteristic::setPermission(SecureMode_t read_perm, SecureMode_t write_perm)
{
  (void) read_perm;
  (void) write_perm;
}
// The value buffer must track the largest size ever requested: the boot
// sequence writes some characteristics (auth blob, long-read blob) before the
// topology sets their max length, and the Bluefruit API allows setMaxLen after
// a write. Allocating at the default 20 bytes and never growing was a heap
// overflow on every later full-length write.
void BLECharacteristic::_ensureValueCapacity(uint16_t needed)
{
  if (needed == 0) {
    needed = 1;
  }
  if (_value != nullptr && _valueCap >= needed) {
    return;
  }
  uint8_t* grown = static_cast<uint8_t*>(realloc(_value, needed));
  if (grown == nullptr) {
    Serial.println("BLECharacteristic: value realloc failed");
    return;
  }
  if (needed > _valueCap) {
    memset(grown + _valueCap, 0, needed - _valueCap);
  }
  _value = grown;
  _valueCap = needed;
}

void BLECharacteristic::setMaxLen(uint16_t max_len)
{
  _maxLen = max_len;
  _fixedLen = false;
  _ensureValueCapacity(_maxLen);
}
void BLECharacteristic::setFixedLen(uint16_t fixed_len)
{
  _maxLen = fixed_len;
  _fixedLen = true;
  _ensureValueCapacity(_maxLen);
}
void BLECharacteristic::setWriteCallback(write_cb_t fp) { _writeCb = fp; }
void BLECharacteristic::setCccdWriteCallback(cccd_cb_t fp) { _cccdCb = fp; }
void BLECharacteristic::setReadAuthorizeCallback(read_authorize_cb_t fp) { _readAuthCb = fp; }

void BLECharacteristic::begin()
{
  ensureInit();
  if (_value == nullptr) {
    _ensureValueCapacity(_maxLen);
    _valueLen = 0;
  }
  if (_chr != nullptr) {
    return;
  }
  if (gCurrentService == nullptr || gCurrentService->nim() == nullptr) {
    Serial.println("BLECharacteristic::begin without a service");
    return;
  }
  _chr = gCurrentService->nim()->createCharacteristic(toNim(_uuid), nimProps(_props), _maxLen);
  _chr->setCallbacks(&gCharCallbacks);
  if (_valueLen > 0) {
    _chr->setValue(_value, _valueLen);
  }
  gChars.push_back(this);
}

uint16_t BLECharacteristic::write(const void* data, uint16_t len)
{
  if (len > _maxLen) {
    len = _maxLen;
  }
  _ensureValueCapacity(_maxLen);
  if (_value == nullptr || _valueCap < len) {
    return 0;
  }
  if (data != nullptr && len > 0) {
    memcpy(_value, data, len);
  }
  _valueLen = len;
  if (_chr != nullptr) {
    _chr->setValue(_value, _valueLen);
  }
  return len;
}

uint16_t BLECharacteristic::read(void* buffer, uint16_t len)
{
  if (_chr != nullptr) {
    NimBLEAttValue v = _chr->getValue();
    uint16_t n = v.length();
    if (n > len) n = len;
    memcpy(buffer, v.data(), n);
    return n;
  }
  uint16_t n = _valueLen;
  if (n > len) n = len;
  if (n > 0) memcpy(buffer, _value, n);
  return n;
}

bool BLECharacteristic::notifyEnabled() const
{
  return (_subscribed || _seeded) && gConnHandle != BLE_CONN_HANDLE_INVALID;
}

bool BLECharacteristic::notify(const void* data, uint16_t len)
{
  write(data, len);
  if (_chr == nullptr || !(_props & (CHR_PROPS_NOTIFY | CHR_PROPS_INDICATE))) {
    return false;
  }
  if (!notifyEnabled()) {
    return false;
  }
  // Bluefruit splits long values over several notifications of MTU-3 bytes.
  uint16_t mtu = gServer ? gServer->getPeerMTU(gConnHandle) : 23;
  if (mtu < 23) mtu = 23;
  uint16_t maxPayload = mtu - 3;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint16_t remaining = len;
  bool ok = true;
  if (remaining == 0) {
    return _chr->notify(gConnHandle);
  }
  while (remaining > 0) {
    uint16_t chunk = remaining > maxPayload ? maxPayload : remaining;
    if (!_chr->notify(p, chunk, gConnHandle)) {
      ok = false;
      break;
    }
    p += chunk;
    remaining -= chunk;
  }
  return ok;
}

ble_gatts_char_handles_t BLECharacteristic::handles() const
{
  ble_gatts_char_handles_t h = { 0, 0, 0, 0 };
  if (_chr != nullptr) {
    h.value_handle = _chr->getHandle();
    // NimBLE places the auto-generated CCCD right after the value attribute.
    h.cccd_handle = (_props & (CHR_PROPS_NOTIFY | CHR_PROPS_INDICATE)) ? static_cast<uint16_t>(h.value_handle + 1)
                                                                        : BLE_GATT_HANDLE_INVALID;
  }
  return h;
}

void BLECharacteristic::_compatOnWrite(uint16_t conn_hdl, const uint8_t* data, uint16_t len)
{
  if (PP_LOG_RAW_BLE_ENABLED) {
    Serial.print("[RAW] ");
    Serial.print(_debugLabel ? _debugLabel : "chr");
    Serial.print(" evt=WRITE handle=0x");
    Serial.print(handles().value_handle, HEX);
    Serial.print(" len=");
    Serial.println(len);
  }
  // Keep the local mirror in sync with what the peer wrote.
  uint16_t n = len > _maxLen ? _maxLen : len;
  if (_value && n > 0) {
    memcpy(_value, data, n);
    _valueLen = n;
  }
  if (_writeCb) {
    uint8_t tmp[512];
    uint16_t m = len > sizeof(tmp) ? sizeof(tmp) : len;
    memcpy(tmp, data, m);
    _writeCb(conn_hdl, this, tmp, m);
  }
}

void BLECharacteristic::_compatOnRead(uint16_t conn_hdl)
{
  if (PP_LOG_RAW_BLE_ENABLED) {
    Serial.print("[RAW] ");
    Serial.print(_debugLabel ? _debugLabel : "chr");
    Serial.print(" evt=READ_AUTH handle=0x");
    Serial.println(handles().value_handle, HEX);
  }
  if (_readAuthCb) {
    ble_gatts_evt_read_t req;
    req.handle = handles().value_handle;
    req.offset = 0;
    _readAuthCb(conn_hdl, this, &req);
  }
}

void BLECharacteristic::_compatOnSubscribe(uint16_t conn_hdl, uint16_t value)
{
  _subscribed = (value & 0x0003) != 0;
  if (PP_LOG_RAW_BLE_ENABLED) {
    Serial.print("[RAW] ");
    Serial.print(_debugLabel ? _debugLabel : "chr");
    Serial.print(" evt=CCCD handle=0x");
    Serial.print(handles().cccd_handle, HEX);
    Serial.print(" value=0x");
    Serial.println(value, HEX);
  }
  if (_cccdCb) {
    _cccdCb(conn_hdl, this, value);
  }
}

// ---------------- Bluefruit singleton ----------------

void BluefruitCompat::configServiceChanged(bool changed) { (void) changed; }
void BluefruitCompat::configUuid128Count(uint8_t count) { (void) count; }
void BluefruitCompat::configAttrTableSize(uint32_t size) { (void) size; }
void BluefruitCompat::configPrphConn(uint16_t mtu_max, uint16_t event_len, uint8_t hvn_qsize, uint8_t wrcmd_qsize)
{
  (void) event_len;
  (void) hvn_qsize;
  (void) wrcmd_qsize;
  gMtu = mtu_max;
}

bool BluefruitCompat::begin(uint8_t prph_count, uint8_t central_count)
{
  (void) prph_count;
  (void) central_count;
  // Real controller init is deferred to the first service/advertising call so
  // that setAddr(), which the firmware calls after begin(), can still change
  // the base MAC before the controller reads it.
  gInitRequested = true;
  return true;
}

void BluefruitCompat::setName(const char* name)
{
  strncpy(gDeviceName, name, sizeof(gDeviceName) - 1);
  gDeviceName[sizeof(gDeviceName) - 1] = 0;
  if (gInitDone) {
    NimBLEDevice::setDeviceName(gDeviceName);
  }
}

void BluefruitCompat::setAddr(ble_gap_addr_t* addr)
{
  if (addr == nullptr) {
    return;
  }
  memcpy(gTargetAddr, addr->addr, 6);
  gTargetAddrSet = true;
  if (gInitDone) {
    Serial.println("setAddr after init: address cannot change any more");
  }
}

void BluefruitCompat::setAppearance(uint16_t appear) { (void) appear; }

bool BluefruitCompat::connected() const { return gConnHandle != BLE_CONN_HANDLE_INVALID; }
uint16_t BluefruitCompat::connHandle() const { return gConnHandle; }

// Periph
void BluefruitCompat::PeriphCompat::setConnectCallback(connect_cb_t fp) { gConnectCb = fp; }
void BluefruitCompat::PeriphCompat::setDisconnectCallback(disconnect_cb_t fp) { gDisconnectCb = fp; }
void BluefruitCompat::PeriphCompat::setConnInterval(uint16_t min, uint16_t max)
{
  gConnIntervalMin = min;
  gConnIntervalMax = max;
}
void BluefruitCompat::PeriphCompat::setConnSupervisionTimeout(uint16_t timeout) { gConnTimeout = timeout; }

// Advertising
void BluefruitCompat::AdvertisingCompat::stop()
{
  // Only send the HCI stop when actually advertising: stopping before the
  // first start raced the controller and crashed in the HCI queue on boot.
  if (gInitDone) {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (adv->isAdvertising()) {
      adv->stop();
    }
  }
}
void BluefruitCompat::AdvertisingCompat::clearData()
{
  gAdvPayload.clear();
  gAdvFlagsSet = false;
}
void BluefruitCompat::AdvertisingCompat::setType(uint8_t type) { (void) type; }
void BluefruitCompat::AdvertisingCompat::addFlags(uint8_t flags)
{
  gAdvFlags = flags;
  gAdvFlagsSet = true;
}
void BluefruitCompat::AdvertisingCompat::addManufacturerData(const void* data, uint16_t len)
{
  addData(0xFF, data, len);
}
void BluefruitCompat::AdvertisingCompat::addData(uint8_t type, const void* data, uint16_t len)
{
  gAdvPayload.push_back(static_cast<uint8_t>(len + 1));
  gAdvPayload.push_back(type);
  const uint8_t* p = static_cast<const uint8_t*>(data);
  gAdvPayload.insert(gAdvPayload.end(), p, p + len);
}
void BluefruitCompat::AdvertisingCompat::restartOnDisconnect(bool enable) { gRestartOnDisconnect = enable; }
void BluefruitCompat::AdvertisingCompat::setInterval(uint16_t fast, uint16_t slow)
{
  (void) slow;
  gAdvInterval = fast;
}
void BluefruitCompat::AdvertisingCompat::setFastTimeout(uint16_t sec) { (void) sec; }

bool BluefruitCompat::AdvertisingCompat::start(uint16_t timeout)
{
  (void) timeout;
  ensureInit();
  ppCompatFinalizeServices();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();

  NimBLEAdvertisementData advData;
  if (gAdvFlagsSet) {
    advData.setFlags(gAdvFlags);
  }
  if (!gAdvPayload.empty()) {
    advData.addData(gAdvPayload.data(), gAdvPayload.size());
  }
  adv->setAdvertisementData(advData);

  NimBLEAdvertisementData scanData;
  if (gScanRespName) {
    scanData.setName(gDeviceName);
  }
  adv->setScanResponseData(scanData);
  adv->enableScanResponse(true);
  adv->setMinInterval(gAdvInterval);
  adv->setMaxInterval(gAdvInterval);
  adv->setConnectableMode(BLE_GAP_CONN_MODE_UND);

  bool ok = adv->start(0);
  Serial.print("Advertising start -> ");
  Serial.println(ok ? "ok" : "failed");
  return ok;
}

// Scan response
void BluefruitCompat::ScanResponseCompat::clearData() { gScanRespName = false; }
void BluefruitCompat::ScanResponseCompat::addName() { gScanRespName = true; }

// ---------------- raw SoftDevice calls ----------------

uint32_t sd_ble_gap_device_name_set(const ble_gap_conn_sec_mode_t* perm, const uint8_t* name, uint16_t len)
{
  (void) perm;
  char buf[32];
  uint16_t n = len >= sizeof(buf) ? sizeof(buf) - 1 : len;
  memcpy(buf, name, n);
  buf[n] = 0;
  Bluefruit.setName(buf);
  return NRF_SUCCESS;
}

uint32_t sd_ble_gap_disconnect(uint16_t conn_handle, uint8_t hci_status_code)
{
  if (gServer == nullptr || conn_handle == BLE_CONN_HANDLE_INVALID) {
    return NRF_ERROR_INVALID_STATE;
  }
  return gServer->disconnect(conn_handle, hci_status_code) ? NRF_SUCCESS : NRF_ERROR_INVALID_STATE;
}

uint32_t sd_ble_gatts_hvx(uint16_t conn_handle, const ble_gatts_hvx_params_t* params)
{
  if (params == nullptr || conn_handle == BLE_CONN_HANDLE_INVALID) {
    return NRF_ERROR_INVALID_STATE;
  }
  BLECharacteristic* c = findByValueHandle(params->handle);
  if (c == nullptr) {
    return NRF_ERROR_INVALID_STATE;
  }
  uint16_t len = params->p_len ? *params->p_len : 0;
  if (!c->notifyEnabled()) {
    return BLE_ERROR_GATTS_SYS_ATTR_MISSING;
  }
  return c->notify(params->p_data, len) ? NRF_SUCCESS : NRF_ERROR_INVALID_STATE;
}

uint32_t sd_ble_gatts_sys_attr_set(uint16_t conn_handle, const uint8_t* sys_attr, uint16_t len, uint32_t flags)
{
  (void) flags;
  if (conn_handle == BLE_CONN_HANDLE_INVALID) {
    return NRF_ERROR_INVALID_STATE;
  }
  // The original firmware only ever calls this with nullptr (reset to defaults)
  // or with a seeded CCCD table built by pp_cccd_seed, which this port
  // replaces with ppCompatSeedNotifyEnabled(). Parse the table anyway so a
  // caller that hands one over still gets the expected effect.
  if (sys_attr != nullptr && len >= 6) {
    uint16_t pos = 0;
    while (pos + 6 <= len) {
      uint16_t handle = static_cast<uint16_t>(sys_attr[pos] | (sys_attr[pos + 1] << 8));
      uint16_t vlen = static_cast<uint16_t>(sys_attr[pos + 2] | (sys_attr[pos + 3] << 8));
      if (vlen != 2) break;
      uint16_t value = static_cast<uint16_t>(sys_attr[pos + 4] | (sys_attr[pos + 5] << 8));
      for (BLECharacteristic* c : gChars) {
        if (c->handles().cccd_handle == handle) {
          c->_compatSetSeeded((value & 0x0003) != 0);
        }
      }
      pos += 6;
    }
  }
  return NRF_SUCCESS;
}

uint32_t sd_ble_gatts_value_get(uint16_t conn_handle, uint16_t handle, ble_gatts_value_t* value)
{
  (void) conn_handle;
  if (value == nullptr || value->p_value == nullptr) {
    return NRF_ERROR_INVALID_STATE;
  }
  for (BLECharacteristic* c : gChars) {
    ble_gatts_char_handles_t h = c->handles();
    if (h.cccd_handle == handle && h.cccd_handle != BLE_GATT_HANDLE_INVALID) {
      uint16_t v = c->notifyEnabled() ? 0x0001 : 0x0000;
      if (value->len >= 2) {
        value->p_value[0] = static_cast<uint8_t>(v & 0xFF);
        value->p_value[1] = static_cast<uint8_t>(v >> 8);
        value->len = 2;
      }
      return NRF_SUCCESS;
    }
    if (h.value_handle == handle) {
      uint16_t n = c->read(value->p_value, value->len);
      value->len = n;
      return NRF_SUCCESS;
    }
  }
  return NRF_ERROR_INVALID_STATE;
}

void ppCompatSeedNotifyEnabled(BLECharacteristic* chr)
{
  if (chr) {
    chr->_compatSetSeeded(true);
  }
}

void ppCompatOwnAddressText(char* out, size_t len)
{
  if (!gInitDone) {
    snprintf(out, len, "--");
    return;
  }
  NimBLEAddress own = NimBLEDevice::getAddress();
  snprintf(out, len, "%s", own.toString().c_str());
}

void ppCompatFinalizeServices()
{
  if (gServicesFinalized) {
    return;
  }
  // NimBLE 2.x registers every service when the server starts.
  gServer->start();
  gServicesFinalized = true;
  Serial.print("GATT services started: ");
  Serial.print(gServices.size());
  Serial.print(" services, ");
  Serial.print(gChars.size());
  Serial.println(" characteristics");
}

// ---------------- nrf_soc.h ----------------

uint32_t sd_ecb_block_encrypt(nrf_ecb_hal_data_t* p)
{
  if (p == nullptr) {
    return NRF_ERROR_INVALID_STATE;
  }
  // Software AES: the S3's hardware AES (shared GDMA) faults when driven
  // from the NimBLE host task during the first-pair auth exchange.
#if PP_USE_SOFT_AES
  pp_aes128_encrypt_block(p->key, p->cleartext, p->ciphertext);
  return NRF_SUCCESS;
#else
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  int rc = mbedtls_aes_setkey_enc(&ctx, p->key, 128);
  if (rc == 0) {
    rc = mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, p->cleartext, p->ciphertext);
  }
  mbedtls_aes_free(&ctx);
  return rc == 0 ? NRF_SUCCESS : NRF_ERROR_INTERNAL;
#endif
}
