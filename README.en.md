[Русский](README.md) · **English**

# Wi-Fi Radio Ai-Sheff

## What it is

An open-source, battery-powered push-to-talk walkie-talkie on an ESP32-S3 board. There is just one button: press it,
hear a short "beep-beep", speak, let go — every other radio on your network has heard you.

The radios do not talk to each other directly. Each one joins an ordinary 2.4 GHz Wi-Fi network — at home, at the
office, or a phone hotspot — and sends speech through a **bridge**: a small Python 3 program (two files, standard
library only) on a computer that is always on and reachable from the internet, such as a cheap VPS or an always-on
PC (Linux, Windows, macOS). That is why radios in different cities hear each other just as well as radios in
neighbouring rooms: **the range is limited only by where there is Wi-Fi with internet access**.

Inside: an ESP32-S3 DevKitC-1, an INMP441 I2S microphone, a MAX98357A I2S amplifier, a 12-LED WS2812 ring, a KY-040
volume knob and an 18650 cell. Speech is 16 kHz IMA ADPCM; every packet is signed and the audio is AES-128-CTR
encrypted with a shared network key, and the bridge forwards speech without decrypting it.

![The radio in its case: the speaker behind a white lens disc with the LED ring glowing around it; the PTT button on the side, the volume knob and power switch on top](docs/img/assembly.png)

The 68 × 38 × 170 mm case is 3D-printed without supports. On the front, the speaker sits behind a white lens disc
through which a ring of 12 LEDs shines; on the side is the PTT button; on top are the volume knob and the power
switch; on the bottom is the USB-C charging port.

Code comments, the radio settings page and the bridge status page are in Russian; the documentation is available in
English and Russian. License: MIT.

## Features

- **A single PTT ("push-to-talk") button.** One person talks at a time: the bridge gives the channel to whoever
  pressed first, and anyone else who presses hears a "boo-boo-boo" busy tone. The bridge cuts off a stuck button
  after 90 seconds.
- **Radios on different Wi-Fi networks** — home, office, warehouse, country house, a phone hotspot. A radio remembers
  up to three networks and connects to the one with the best signal. 2.4 GHz Wi-Fi is required: the ESP32-S3 cannot
  see 5 GHz networks.
- **Encryption with the network key.** The network key is a shared password for all your radios and bridges, a string
  like `XXXX-XXXX-XXXX-XXXX`. Every packet is signed with this key and speech is encrypted (AES-128). A stranger's
  radio or a random scanner on the internet cannot get onto the channel. The bridge does not decrypt speech — it only
  forwards it.
