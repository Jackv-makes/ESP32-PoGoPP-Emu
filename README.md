# Pokémon GO Plus+ backup on an M5Stack Stick S3

This is firmware that lets an M5Stack Stick S3 stand in for a Pokémon GO Plus+
that you own. I wanted a backup of my own Plus+ that I can press to catch
with, and that can also catch and spin by itself. The Stick S3 was the board I
had on hand to try it on.

It is a port of kiyoukan's
[nRF52 Plus+ backup firmware](https://github.com/kiyoukan/nRF52-go-plus-plus-backup-)
to the ESP32-S3. kiyoukan did the hard part, which is the pairing and
reconnect protocol. I moved it onto NimBLE, added the catch and spin handling
from [pgpemu](https://github.com/shortcuts/pgpemu), and gave it a status
screen and a menu.

There is no device material in this repo. The firmware only works with the
Bluetooth address and the keys of a real Plus+, and you have to read those
from a device you own and put them in yourself. It won't compile without them.

## What it does

- Pairs with Pokémon GO as a Plus+ and reconnects like one
- Auto catch (everything, or only new Pokémon) and auto spin, each with its
  own switch
- A manual catch or spin press on the front button
- A status screen with the link state, the last thing the app reported and
  counters for caught, fled and spins
- A menu for press speed, alerts, brightness, hiding the device and wiping
  the pairing
- Deep sleep after 15 minutes without a connection

## Disclaimer

This is a hobby project and there are probably rough edges. It is meant for
backing up a Plus+ you own, nothing else. I'm not affiliated with Nintendo,
Niantic or The Pokémon Company. A device that plays for you may be against
the game's terms of service, and if your account gets in trouble over it,
that is on you. Getting the keys means opening your Plus+ and reading a
1.8 V flash chip, and if that kills it, that is on you too. I've only tested
this on my own Stick S3, with the material from my own Plus+, on one iPhone
and one Android phone.

The port and the documentation here were written with help from AI. If
something in the text doesn't match the code, trust the code and let me know.

## What you need

- An M5Stack Stick S3. It is the only board I've run this on. Everything
  board specific is in `pp_board.*`, so another ESP32 board would need its own
  version of that, and I haven't tried one.
- Your own Plus+, and a way to read its 1.8 V flash chip (next section).
- A computer with `arduino-cli` and `esptool`. I use a Mac.

## Getting your device material

### Reading the flash chip

The keys are in the Plus+'s external flash, a 1.8 V Macronix MX25U6433F in a
WSON-8 package. I desoldered mine and read it with a CH341A programmer, a
1.8 V adapter and a QFN8 to DIP8 socket (5 x 6 mm), using flashrom, which
knows the chip as `MX25U6435E/F`. I read it twice and compared the two files
before trusting it. Afterwards I soldered the chip back and my Plus+ works
again.

kiyoukan's README describes reading the chip in place with a spring needle
probe instead. I haven't tried that.

Whichever way you do it, this is a 1.8 V part. Don't read it at 3.3 V or 5 V.
And keep the image on your own machine, it holds the keys of your device.

### Making the header

1. Get `tools/flash_image_reader` from kiyoukan's repo, put your image in its
   `input` folder and run `python read_flash_data.py`.
2. Open the `pp_device_material.generated.h` it writes.
3. Fill in `PP_DEVICE_PUBLIC_ADDR_LE_BYTES` with the public Bluetooth address
   of your Plus+, in little-endian order. That is backwards from how a
   scanner shows it: `AA:BB:CC:DD:EE:FF` becomes
   `{ 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA }`.
4. Set `PP_DEVICE_MATERIAL_CONFIGURED` to `1`.
5. Save it as `PlusPlusStick/pp_device_material.h`.

### Getting the right address

The address is not in the flash image. It lives in the Plus+'s
microcontroller, so you read it over the air with a Bluetooth scanner app
while the Plus+ is advertising.

Make sure it is really yours. Every Plus+ advertises the same name, and my
first try used the address of a different Plus+ that my phone had seen
before. With the wrong address pairing fails partway, and it looks just like
a firmware bug. A simple check is that the entry only shows up in the scanner
while yours is awake.

## Building

You need `arduino-cli` with:

- the M5Stack ESP32 core (I built with 3.3.9), board index
  `https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json`
- M5Unified (0.2.21) and M5GFX (0.2.29)
- NimBLE-Arduino 2.5.2

```bash
arduino-cli lib install "NimBLE-Arduino@2.5.2"
```

```bash
./build.sh
```

`build.sh` compiles for `m5stack:esp32:m5stack_sticks3:PSRAM=opi` into
`build/`. It also passes a few NimBLE settings as compiler flags, and the
firmware needs them, so if you build some other way, copy them from the
script. [DESIGN.md](DESIGN.md) explains what they are for.

## Flashing

Flashing replaces whatever is on your Stick, so back that up first if you
want it back.

You need `esptool` 5 or newer on your `PATH`. The M5Stack core comes with a
copy.

The first time, write the complete image (bootloader, partition table and
app). This overwrites the whole 8 MB flash:

```bash
esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --after watchdog-reset write-flash 0x0 build/PlusPlusStick.ino.merged.bin
```

After that, the script only rewrites the app:

```bash
PORT=/dev/cu.usbmodemXXXX ./build.sh flash
```

Keep the `--after watchdog-reset` part. On my Mac, every reset through
DTR/RTS (esptool's default, `arduino-cli upload`, a serial terminal that
toggles DTR) left the Stick in download mode: black screen, green LED
blinking. If yours ends up like that, any esptool command with
`--after watchdog-reset` brings it back. Holding the side power button for
about 4 seconds puts mine into download mode by hand.

The serial log is on the same port at 115200. Open it with something that
leaves DTR and RTS alone. Near the top it prints the address the radio took,
and `Clone address check: MATCH` if that is the one from your header.

## Using it

**Never have your real Plus+ and the Stick switched on at the same time.**
They share one address, and that confuses the phone and anything else that
is scanning. Put the real one away, asleep, while you use the Stick.

Pair it in Pokémon GO under Settings > Connected Devices and Services >
Accessory Devices, the same way as a real Plus+.

- If your phone has talked to the real Plus+ before, it may still remember
  that device's layout. My iPhone held on to it until I restarted the phone.
  If pairing stalls, restart the phone and try again.
- "Enable Quick-Reconnect Mode" on that settings page makes the app try once
  to reconnect right after a drop. With it off, the app waits for a press on
  the front button. Tapping the device icon on the map reconnects any time.
- The Info page shows your device address, so keep it out of photos and
  screenshots you share.

### Buttons

| Input | What it does |
|---|---|
| Front button, click | manual catch or spin press (when connected) |
| Side button, click | opens the menu |
| Side button, hold 5 s | wipes the pairing |
| Either button | wakes it from deep sleep |

In the menu, the side button goes to the next item, the front button selects,
and holding either one goes back. The menu closes by itself after 10 seconds.

### Menu

| Item | What it does |
|---|---|
| Auto catch | Off / All / New only |
| Auto spin | On / Off |
| Press speed | Normal presses 1.0 to 2.5 s after the app asks, Fast in 0.3 to 0.8 s |
| Alerts | beeps on events, and a warning flash for no balls, bag full and box full |
| Advertise | Off hides the Stick, and it stays hidden after a disconnect |
| Reset pairing | asks first, then wipes the saved pairing |
| Sleep now | deep sleep right away |
| Brightness | Low / Mid / High |
| Info | addresses, pairing stage, counters, link time, why the last drop happened, build tag |

All settings are saved.

## What works and what I haven't checked

Working, on my Stick S3 with my own Plus+'s material:

- First pairing, auto catch and auto spin in Pokémon GO on an iPhone and on
  an Android phone
- Reconnecting after a drop, on Android
- Telling apart Pokémon nearby, PokéStop, caught, fled and items

Not checked yet:

- The warnings for no balls, bag full and box full, and the New only mode
- Waking from deep sleep with the buttons
- Any board other than the Stick S3
- The first-time flash command above. My Stick got its first image through
  `arduino-cli upload` in download mode, which writes the same four parts.

## Keeping your keys private

If you fork this or share a build, leave these out:

- `pp_device_material.h` and `pp_device_material.generated.h`
- `device_material_info.txt`
- flash images
- `build/` and any built image (`.bin`, `.elf`, `.map`), because they contain
  your device material
- serial logs and Bluetooth captures, because they show your address, and at
  the higher log levels the pairing traffic too

The `.gitignore` here covers the files. Logs and captures are up to you.

## What I changed

The protocol code is kiyoukan's and I touched as little of it as I could.
The changes are from November 2026:

- Hooks for the board code and the catch engine, so the LED patterns the app
  sends go to the catch engine once the link is authenticated
- The "paired" byte in the advertisement is `0x02` instead of `0x01`.
  kiyoukan's notes and my real Plus+ both show `02` for a paired device,
  and with `01` the app would not reconnect by itself.
- A small layer that stands in for the Adafruit nRF52 core, so the rest
  builds on the ESP32 unchanged

[DESIGN.md](DESIGN.md) has the file by file list and the places where the
Stick behaves differently from the nRF52 build. The protocol itself is
documented in kiyoukan's repo.

## License

GNU AGPL v3.0, the same as kiyoukan's firmware that this is a port of. See
[LICENSE](LICENSE).

## Credits

- [kiyoukan](https://github.com/kiyoukan/nRF52-go-plus-plus-backup-) for
  working out the Plus+ pairing and reconnect protocol and publishing the
  nRF52 firmware this is ported from.
- [pgpemu](https://github.com/shortcuts/pgpemu) for pgpemu, and yohanes,
  spezifisch and paper183 for the pgpemu work it builds on. The LED pattern
  reading and the button press timing come from there.
- u/EeveesGalore for the notes on the GO Plus button format.
- M5Stack for M5Unified and M5GFX, and h2zero for NimBLE-Arduino.
