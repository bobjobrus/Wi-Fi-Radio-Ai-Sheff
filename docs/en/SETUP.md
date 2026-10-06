[Русский](../SETUP.md) · **English**

# Flashing and setup

From a bare board to a radio on the channel — and how to use it every day.

**Before you start**

- **The bridge is already running**, and you know its address and network key — see [Your own bridge](BRIDGE.md). The bridge
  is a small program on a computer or server through which the radios hear each other. Without a bridge the radio will
  power on, but it won't get on the channel.
- **Board** — ESP32-S3-DevKitC-1 or its clone YD-ESP32-S3, with 4 MB of flash or more (the firmware doesn't need PSRAM).
  Wiring — as in the [build guide](BUILD.md).
- **A USB-C cable that carries data.** A "charge-only" cable won't show the board to the computer.

You can flash and set up the board before you assemble the case: the microphone and speaker aren't needed to get on the
network.

**Contents**

1. [Flashing](#1-flashing)
2. [First-time setup](#2-first-time-setup)
3. [Everyday use](#3-everyday-use)
4. [Updates](#4-updates)
5. [USB commands](#5-usb-commands)

---

## 1. Flashing

The firmware is the program inside the board. You write it over the cable once; after that the radio updates itself
through the bridge ([section 4](#4-updates)).

### Which file to use

| File | What's inside | When you need it |
|---|---|---|
| [`firmware/dist/walkie-full-v9.bin`](../../firmware/dist/walkie-full-v9.bin) | the whole flash memory, 4 MB: bootloader, partition table and the program itself, version 9 | first flashing over the cable and "rescuing" a board that won't start. Written from address `0x0`. **Erases all radio settings** |
| [`firmware/dist/walkie.bin`](../../firmware/dist/walkie.bin) + [`walkie.json`](../../firmware/dist/walkie.json) | the program only (≈1.2 MB) and its version number | for the bridge, so the radios update themselves, and for uploading through the settings page |

### USB connectors and the serial port

The board has two USB-C connectors:

- **"USB"** — the ESP32-S3's own built-in USB. It's the most convenient one for both flashing and setup.
- **"COM"** (labelled "UART" on the original board) — goes through a separate USB-to-serial adapter chip. On some clones
  this connector **gets no power from a C-to-C cable**: the board is missing the resistors that tell the computer a
  device has been plugged in. If the board doesn't light up, switch to the "USB" connector or use a USB-A → USB-C cable.

The port is the name under which the computer sees the connected board:

| System | What the port looks like |
|---|---|
| macOS | `/dev/cu.usbmodem…`, `/dev/cu.wchusbserial…` or `/dev/cu.usbserial…` |
| Linux | `/dev/ttyACM0` or `/dev/ttyUSB0`. Add yourself to the `dialout` group once: `sudo usermod -aG dialout $USER`, then log out and back in |
| Windows | `COM3`, `COM5`… — "Device Manager → Ports (COM & LPT)". If the "COM" connector gives no port, install the driver for its adapter chip — CH343 from the WCH website (YD-ESP32-S3 clone) or CP210x from the Silicon Labs website (original DevKitC-1) — or connect to the "USB" connector |

Not sure which port is yours? Look at the list before and after plugging in the cable: the new one is the board.

In an assembled radio the board's connectors are inside the case — remove the back cover (4 screws). Before plugging
the cable into the board, turn off the power switch (the toggle on top): the radio will run on power from the
computer, and the battery won't drain for nothing.

### Option A. Prebuilt file with esptool

esptool is Espressif's official flashing tool; it runs on macOS, Linux and Windows. You need Python 3.

```sh
python3 -m pip install --upgrade esptool
```

macOS and Linux (substitute your own port):

```sh
esptool --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 921600 --after watchdog-reset \
  write-flash 0x0 firmware/dist/walkie-full-v9.bin
```

Windows (PowerShell):

```powershell
python -m esptool --chip esp32s3 --port COM5 --baud 921600 --after watchdog-reset write-flash 0x0 firmware\dist\walkie-full-v9.bin
```

- `0x0` — write from the very beginning of the memory: the file contains everything at once, including the bootloader.
- `--after watchdog-reset` — after writing, restart the board in a way that makes sure it leaves download mode.
  A normal reset through the "USB" connector sometimes leaves it in the bootloader, and the radio stays silent.
- This is the esptool 5 syntax. If the command doesn't understand `write-flash` or `watchdog-reset`, you have an old
  version — update it with the command above.

**macOS, the short way:** [`tools/flash.sh`](../../tools/flash.sh) finds the port and the newest
`walkie-full-v*.bin` by itself and runs the same command (`tools/flash.sh /dev/cu.xxx` — specify the port manually).
The script takes esptool from the Arduino core, so
[`tools/build_firmware.sh`](../../tools/build_firmware.sh) must have run once (option B below).

### Option A2. In the browser, nothing to install

You need Chrome or Edge on a computer: only they can work with USB ports from a web page (Safari and Firefox can't).

1. Open Espressif's official web flasher: <https://espressif.github.io/esptool-js/>.
2. Baudrate `921600` → **Connect** → choose the board's port.
3. Flash Address `0x0`, file `walkie-full-v9.bin` → **Program**.
4. When writing finishes, press **RST** on the board (or unplug and replug the cable).

### Option B. Build the firmware yourself

You need this if you changed the code or want to build your bridge's address into the firmware.

**What you can change before building** — in [`firmware/walkie/config.h`](../../firmware/walkie/config.h):

- `FW_VERSION` — the version number, an integer. Give every new build for the bridge a **higher** number,
  otherwise the radios won't take it.
- `DEFAULT_SERVER` — your bridge's address, e.g. `"radio.example.com"`. Then freshly flashed radios know the bridge
  without any setup. An address entered later on the page or over USB takes priority over the built-in one; after the
  `factory` command the radio goes back to the built-in address. The network key is never built into the
  firmware — it's entered separately.
- Pin numbers — if you wired the board differently.

**macOS and Linux:**

1. Install arduino-cli: on macOS `brew install arduino-cli`, on Linux — following the
   [official instructions](https://arduino.github.io/arduino-cli/latest/installation/).
2. Run the build:

   ```sh
   tools/build_firmware.sh
   ```

   The first time, the script installs the esp32 core version 3.3.12 by itself (a few hundred megabytes). The firmware
   needs no third-party libraries. The result goes to `firmware/dist/`: `walkie-full-vN.bin`, `walkie.bin` and `walkie.json`.

The board settings the script builds with: "ESP32S3 Dev Module", Flash Size 4M, partition scheme
`min_spiffs` (two halves of 1.9 MB each — needed for over-the-air updates), PSRAM disabled, USB Mode `hwcdc`,
USB CDC On Boot enabled.

> The script replaces the ctags helper program with a dummy: on an Apple Silicon Mac without Rosetta, the one that
> ships with Arduino won't run. That's why in `walkie.ino` every function must be placed **before** the point where it's
> called — otherwise the build fails.

**Windows** (PowerShell, from the project folder):

```powershell
arduino-cli core install esp32:esp32@3.3.12 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs,PSRAM=disabled,USBMode=hwcdc,CDCOnBoot=cdc" --output-dir firmware\build firmware\walkie
```

`firmware\build` will then contain `walkie.ino.merged.bin` (this is the full image, like `walkie-full`) and `walkie.ino.bin`
(this is `walkie.bin`). Create `walkie.json` for the bridge yourself: `{"version": 9}` — the number from `FW_VERSION`.

**Arduino IDE 2** works too. Core "esp32 by Espressif" 3.3.12, in the "Tools" menu:
board "ESP32S3 Dev Module", Flash Size "4MB (32Mb)", Partition Scheme "Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)",
PSRAM "Disabled", USB Mode "Hardware CDC and JTAG", USB CDC On Boot "Enabled".
Get the files through "Sketch → Export Compiled Binary".

### If flashing fails

- **The port doesn't appear** — the cable doesn't carry data, or the "COM" connector gets no power (see above). Try
  another cable and the "USB" connector.
- **`Failed to connect` / `No serial data received`** — put the board into download mode by hand: hold **BOOT**,
  press and release **RST**, release **BOOT**. Run the command again.
- **Errors in the middle of writing** — lower the speed: `--baud 115200`.
- **Written successfully, but the ring doesn't light up** — press **RST** or replug the cable.

---

## 2. First-time setup

The radio needs to know three things — Wi-Fi, the bridge address and the network key — and, for convenience, its own name.

| What | Where to get it |
|---|---|
| **Bridge address** | IP address or domain name of the server where the bridge runs: `203.0.113.10` or `radio.example.com`. The default port is 47000; for a different one, add it after a colon: `radio.example.com:47010`. If the bridge is on the same network as the radio, the field can be left empty ([below](#empty-bridge-address--searching-your-own-network)) |
| **Network key** | the shared password of all radios and bridges — the `key` line in the bridge settings. `python3 hub/walkie_hub.py --genkey` generates a new key. It must be at least 12 **Latin** letters and digits (the radio skips Cyrillic letters but the bridge counts them, so the keys won't match); hyphens, spaces and case don't matter: `abcd-efgh…` and `ABCDEFGH…` are the same key |
| **Wi-Fi** | network name and password. **2.4 GHz** only: the ESP32-S3 doesn't see 5 GHz networks. Networks with a sign-in page (as in cafés and hotels) and corporate networks with a login won't work |
| **Name** | how the radio appears on the bridge page: "Kitchen", "Garage". Without a name — «Рация XXXX» ("Radio XXXX"). Up to 31 bytes fit: about 15 Cyrillic letters or 31 Latin ones |

The key is stored in the radio's memory, not in the firmware. Every packet is signed with this key and speech is
encrypted: a radio with a different key won't get on the channel.

### Option 1. Over a USB cable (macOS, Linux)

[`tools/provision.py`](../../tools/provision.py) — needs only standard Python 3, nothing to install.

```sh
python3 tools/provision.py --name Kitchen --server 203.0.113.10 --key XXXX-XXXX-XXXX-XXXX
python3 tools/provision.py --wifi "MyNetwork|password" --wifi "SecondNetwork|password2"
python3 tools/provision.py --show
```

| Flag | What it does |
|---|---|
| `--name` | radio name |
| `--server` | bridge address `host[:port]`; `auto` — erase the address and search for the bridge on your own network |
| `--key` | network key |
| `--wifi "Network\|password"` | add a network; can be repeated, the radio remembers up to three networks |
| `--volume 0..20` | volume |
| `--amp i2s` / `--amp pdm` | amplifier type: `i2s` — MAX98357A (default), `pdm` — PAM8403 through a filter. The radio restarts, so the script does this last |
| `--show` | show the radio's status (same as running without flags) |
| `--reboot` | restart the radio at the end |
| `--port` | specify the port manually |

The script finds the port by itself only on macOS. On Linux, specify it: `--port /dev/ttyACM0`. For each step the script
prints ✓ or ✗; the key and passwords are replaced with `***` in the output.

The script doesn't work on Windows (it needs the termios module, which exists only on macOS and Linux) — type the same
commands in a serial terminal window, see [section 5](#5-usb-commands).

### Option 2. From a phone

A radio that doesn't know any Wi-Fi network opens its own **access point** — a Wi-Fi network `RADIO-XXXX`
with no password (XXXX — four characters of the radio's ID). Meanwhile the ring slowly "breathes" purple.

1. Turn on the radio and connect your phone to the `RADIO-XXXX` network.
2. The settings page opens by itself, like hotel Wi-Fi. If it doesn't, type `http://192.168.4.1` in the browser.
3. The **«Настройки»** (Settings) block: name («Имя рации»), bridge address («Адрес моста»), network key
   («Ключ сети») → **«Сохранить»** (Save).
   Do this before Wi-Fi: after Wi-Fi the radio restarts, and the phone disconnects from it.
4. The **«Wi-Fi»** block: **«Найти»** (Find) → tap your network → password → **«Подключиться к этой сети»**
   (Connect to this network). The radio remembers the network, restarts and connects.
5. A few seconds later you'll hear a rising "tee-doo" — the radio is connected to the bridge.

If you entered Wi-Fi first and left the key empty, after the restart the radio opens `RADIO-XXXX` again
(there is Wi-Fi, but no key): connect once more and finish step 3. You can also do it through your home network — see below.

The access point closes by itself once the radio has been connected to Wi-Fi for more than a minute, is connected to
the bridge, and nobody is connected to the access point.

### The settings page on your home network

The same page opens from any device on the same Wi-Fi network at `http://radio-xxxx.local`
(the same four characters, in lowercase). It usually opens from computers and iPhones; Android often doesn't understand
such addresses — then find `radio-xxxx` in your router's device list and type its numeric address
(or look it up with the `show` command over USB).

The **«Состояние»** (Status) block refreshes every 2 seconds: link status («на связи» (connected), «ищу мост»
(looking for the bridge), «не тот ключ сети» (wrong network key)…), battery, network and signal strength, the bridge
and the latency to it, how many radios are online, received and lost packets, «Чужие пакеты» (foreign packets — with
an invalid signature), «Работает» (running) — for how long and why the radio last started («включение питания»
(power-on), «сторож: зависание» (watchdog: hang), «просадка питания» (brownout)…), «Свист рядом с другой рацией»
(howl near another radio) and the last error.

| Field | What it does | Default |
|---|---|---|
| «Имя рации» (Radio name) | how it appears on the bridge page | «Рация XXXX» (Radio XXXX) |
| «Адрес моста» (Bridge address) | `host[:port]`; empty — search for the bridge on your own network | empty |
| «Ключ сети» (Network key) | the field is always shown empty; next to it it says «задан» (set) or «не задан» (not set). Leave it empty to keep the current key | — |
| «Громкость» (Volume) | 0–20, ≈ 2 dB per step | 14 |
| «Яркость подсветки» (LED brightness) | the ring, 0–100 | 60 |
| «Автоусиление микрофона» (Microphone automatic gain control) | the radio adjusts the sensitivity to your voice by itself (up to +36 dB) | on |
| «Усиление, если автоусиление выключено» (Gain if AGC is off) | 0–42 dB | 24 |
| «Сигнал в конце чужой передачи» (Tone at the end of someone else's transmission) | a short falling "tee-doo" when the other person releases the button | on |
| «Микрофон на правом канале» (Microphone on the right channel) | if the L/R pin of the INMP441 microphone is connected to 3.3 V rather than to GND. Check with the `micraw` command | off |
| «Регулятор крутится наоборот» (Volume knob turns the other way) | if turning clockwise lowers the volume | off |
| «Экономия аккумулятора» (Battery saving) | after 15 s of silence Wi-Fi dozes off; the first words of an incoming transmission arrive with a delay of up to ≈0.3 s (the buffer smooths it out), and the battery lasts longer | **off** |

> **Turn battery saving on with care.** At idle the radio then draws so little current that a charger module built
> like a power bank may decide the load has been disconnected and cut the power: the radio "turns itself off".
> Test your module: turn battery saving on and leave the radio running on its battery, untouched, for half an hour.

There's no amplifier type switch on the page — only over USB (`amp`, see [section 5](#5-usb-commands)).

> **The page has no password.** Anyone on the same Wi-Fi network (or connected to the open `RADIO-XXXX`) can
> change the settings and upload firmware. The page never shows the network key itself. Keep the radios on your own
> network, not a public one.

### Wi-Fi: up to three networks

- The radio remembers **up to three networks**, for example home, the country house and the workshop, and when connecting
  it picks whichever known network has the strongest signal. A fourth one pushes the first one out of the list. You can
  remove an extra one with the «убрать» (remove) button on the page.
- If the network disappears, the radio tries to connect every 15 seconds. After a minute without a network it also opens
  `RADIO-XXXX` so you can give it a new network. If the network has just dropped out for a while, don't touch
  anything: when the network comes back, the radio reconnects by itself.
- After 15 minutes without Wi-Fi, with nobody connected to the access point, the radio restarts by itself.

### Empty bridge address — searching your own network

If the address is empty (or `server auto` was set over USB), the radio broadcasts a greeting to all devices on its
network on UDP port 47000 and takes the first bridge that replies with the same key. This works only when the bridge
is on the same network, behind the same router. It won't work if the radio is somewhere else, on a guest network with
client isolation, or if the router doesn't pass broadcast packets — then enter the address.

If an address is set, the radio talks only to it. It looks up a domain name again every 10 minutes (and every 15 seconds
while there's no connection): if the server's IP changes, it's enough to fix one record at your domain registrar — the
radios will find the bridge by themselves.

### Reopening setup with the button

Hold down the PTT (push-to-talk) button, turn on the power switch and keep holding the button for about 1.5 seconds, until the ring
fills with purple. The radio opens `RADIO-XXXX` and at the same time connects to a known network. A low beep after
that is normal: the button was held while there was no connection yet. In this mode the access point stays up until
the radio is switched off — when you're done, turn the radio off and on again.

---

## 3. Everyday use

### First test with a single radio

You don't need a second radio for testing: you can run a "parrot" on a computer that sends everything you say back
to the channel.

```sh
python3 hub/sim_radio.py --hub 203.0.113.10 --key XXXX-XXXX-XXXX-XXXX echo
```

Press the button, say something, release it — a moment later the radio repeats your words. The "parrot" runs
for 30 minutes (`--minutes` changes this). Other tests with the same program — `status`, `tone`, `say`, `listen`:
`python3 hub/sim_radio.py --help`. The computer needs UDP access to the bridge, just like the radios.

### Talking

1. Press and hold the PTT button. Two white LEDs run around the ring — the radio is asking the bridge
   for the channel.
2. You'll hear a rising "beep-beep" — you can talk. Anything said before the tone isn't transmitted.
3. Speak in your normal voice. The ring is red and "dances" in time with your voice — you're being heard.
4. Release the button. The other radios hear you right away, while you're talking, not afterwards.

One person talks at a time. If the channel is busy, you'll hear "boo-boo-boo" (the busy tone) — wait for the other
transmission to end and press again. A single press lasts no longer than 90 seconds (protection against a stuck button;
the limit is set by the `tot_seconds` line in the bridge settings, but the radio won't transmit for more than 100 seconds
anyway): after that comes a low beep — release the button and press it again.

### Volume knob

| Action | What happens |
|---|---|
| Turn | volume, one step per click (0–20). For 1.5 s the ring shows the level, and you hear a click. Turning also unmutes the sound |
| Short press (under 1 s) | mute or unmute. While muted, a single purple LED is lit at idle; if someone talks meanwhile, the ring glows a soft light blue |
| Hold 1–4 s and release | for 2.5 s — the battery gauge: green — more than 40 %, amber — 16–40 %, red — 15 % or less. Dim white — the charge is still being measured, blue — USB power with no battery |
| Hold 10 s | settings reset ([below](#resetting-settings)). From the 5th second the ring flashes red: if you change your mind, release it and nothing happens |

If it turns "the wrong way", enable «Регулятор крутится наоборот» (Volume knob turns the other way) on the settings page.

### Sounds

| Sound | Meaning |
|---|---|
| rising "beep-beep" | you can talk |
| "boo-boo-boo" | the channel is busy |
| a single low beep | no connection, the bridge didn't respond to the press, or the button was held longer than 90 s |
| two falling tones | the connection to the bridge was lost |
| rising "tee-doo" | the radio is connected: after power-on or when the connection comes back |
| short falling "tee-doo" | someone else's transmission has ended (turned off with the «Сигнал в конце чужой передачи» setting) |
| click | the volume changed or the sound was unmuted |
| three descending notes | the battery is running low |

### LED ring

At idle — a short green flash every 3 seconds: all is well. What the other colours mean — see the
[Ring colours](TROUBLESHOOTING.md#ring-colours) table.

### Battery and charging

- **Charging** — with a cable into the USB-C socket at the bottom of the case (the charger module). The radio charges
  even with the power switch off: the switch only cuts power to the radio itself. While it's charging you can usually
  use it — this depends on the charger module. The percentage is overstated while charging: the voltage is higher under
  charge — the accurate value appears about a minute after you unplug the cable.
- **How much is left** — hold the knob for 1–4 s; on the settings page — the «Аккумулятор» (Battery) row (percent and
  volts); the bridge page shows the charge of all radios at once.
- For the first ≈20 seconds after power-on the charge shows «измеряется» (measuring): the voltage wanders while Wi-Fi is
  connecting. Readings taken during a conversation are ignored too — the voltage sags under load.
- **Running low:** if the charge stays at 15 % or less for 30 seconds, the radio plays three descending notes every
  5 minutes (every minute from 5 %; silent during a conversation), and at idle it flashes red instead of green.
  The bridge log gets «пора на зарядку» ("time to charge").

### Resetting settings

| How | What is erased | What stays |
|---|---|---|
| Hold the knob for 10 s | Wi-Fi networks, volume, brightness, microphone and tone settings, knob direction, battery saving | name, bridge address, network key, amplifier type |
| The `factory` command over USB | everything | nothing: the radio is like new |
| Flashing `walkie-full-…bin` over the cable | everything | nothing |

After a reset the radio restarts and opens `RADIO-XXXX` — set up Wi-Fi again ([section 2](#2-first-time-setup)).
The knob reset deliberately leaves the key and the bridge address alone: after it, anyone who knows the Wi-Fi
password can bring the radio back onto the channel.

---

## 4. Updates

### Automatically from the bridge

The bridge tells every radio the version number of the firmware sitting in its folder. If it's higher than the radio's
own number, the radio waits until the channel has been quiet for 20 seconds (nobody is talking or has just been
talking), and then:

1. downloads the file from the bridge over HTTP (port 47080, unless the bridge is set to a different one);
2. checks the size and the SHA-256 checksum — the radio received it in a packet signed with the network key, so without
   the key nobody can slip it a foreign file;
3. writes the firmware into the other half of the memory (not the one it's running from now) and restarts with it.

While the update is in progress, a blue bar grows around the ring — don't turn the radio off. Settings are kept.
If the download breaks off or the file doesn't match, the radio stays on the old firmware (it sits untouched in the
other half of the memory) and tries again in an hour or after a restart.

How to put firmware on the bridge — see [Your own bridge](BRIDGE.md). In short: build the firmware with a higher
`FW_VERSION` ([option B](#option-b-build-the-firmware-yourself)) and put into the bridge's firmware folder first
`walkie.json`, then `walkie.bin` — the bridge notices them within 10 seconds. The order matters: the bridge rereads the
folder only when `walkie.bin` changes ([details](BRIDGE.md#9-over-the-air-firmware-updates)). The build script itself
writes the same number into `walkie.json` as in `config.h`.

The radio's firmware version is shown on the settings page (the «Прошивка» (Firmware) block) and by the `show` command.

### Manually through the settings page

The **«Прошивка»** (Firmware) block → choose the `walkie.bin` file → **«Загрузить прошивку»** (Upload firmware).
The radio replies «Прошивка записана, перезапуск…» ("Firmware written, restarting…") and restarts; settings are kept.
The version number isn't checked here — any build of this firmware will be loaded.

Upload `walkie.bin` specifically, not `walkie-full-…bin`: the full image (4 MB) doesn't fit into half of the memory
(1.9 MB), and the radio replies «Ошибка записи прошивки» ("Firmware write error").

### Over the cable

If the radio won't start at all, write the full image again ([section 1](#1-flashing)). This erases the
settings.

---

## 5. USB commands

The radio accepts text commands over the cable through either of its two connectors. Speed — 115200 (for the "COM"
connector; for the "USB" connector the speed doesn't matter). One line — one command, ending with Enter. The radio also
writes its log there (in Russian): what's going on with Wi-Fi, the bridge, updates. If something is wrong, look here
first.

**How to connect:**

- Any system where esptool is installed (pyserial comes with it):

  ```sh
  python3 -m serial.tools.miniterm --echo /dev/cu.usbmodem1101 115200
  ```

  On Windows: `python -m serial.tools.miniterm --echo COM5 115200`. Exit with `Ctrl+]`.
- Arduino IDE: "Serial Monitor", speed 115200, line ending "New Line" (with "No Line Ending" commands
  aren't executed).
- arduino-cli: `arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200`.
- Just to view the status on macOS and Linux: `python3 tools/provision.py --show` (on Linux — with `--port /dev/ttyACM0`).

| Command | What it does |
|---|---|
| `help` | list of commands |
| `show` | everything about the radio: ID, name, firmware version, bridge, whether the key is set, Wi-Fi, latency, charge, volume, receive statistics |
| `name Kitchen` | radio name |
| `server 203.0.113.10`<br>`server radio.example.com:47010` | bridge address |
| `server auto` | erase the address: search for the bridge on your own network |
| `key XXXX-XXXX-XXXX-XXXX` | network key. Fewer than 12 Latin letters and digits — the reply is `ERR key` |
| `wifi MyNetwork\|password` | add a network (for a known one — change its password) and connect right away. For a network without a password — `wifi MyNetwork` |
| `wifi-clear` | forget all networks |
| `volume 14` | volume 0–20 |
| `agc on` / `agc off` | microphone automatic gain control (AGC) |
| `led 60` | ring brightness 0–100 |
| `amp i2s` / `amp pdm` | amplifier type: MAX98357A (default) or PAM8403 through a filter. The radio restarts |
| `testtx 5 1000` | transmit a 1000 Hz tone to the channel for 5 s, as if the button were pressed: tests transmission without the button and microphone. 1–20 s (default 3), 100–4000 Hz (default 1000) |
| `testmic 10` | transmit the live microphone to the channel for 10 s without the button: 1–80 s (default 10) |
| `micraw` | for 1 second shows the swing of the "raw" microphone samples on the left (L) and right (R) channels |
| `reboot` | restart |
| `factory` | full reset: erases everything, including the key and the bridge address ([reset](#resetting-settings)) |

Every command except `help` and `show` gets a reply line `OK …` or `ERR …`; an unknown command
(or `name` without a name) gets `ERR неизвестная команда` ("unknown command").

**How to read `show`.** The «связь: N» (link: N) line is a number: `0` no Wi-Fi, `1` no key, `2` looking for the bridge,
`3` wrong network key, `4` connected. «мост:» (bridge:) — the address the radio is working with right now;
«адрес в настройках» (address in settings) — what's entered (empty — searching your own network). «чужих пакетов»
(foreign packets) — packets with an invalid signature: if this number grows, the radio is getting replies from a bridge
or device with a different key.

**How to read `micraw`.** While the command is running, talk into the microphone or tap on it. The channel the
microphone is connected to shows a wide swing of numbers; the other one shows about zero or the same number over and
over. Signal on R — enable «Микрофон на правом канале» (Microphone on the right channel) or move the microphone's L/R
pin to GND.

`testtx` and `testmic` work only when the radio is connected to the bridge; without a connection you'll get a low beep,
just like with a normal press. You can hear the tone on a second radio or record the channel on a computer:
`python3 hub/sim_radio.py --hub 203.0.113.10 --key XXXX-XXXX-XXXX-XXXX listen --seconds 30 --out efir.wav`.
