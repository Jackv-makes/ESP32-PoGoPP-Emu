#pragma once
// ESP32 port: Bluefruit-compatible facade over NimBLE-Arduino 2.x.
//
// The Plus+ protocol modules were written against the Adafruit nRF52 Bluefruit
// API. This header reproduces the subset they use (services, characteristics,
// advertising, GAP preferences, a few SoftDevice calls) so those modules compile
// unchanged. Implementation lives in pp_compat_ble.cpp.

#include <Arduino.h>
#include <stdint.h>
#include <string.h>

#include "nrf_soc.h"

class NimBLEService;
class NimBLECharacteristic;

// ---- SoftDevice constants used by the protocol code ----
#define BLE_CONN_HANDLE_INVALID 0xFFFF
#define BLE_GATT_HANDLE_INVALID 0x0000
#define BLE_ERROR_GATTS_SYS_ATTR_MISSING 0x3401
#define BLE_GATTS_SYS_ATTR_FLAG_USR_SRVCS 0x02
#define BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION 0x13
#define BLE_GATT_HVX_NOTIFICATION 0x01
#define BLE_GATT_HVX_INDICATION 0x02
#define BLE_GAP_ADDR_TYPE_PUBLIC 0x00
#define BLE_GAP_ADDR_TYPE_RANDOM_STATIC 0x01
#define BLE_APPEARANCE_UNKNOWN 0
#define BLE_GATTC_WRITE_CMD_TX_QUEUE_SIZE_DEFAULT 1
#define BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED 0x00
#define BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE 0x06

enum CharacteristicProperties : uint8_t {
  CHR_PROPS_BROADCAST = 0x01,
  CHR_PROPS_READ = 0x02,
  CHR_PROPS_WRITE_WO_RESP = 0x04,
  CHR_PROPS_WRITE = 0x08,
  CHR_PROPS_NOTIFY = 0x10,
  CHR_PROPS_INDICATE = 0x20,
};

enum SecureMode_t : uint8_t {
  SECMODE_NO_ACCESS = 0x00,
  SECMODE_OPEN = 0x11,
  SECMODE_ENC_NO_MITM = 0x21,
  SECMODE_ENC_WITH_MITM = 0x31,
};

struct ble_gap_addr_t {
  uint8_t addr_id_peer;
  uint8_t addr_type;
  uint8_t addr[6];
};

struct ble_gap_conn_sec_mode_t {
  uint8_t sm;
  uint8_t lv;
};

static const ble_gap_conn_sec_mode_t BLE_SECMODE_NO_ACCESS = { 0, 0 };
static const ble_gap_conn_sec_mode_t BLE_SECMODE_OPEN = { 1, 1 };

struct ble_gatts_evt_read_t {
  uint16_t handle;
  uint16_t offset;
};

struct ble_gatts_char_handles_t {
  uint16_t value_handle;
  uint16_t user_desc_handle;
  uint16_t cccd_handle;
  uint16_t sccd_handle;
};

struct ble_gatts_hvx_params_t {
  uint16_t handle;
  uint8_t type;
  uint16_t offset;
  uint16_t* p_len;
  const uint8_t* p_data;
};

struct ble_gatts_value_t {
  uint16_t len;
  uint16_t offset;
  uint8_t* p_value;
};

// ---- UUID ----
class BLEUuid
{
public:
  BLEUuid() : _is16(false), _uuid16(0) { _str[0] = 0; }
  BLEUuid(const char* uuid128) : _is16(false), _uuid16(0)
  {
    strncpy(_str, uuid128, sizeof(_str) - 1);
    _str[sizeof(_str) - 1] = 0;
  }
  BLEUuid(uint16_t uuid16) : _is16(true), _uuid16(uuid16) { _str[0] = 0; }
  bool is16() const { return _is16; }
  uint16_t uuid16() const { return _uuid16; }
  const char* str() const { return _str; }

private:
  bool _is16;
  uint16_t _uuid16;
  char _str[40];
};

// ---- Service ----
class BLEService
{
public:
  explicit BLEService(BLEUuid bleuuid);
  explicit BLEService(uint16_t uuid16);
  virtual ~BLEService() {}

  void begin();
  NimBLEService* nim() const { return _svc; }

protected:
  uint16_t _handle;
  BLEUuid _uuid;
  NimBLEService* _svc;
};

// ---- Characteristic ----
class BLECharacteristic
{
public:
  typedef void (*write_cb_t)(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len);
  typedef void (*cccd_cb_t)(uint16_t conn_hdl, BLECharacteristic* chr, uint16_t value);
  typedef void (*read_authorize_cb_t)(uint16_t conn_hdl, BLECharacteristic* chr, ble_gatts_evt_read_t* request);

  explicit BLECharacteristic(BLEUuid bleuuid);
  explicit BLECharacteristic(uint16_t uuid16);
  virtual ~BLECharacteristic();

