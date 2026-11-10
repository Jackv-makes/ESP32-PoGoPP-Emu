# Design notes

This is the technical side of the Stick S3 port: how kiyoukan's nRF52
firmware was moved onto the ESP32-S3, which of the original files I changed,
and where the Stick behaves differently from the nRF52 build. The
[README](README.md) covers building and using it. The Plus + protocol itself
is documented in
[kiyoukan's repo](https://github.com/kiyoukan/nRF52-go-plus-plus-backup-).

## How it is put together

Everything is in the `PlusPlusStick/` sketch:

- The protocol modules (`pp_*.cpp/.h`) are kiyoukan's: first pairing,
  reconnect and sleep handling.
- A compatibility layer stands in for the Adafruit nRF52 core, so those
  modules build for the ESP32 almost untouched (next section).
- `pp_catch_engine.*` reads the LED patterns the app sends and answers with
  button presses.
- `pp_board.*` is everything Stick S3 specific: buttons, status screen, menu,
  alerts, counters and deep sleep.
- `pp_device_material.template.h` is the empty template for your own device
  material.

`build.sh` compiles the sketch, and flashes it with the reset that works on
this board.

## The compatibility layer

| File | Stands in for |
|---|---|
| `bluefruit.h`, `pp_compat_ble.cpp` | Bluefruit services, characteristics, advertising and GAP, on top of NimBLE-Arduino 2.x |
| `nrf_soc.h` | the SoftDevice AES block call and system-off (deep sleep here) |
| `pp_aes128_soft.*` | AES-128 in software |
| `nrf_gpio.h` | `nrf_gpio_*` on Arduino GPIO |
| `Adafruit_LittleFS.h`, `InternalFileSystem.h`, `pp_compat_fs.cpp` | the two saved records, kept in NVS |
| `pp_cccd_seed.cpp` | the nRF `sys_attr` trick, as a "treat as subscribed" flag |

## The device address

The header holds the address in little-endian order, the way the nRF
SoftDevice wants it. `pp_compat_ble.cpp` reverses it and sets it as the
Bluetooth interface address with `esp_iface_mac_addr_set(..., ESP_MAC_BT)`.
Setting the interface address directly matters: going through the base MAC
did not work, because reading the Bluetooth address first makes the
controller keep the factory one.

Once NimBLE is up, the firmware reads back the address it is really using
and prints it, followed by `Clone address check: MATCH` or
`MISMATCH (app will not recognise this device)`.

## The NimBLE build flags

`build.sh` passes four NimBLE settings as compiler flags:

```
-DMYNEWT_VAL_BLE_SVC_GAP_PPCP_MIN_CONN_INTERVAL=6
-DMYNEWT_VAL_BLE_SVC_GAP_PPCP_MAX_CONN_INTERVAL=24
-DMYNEWT_VAL_BLE_SVC_GAP_PPCP_SLAVE_LATENCY=0
-DMYNEWT_VAL_BLE_SVC_GAP_PPCP_SUPERVISION_TMO=500
```

They add the Peripheral Preferred Connection Parameters characteristic to
the GAP service. That puts the Service Changed descriptor on the same handle
as on a real Plus +. A phone that still remembers the real device's table
subscribes there, and the firmware answers with a Service Changed indication
so the phone discovers the services again.

## Files changed from kiyoukan's sources

The protocol modules are kiyoukan's sources. The edits are from November
2026:

- `pp_app_lifecycle.*`: hooks for the board code, log text
- `pp_flow_refs.h`: LED writes go to the catch engine once the link is
  authenticated
- `pp_wiring.h`: forward declarations
- `pp_types.h`: the "paired" state byte in the advertisement is `0x02`. The
  original code had `0x01`, while its notes and a real paired Plus + say
  `02`. With `01` the app would not reconnect by itself.
- the sketch file: board and catch engine wiring

Everything else in the sketch (the compatibility layer, `pp_catch_engine.*`,
`pp_board.*`) is new in this port.

## Where it behaves differently from the nRF52 build

- The GATT Service Changed characteristic is exposed. The nRF52 build turns
  it off. The reason is in the build flags section above.
- AES-128 runs in software. The S3's hardware AES faulted when it was called
  from the NimBLE host task.
- Serial output never blocks. With a computer attached but not reading,
  blocking output delayed a write response long enough for Android to drop
  the link.
- When a link ends, NimBLE reports an unsubscribe for every subscribed
  characteristic. The log shows these as writes of 0 at the moment of the
  disconnect. They come from the stack and not from the phone.

## Saved data

Pairing and settings are kept in NVS through the Arduino `Preferences`
library. The two records the protocol code saves go through
`pp_compat_fs.cpp`. The menu settings are saved by `pp_board.cpp`, along
with the last link time and the reason for the last drop. Reset pairing in
the menu, or holding the side button for 5 seconds, wipes the pairing.

## Flash layout

The Stick S3 has 8 MB of flash. The build writes four parts:

| Offset | Part |
|---|---|
| `0x0` | bootloader |
| `0x8000` | partition table |
| `0xe000` | boot_app0 |
| `0x10000` | the app |

`build/PlusPlusStick.ino.merged.bin` is all of them in one full 8 MB image,
so writing it at `0x0` replaces everything on the Stick. `./build.sh flash`
only writes the app at `0x10000` and leaves the saved pairing and settings
alone.

## Serial log levels

`pp_log.h` sets how much goes to the serial port:

| Level | Value |
|---|---|
| `PP_LOG_LEVEL_DISABLED` | 0 |
| `PP_LOG_LEVEL_PROTOCOL` | 1 (default) |
| `PP_LOG_LEVEL_VERBOSE` | 2 |
| `PP_LOG_LEVEL_RAW_BLE` | 3 |

The higher levels print the pairing traffic, so treat those logs like the
device material and don't share them.