- **Over-the-air (OTA) firmware updates through the bridge.** Put a new firmware file into the bridge's folder — every
  radio, once it has been idle for 20 seconds, downloads it, checks the checksum (sha256 — the file's "fingerprint")
  and only then restarts into the new version. No need to touch the radios.
- **Howl protection.** If two radios lie next to each other, the sound can loop round and start to howl — like a
  microphone held up to a loudspeaker. The firmware finds the howl frequency by itself and cuts it out with a narrow
  filter.
- **LED ring indication.** The ring around the speaker shows the connection, who is talking, the volume, the battery
  charge and update progress. What each colour means is in the [Ring colours](docs/en/TROUBLESHOOTING.md#ring-colours)
  table.
- **Battery charge estimate** for the 18650 cell: a gauge on the ring, a "time to charge" signal, and a percentage on
  the radio's page and on the bridge's page. Voltage dips while talking do not cause false alarms.
- **Setup from a phone.** A new radio opens its own `RADIO-XXXX` access point. Connect to it with your phone and the
  settings page opens by itself, like hotel Wi-Fi: Wi-Fi, bridge address, network key, radio name, volume,
  brightness.
- **Bridge status page:** which radios are online, how much charge each one has, who is talking right now, and an
  event log. The bridge points out faults by itself: «в передаче одна тишина — проверьте микрофон» ("only silence in
  the transmission — check the microphone"), «не тот ключ сети?» ("wrong network key?").
- **The bridge is a Python program** with no third-party libraries (two files: `walkie_hub.py` and `wt_proto.py`): it
  runs on Linux, Windows and macOS. Ready-made service installers are included for Linux (systemd, with a self-check
  every 2 minutes) and for Windows (Task Scheduler).
- **Audio:** 16 kHz, 4-bit IMA ADPCM compression — about 86 kbit/s per person talking, so mobile internet is enough. The
  receiver buffers 60–160 ms of speech to smooth out network hiccups.

## How it works

A radio sends speech to the bridge in 20 ms chunks. The bridge decides who has the channel and forwards the speech to
all the other radios. The connection is always started by the radio, so there is no router setup on the radios'
side — incoming ports only need to be open on the bridge: **UDP 47000** (radio traffic) and **TCP 47080** (status
page and firmware downloads).

```
   Home                                                              Office
 ┌─────────┐  Wi-Fi  ┌────────┐                     ┌────────┐  Wi-Fi  ┌─────────┐
 │ radio 1 │ ~~~~~~~ │ router │───┐             ┌───│ router │ ~~~~~~~ │ radio 2 │
 └─────────┘         └────────┘   │  internet   │   └────────┘         └─────────┘
                                  ▼             ▼
                            ┌───────────────────────┐
                            │        BRIDGE         │  cloud server
                            │  UDP 47000 — radios   │  or a computer
                            │  TCP 47080 — web page │  with a fixed address
                            └───────────────────────┘
                                          ▲
 ┌─────────┐  Wi-Fi hotspot  ┌─────────┐  │
 │ radio 3 │ ~~~~~~~~~~~~~~~ │  phone  │──┘
 └─────────┘                 └─────────┘
```

What happens when you press the button:

1. The radio asks the bridge: "may I talk?". If the channel is free, you hear "beep-beep" and the ring lights up red.
   If someone is already talking, you hear the "boo-boo-boo" busy tone — wait.
2. While the button is held, your speech goes to the bridge compressed and encrypted.
3. The other radios play it with a small buffer against network hiccups; their ring "dances" green in time with the
   voice.
4. When you let go of the button, the others hear a short "tee-doo": the transmission is over.

### Without the cloud

The bridge can run on a computer in your own network. If the «Адрес моста» (Bridge address) field on the radio is
left empty, the radio finds the bridge on its network by itself with a broadcast request (a packet "to everyone on
this network").

```
 ┌────────┐  Wi-Fi  ┌────────┐     ┌────────────────┐
 │ radios │ ~~~~~~~ │ router │─────│ PC with bridge │    «Адрес моста» (Bridge address) field on the radios — empty
 └────────┘         └────────┘     └────────────────┘
```

If you have several sites, each one can have its own bridge, and the bridges link up with each other (the `peers`
field in the bridge settings). The bridges need to reach each other over UDP 47000 — through the internet or through
a VPN (for example, Tailscale or WireGuard). The advantage of this setup: within one site, the radios keep talking to
each other even when the internet is down.

```
 radios ~~~ [ bridge "Home" ] ◄═════ peers: internet or VPN ═════► [ bridge "Office" ] ~~~ radios
```

How to choose and set up a bridge — [docs/en/BRIDGE.md](docs/en/BRIDGE.md). Packet format —
[docs/en/protocol.md](docs/en/protocol.md).

## What you need

| Part | What for |
|---|---|
| ESP32-S3-DevKitC-1 N16R8 board (or a YD-ESP32-S3 clone) | the radio's "brain" and Wi-Fi |
| INMP441 microphone (digital, I2S) | voice |
| MAX98357A amplifier (digital, I2S, 3 W) | drives the speaker |
| Speaker, 8 Ω, 3 W, 32 × 32 mm | sound |
| WS2812B ring with 12 LEDs, Ø 50 mm | indication |
| KY-040 volume knob (rotary encoder with a push button) | volume; press to mute |
| 16 mm push button (anti-vandal) | PTT ("talk") |
| Miniature toggle switch | power on/off |
| 18650 cell and a holder for it | power |
| USB-C charger module, 5 V 2 A ("Power Bank Mini" type) | charges the battery and supplies 5 V |
| Two 100 kΩ resistors, a 1N4007 diode, two 5-way lever connectors (WAGO 221-415 type), female-female jumper wires, heat-shrink tubing, 3 mm self-tapping screws | battery voltage divider, LED ring power, assembly |
| Case: ≈ 140 g of PLA and the white lens disc for the speaker | 3D printing, ≈ 6 h |

**About 4,200 ₽ per radio** for parts and plastic if you build three at once (a single radio on its own costs about
4,500 ₽, because some parts are sold in packs). Prices are from Russian stores as of September 2026; all the parts
are common and easy to find on AliExpress and similar marketplaces. The exact list with quantities, "where to buy"
examples and screw sizes is in [docs/en/BUILD.md](docs/en/BUILD.md).

You will also need a soldering iron, a multimeter (it makes troubleshooting much easier), a USB-C cable **that
carries data**, and a computer to act as the bridge: an inexpensive cloud server or a home PC that is always on.

Calculated run time on a single 18650 cell: about 15 hours.

## How to build it — step by step

| Step | Where it is described |
|---|---|
| 1. Buy the parts, print the case, solder and assemble | [docs/en/BUILD.md](docs/en/BUILD.md) — parts, printing, wiring diagram ([wiring.svg](docs/img/en/wiring.svg)), layout inside the case ([layout.svg](docs/img/en/layout.svg)) |
| 2. Set up your own bridge and come up with a network key | [docs/en/BRIDGE.md](docs/en/BRIDGE.md) |
| 3. Flash the radio, enter the Wi-Fi, bridge address and key, learn how to use it | [docs/en/SETUP.md](docs/en/SETUP.md) — prebuilt firmware is in [firmware/dist/](firmware/dist/) |
| 4. If something doesn't work | [docs/en/TROUBLESHOOTING.md](docs/en/TROUBLESHOOTING.md) — symptom → cause → what to do |
| For developers: how the packets work | [docs/en/protocol.md](docs/en/protocol.md) |

Tip: flash the board and test it on the bench **before** putting it into the case — tracking down a fault afterwards
takes longer.

## What's where

| Folder / file | Contents |
|---|---|
| [firmware/walkie/](firmware/walkie/) | radio firmware (Arduino, esp32 core 3.3.12). Board pins and the main parameters are in [config.h](firmware/walkie/config.h) |
| [firmware/dist/](firmware/dist/) | prebuilt firmware: `walkie-full-v9.bin` — the complete image for the first flash, written from address 0x0; `walkie.bin` + `walkie.json` — for distribution through the bridge |
| [hub/walkie_hub.py](hub/walkie_hub.py) | the bridge; [wt_proto.py](hub/wt_proto.py) — the protocol, [hub.example.json](hub/hub.example.json) — example settings |
| [hub/install_linux.sh](hub/install_linux.sh), [hub/install_windows.ps1](hub/install_windows.ps1) | install the bridge as a service on Linux and Windows |
| [hub/healthcheck.py](hub/healthcheck.py) | bridge self-check (a service "radio" knocks on the bridge) |
| [hub/sim_radio.py](hub/sim_radio.py) | a software radio on your computer: test the bridge, transmit a tone or a WAV file, record the channel, "parrot" mode |
| [tools/build_firmware.sh](tools/build_firmware.sh) | builds the firmware with arduino-cli → `firmware/dist/` |
| [tools/flash.sh](tools/flash.sh) | first flash over USB (macOS) |
| [tools/provision.py](tools/provision.py) | configures a radio over a USB cable: name, bridge, key, Wi-Fi (macOS; on Linux — with `--port`) |
| [tools/make_print.sh](tools/make_print.sh), [tools/bambu_profile.py](tools/bambu_profile.py) | slices the case in Bambu Studio for a Bambu Lab P2S (macOS) |
| [tools/howl_eval.cpp](tools/howl_eval.cpp), [tools/howl_chain.cpp](tools/howl_chain.cpp) | howl-protection test benches: a run over WAV recordings and a model of the "loop" between radios |
| [cad/radio_handheld.scad](cad/radio_handheld.scad) | OpenSCAD model of the case (parameters are at the top of the file); [measurements.md](cad/measurements.md) — part measurements |
| [stl/](stl/) | print-ready case parts |
| [tests/](tests/) | automated tests for the bridge, the protocol and the portable part of the firmware |
| [docs/](docs/) | instructions and images; English instructions are in [docs/en/](docs/en/) |

## Development

Tests — no hardware needed, about 30 seconds:

```bash
python3 -m unittest discover -s tests
```

What is tested (21 tests): the bridge with "software radios" on real UDP sockets — the channel, busy tone, silence,
cutting off a stuck button, a wrong key, firmware distribution, speech through two linked bridges, simultaneous
presses at two sites; the AES cipher against reference vectors, packet signatures; matching protocol constants in the
firmware and the bridge. The portable parts of the firmware (speech compression, receive buffer, battery estimate,
howl protection) are compiled with your computer's compiler and checked by their own tests, and the speech compression
is also compared against a Python reference. This needs `clang++` or `g++`; without them these tests are skipped.

Building the firmware (requires [arduino-cli](https://arduino.github.io/arduino-cli/); the script installs esp32 core
3.3.12 by itself):

```bash
tools/build_firmware.sh
```

The build targets "ESP32S3 Dev Module", 4 MB flash, the Minimal SPIFFS partition scheme (two firmware slots — needed
for over-the-air updates), PSRAM disabled, USB in Hardware CDC mode. It also works on boards with 16 MB flash.
The output goes to `firmware/dist/`.

Releasing a new version to all radios:

1. Increase `FW_VERSION` in [firmware/walkie/config.h](firmware/walkie/config.h) — the bridge only distributes
   firmware with a higher number.
2. Build it: `tools/build_firmware.sh`.
3. Copy `firmware/dist/walkie.bin` and `firmware/dist/walkie.json` into the `firmware/` folder next to `hub.json` on
   the bridge. The bridge picks them up by itself within 10 seconds — see [docs/en/BRIDGE.md](docs/en/BRIDGE.md) for
   details.

Rules for anyone editing the code:

- The protocol is defined in two places — [firmware/walkie/protocol.h](firmware/walkie/protocol.h) and
  [hub/wt_proto.py](hub/wt_proto.py); change them together (along with the description in
  [docs/protocol.md](docs/protocol.md) and its English version [docs/en/protocol.md](docs/en/protocol.md));
  `tests/test_parity.py` checks that the constants match.
- The build runs without ctags, so functions in `walkie.ino` are declared **before** they are used.
- Code comments are in Russian.

## License

[MIT](LICENSE): you may use, modify and sell it, as long as you keep the copyright notice. No warranty.