  void setProperties(uint8_t prop);
  void setPermission(SecureMode_t read_perm, SecureMode_t write_perm);
  void setMaxLen(uint16_t max_len);
  void setFixedLen(uint16_t fixed_len);
  void setWriteCallback(write_cb_t fp);
  void setCccdWriteCallback(cccd_cb_t fp);
  void setReadAuthorizeCallback(read_authorize_cb_t fp);

  void begin();

  uint16_t write(const void* data, uint16_t len);
  uint16_t write(uint8_t value) { return write(&value, 1); }
  uint16_t read(void* buffer, uint16_t len);
  bool notify(const void* data, uint16_t len);
  bool notifyEnabled() const;

  ble_gatts_char_handles_t handles() const;
  NimBLECharacteristic* nim() const { return _chr; }
  uint8_t properties() const { return _props; }
  const char* debugLabel() const { return _debugLabel; }

  // Port-only helpers used by the compat layer and the CCCD seeding shim.
  void _compatSetSubscribed(bool on) { _subscribed = on; }
  void _compatSetSeeded(bool on) { _seeded = on; }
  void _compatOnWrite(uint16_t conn_hdl, const uint8_t* data, uint16_t len);
  void _compatOnRead(uint16_t conn_hdl);
  void _compatOnSubscribe(uint16_t conn_hdl, uint16_t value);

protected:
  const char* _debugLabel;

private:
  BLEUuid _uuid;
  uint8_t _props;
  uint16_t _maxLen;
  bool _fixedLen;
  bool _subscribed;
  bool _seeded;
  uint8_t* _value;
  uint16_t _valueLen;
  uint16_t _valueCap;  // bytes allocated at _value; grows with setMaxLen/setFixedLen
  void _ensureValueCapacity(uint16_t needed);
  NimBLECharacteristic* _chr;
  write_cb_t _writeCb;
  cccd_cb_t _cccdCb;
  read_authorize_cb_t _readAuthCb;
};

// ---- Bluefruit singleton ----
class BluefruitCompat
{
public:
  typedef void (*connect_cb_t)(uint16_t conn_hdl);
  typedef void (*disconnect_cb_t)(uint16_t conn_hdl, uint8_t reason);

  struct AdvertisingCompat {
    void stop();
    void clearData();
    void setType(uint8_t type);
    void addFlags(uint8_t flags);
    void addManufacturerData(const void* data, uint16_t len);
    void addData(uint8_t type, const void* data, uint16_t len);
    void restartOnDisconnect(bool enable);
    void setInterval(uint16_t fast, uint16_t slow);
    void setFastTimeout(uint16_t sec);
    bool start(uint16_t timeout);
  } Advertising;

  struct ScanResponseCompat {
    void clearData();
    void addName();
  } ScanResponse;

  struct PeriphCompat {
    void setConnectCallback(connect_cb_t fp);
    void setDisconnectCallback(disconnect_cb_t fp);
    void setConnInterval(uint16_t min, uint16_t max);
    void setConnSupervisionTimeout(uint16_t timeout);
  } Periph;

  void configServiceChanged(bool changed);
  void configUuid128Count(uint8_t count);
  void configAttrTableSize(uint32_t size);
  void configPrphConn(uint16_t mtu_max, uint16_t event_len, uint8_t hvn_qsize, uint8_t wrcmd_qsize);

  bool begin(uint8_t prph_count = 1, uint8_t central_count = 0);
  void setName(const char* name);
  void setAddr(ble_gap_addr_t* addr);
  void setAppearance(uint16_t appear);
  bool connected() const;
  uint16_t connHandle() const;
};

extern BluefruitCompat Bluefruit;

// ---- Raw SoftDevice GATTS/GAP calls used by the protocol code ----
uint32_t sd_ble_gap_device_name_set(const ble_gap_conn_sec_mode_t* perm, const uint8_t* name, uint16_t len);
uint32_t sd_ble_gap_disconnect(uint16_t conn_handle, uint8_t hci_status_code);
uint32_t sd_ble_gatts_hvx(uint16_t conn_handle, const ble_gatts_hvx_params_t* params);
uint32_t sd_ble_gatts_sys_attr_set(uint16_t conn_handle, const uint8_t* sys_attr, uint16_t len, uint32_t flags);
uint32_t sd_ble_gatts_value_get(uint16_t conn_handle, uint16_t handle, ble_gatts_value_t* value);

// Port-only: make the compat layer treat notifications on this characteristic
// as enabled even if the peer never wrote its CCCD. Mirrors the nRF
// sys_attr seeding trick used by the original firmware.
void ppCompatSeedNotifyEnabled(BLECharacteristic* chr);
// Port-only: finish NimBLE service registration after topology setup.
void ppCompatFinalizeServices();
// Port-only: the address the controller is actually using, "--" before init.
void ppCompatOwnAddressText(char* out, size_t len);
