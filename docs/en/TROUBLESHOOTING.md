[Русский](../TROUBLESHOOTING.md) · **English**

# If something isn't working

This page collects faults that turned up on real radios, plus hints from the firmware itself.
Every item follows the same pattern: **symptom → cause → what to do**.

The radio's settings page, the bridge status page and the logs are in Russian. Where this page quotes them, the
original Russian text is kept in «guillemets» with the English meaning in brackets, so you can match what you see.

Jump to:
[Ring colours](#ring-colours) ·
[Sounds](#radio-sounds) ·
[Where to look for clues](#where-to-look-for-clues) ·
[Connection](#connection) ·
[Audio](#audio) ·
[LED ring and knob](#led-ring-and-volume-knob) ·
[Power and charging](#power-and-charging) ·
[USB and firmware](#usb-and-firmware) ·
[Bridge](#bridge)

---

## Ring colours

The LED ring always shows the most important thing that is going on. The rows below go from more important to less
important: while an update is running, for example, the ring will show neither the volume nor the connection.

| What you see on the ring | What it means | What to do |
|---|---|---|
| The whole ring is dim white right after switching on | the radio is starting up | nothing; after a second or two the ring switches to its normal indication |
| The ring gradually fills with purple while you hold the PTT (push-to-talk) button during power-on | entering setup mode: after 1.5 s the `RADIO-XXXX` access point will open | release the button once the ring has filled; a low beep after that is normal |
| The whole ring flashes red rapidly while you hold the volume knob pressed | from the 5th second — a warning that settings are about to be reset | changed your mind — let go; at the 10th second the settings are reset (see [Reset](#reset-settings)) |
| A blue bar grows around the ring | the firmware is being updated | **do not switch off** the radio; after the update it restarts by itself |
| The whole ring flashes **red** briefly and rapidly | the channel is busy: someone is already talking, or the bridge refused | wait until the other transmission ends and press again |
| The whole ring flashes **orange** briefly and rapidly | you pressed the button but there is no connection (or the bridge didn't answer within 0.7 s), or the connection to the bridge has just been lost | check what the ring shows when idle — further down this table |
| A bar on the ring after you hold the knob for 1–4 s | battery charge: the more LEDs, the more charge | green — over 40 %, amber — 16–40 %, red — 15 % or less |
| …a dim white ring instead of the bar | the charge is still being measured (the first ≈ 20 s after power-on) | wait and look again |
| …a blue ring instead of the bar | the radio doesn't see the battery and assumes it is powered from USB | if the battery is in place, check the battery voltage divider, see [below](#charge-always-shows-нет-питание-от-usb-no-battery-usb-power) |
| A white bar for 1.5 s after turning the knob | volume (0–20) | a purple bar means the sound is muted |
| A red ring "dances" in time with your voice | you are talking and can be heard | — |
| Two white LEDs run quickly around the ring | you pressed the button; the radio is waiting for the bridge's permission | wait for the "beep-beep", then talk |
| A green ring "dances" in time with a voice | someone else is talking | — |
| A light blue ring gently "breathes" | someone is talking, but your sound is muted | press the knob to unmute |
| The whole ring slowly "breathes" purple | the `RADIO-XXXX` access point is open and there is no connection to the bridge: the radio doesn't know any Wi-Fi network, no network key is set, Wi-Fi has been gone for more than a minute, or the access point was opened with the button at power-on | connect to `RADIO-XXXX` with a phone and read the status line on the page. If the network simply dropped out, don't touch anything — the radio will reconnect by itself |
| One blue LED runs around the ring | looking for Wi-Fi | wait; after a minute without a network the access point opens (purple "breathing") |
| Two opposite orange LEDs blink: half a second on, half a second off | Wi-Fi is there, the bridge isn't | see [Radio can't find the bridge](#radio-cant-find-the-bridge) |
| Red and purple LEDs alternate and slowly shift around | the bridge answers, but the network key doesn't match | see [Wrong network key](#wrong-network-key) |
| Two opposite **green** LEDs flash briefly once every 3 s | all is well, the radio is connected | — |
| …**amber** flashes once every 3 s | connected to the bridge, but not all bridges in its `peers` list are online | see [Amber flashes](#amber-flashes-instead-of-green) |
| …**red** flashes once every 3 s | the charge has been at 15 % or less for half a minute — time to charge | put it on charge |
| One purple LED is lit steadily (along with the flashes) | the sound is muted | press the knob |
| The ring glows steadily and dimly (green or amber) | the radio is connected and running on USB power, not the battery | — |

When idle, the ring is almost dark — just a short flash once every 3 seconds: that way it uses less battery.

## Radio sounds

| Sound | What it means |
|---|---|
| rising "beep-beep" | the channel is yours — talk |
| "boo-boo-boo" (three low beeps) | the channel is busy (busy tone) |
| one long low beep | no connection: you pressed the button with no connection, the bridge didn't answer, or the button was held for more than 90 s — release it and press again |
| two falling tones | the connection to the bridge is lost (it has been silent for more than 6.5 s) |
| rising "tee-doo" | the radio has connected to the bridge |
| falling "tee-doo" | someone else's transmission has ended (turned off with the «Сигнал в конце чужой передачи» (Tone at the end of someone else's transmission) checkbox) |
| short click | one volume step |
| three falling notes | battery at 15 % or less: once every 5 minutes, and from 5 % — once a minute |

These service signals are heard even when the sound is muted — quietly, so you know the button worked.

## Where to look for clues

1. **Radio settings page.** On the same network as the radio, open `http://radio-xxxx.local` (xxxx is the last
   4 characters of its access point name, in lowercase). If `.local` doesn't open (happens on Android), find
   `radio-xxxx` in your router's device list and type its numeric address. Or hold the PTT button while
   switching on — the `RADIO-XXXX` access point opens; connect to it and open `http://192.168.4.1`.
   The page shows: connection status, Wi-Fi signal, the bridge and the delay to it, charge, «Приём: сыграно / потеряно»
   (Received: played / lost), «Работает … · запуск: *reason for the last start*» (Running … · started by: …),
   «Свист рядом с другой рацией» (Howl near another radio) and, if something is broken, an «Ошибка» (Error)
   line.
2. **Bridge status page** `http://203.0.113.10:47080/` (use your own bridge's address; if `status_token` is set in
   the bridge settings, append `?t=token`). It shows the list of radios with their charge, who is talking right now,
   and the event log.
3. **Radio log over USB.** Connect the board with a cable and open a serial monitor at 115200 baud — for example, in
   the Arduino IDE (Serial Monitor, line ending "New Line") or like this:

   ```bash
   arduino-cli monitor -p /dev/cu.usbmodem1101 -c baudrate=115200
   ```

   Your port name will be different: on macOS `/dev/cu.usbmodem…` or `/dev/cu.wchusbserial…`, on Linux
   `/dev/ttyACM0` or `/dev/ttyUSB0`, on Windows `COM3` and so on. The log goes to both of the board's USB ports.
   Useful commands (type one and press Enter):

   | Command | What it does |
   |---|---|
   | `show` | the full status: id, name, bridge, Wi-Fi, charge, amplifier, receive statistics |
   | `help` | a list of all commands |
   | `micraw` | one second of raw microphone data on both channels — a microphone check without the bridge |
   | `testmic 10` | transmits the live microphone to the channel for 10 seconds, as if the button were held |
   | `testtx 5 1000` | transmits a 1000 Hz tone to the channel for 5 seconds instead of the microphone |
   | `amp i2s` / `amp pdm` | which amplifier is fitted: MAX98357A (I2S) or the analogue PAM8403 via a filter; the radio restarts |
   | `reboot` | restart |
   | `factory` | full reset, including the network key, the name and the bridge address |

4. **Software radio on a computer** — to check the bridge and the audio when you only have one radio at hand
   (substitute your bridge's address and your own key):

   ```bash
   # did the bridge answer, how many radios are on the network
   python3 hub/sim_radio.py --hub 203.0.113.10 --key XXXX-XXXX-XXXX-XXXX status
   # "parrot": say a phrase into the radio, release the button — you will hear yourself
   python3 hub/sim_radio.py --hub 203.0.113.10 --key XXXX-XXXX-XXXX-XXXX echo
   # 10 seconds of an 800 Hz tone to all radios — speaker check
   python3 hub/sim_radio.py --hub 203.0.113.10 --key XXXX-XXXX-XXXX-XXXX tone --seconds 10
   ```

5. **Multimeter.** DC voltage mode, 20 V range (on the dial — `V⎓ 20` or `DCV 20`).
   Black probe on GND, red probe on the point you are checking. On signal lines carrying pulses, a multimeter shows
   the average value: on clock lines that is ≈ 1.6 V — half of 3.3 V.

---

## Connection

### Radio won't connect to Wi-Fi

**Symptom:** the blue LED runs around for a long time, then the ring "breathes" purple; the page says «нет Wi-Fi»
(no Wi-Fi).

**Causes and what to do:**

- **5 GHz network.** The ESP32-S3 only works with 2.4 GHz Wi-Fi. Enable 2.4 GHz on the router. For an iPhone
  hotspot, turn on "Maximize Compatibility" — then it runs on 2.4 GHz.
- **Wrong password.** Connect to `RADIO-XXXX` and add the same network again with the correct password — the password
  will be updated.
- **A guest network with a login page** (as in hotels and cafés) won't work: the radio can't press "Log in".
- **A hidden network** won't appear in the «Найти» (Find) list — type its name in manually.
- **The phone is still connected to `RADIO-XXXX`.** While anyone is connected to the access point, the radio doesn't
  try to join Wi-Fi, so as not to get in the way of setup. When you're done, disconnect the phone from it.
- The radio remembers **up to three networks**; if you add a fourth, the oldest one is forgotten.

If there has been no Wi-Fi for 15 minutes and nobody is connected to its access point, the radio restarts itself —
that's normal, it's how it cures a "stuck" Wi-Fi.

### Radio can't find the bridge

**Symptom:** two orange LEDs blink; pressing the button gives a long low beep and orange flashing;
the radio settings page says «ищу мост» (looking for the bridge).

**Causes and what to do:**

1. **The «Адрес моста» (Bridge address) field is empty.** Then the radio only looks for the bridge on its own network
   (the page shows «Мост: ищу в своей сети» (Bridge: searching my own network)). A bridge on the internet has to be
   entered: `203.0.113.10`, `radio.example.com` or with a port — `radio.example.com:47000`. The default port is 47000.
   If the bridge really is on your network, it must be on the same subnet as the radio, and the router must not have
   "client isolation" enabled (guest networks usually do) — otherwise the radio won't hear it.
2. **The bridge name can't be resolved.** The radio page shows «Ошибка: адрес «…» не найден» (Error: address "…" not
   found). Check the spelling and the DNS record for the name (DNS is the internet's "phone book" that turns a name
   into an address).
3. **The bridge isn't running.** Open the bridge status page `http://203.0.113.10:47080/`. If it doesn't open, check
   the service: on Linux `systemctl status walkie-hub` and `journalctl -u walkie-hub -f`, on Windows — the `hub.log`
   file next to `hub.json`.
4. **UDP port 47000 is closed on the server.** The bridge installers open it themselves in the built-in firewall (ufw on
   Linux, Windows Firewall), but a cloud server also has **network rules in the provider's console** — UDP 47000 and
   TCP 47080 have to be allowed there by hand. If the bridge is at home behind a router, the router needs port
   forwarding of these ports to the computer running the bridge. Details — [BRIDGE.md](BRIDGE.md).
5. **The radio's network doesn't let UDP out.** Some office and guest networks block it. Try the radio on a phone
   hotspot.

A quick check from a computer on the same network as the radio:
`python3 hub/sim_radio.py --hub 203.0.113.10 --key XXXX-XXXX-XXXX-XXXX status`. The reply «мост … ответил за N мс»
(bridge … answered in N ms) means the bridge and the port are fine — look for the mistake in the radio's settings.
«Мост … не ответил за 5 с» (bridge … did not answer within 5 s) means the problem is the bridge, the port or the network.

### Wrong network key

**Symptom:** red and purple LEDs alternate; the radio page says «мост отвечает, но ключ сети не
совпадает» (the bridge answers, but the network key doesn't match); the bridge log shows «⚠ пакет раций с неверной
подписью от … — не тот ключ сети?» (radio packet with an invalid signature from … — wrong network key?).

**Cause:** the key in the radio doesn't match the `key` field in the bridge's `hub.json`.

**What to do:**

- Enter the same key in the radio as in the bridge. Letter case, spaces and hyphens don't matter: `k7q2 mx9p…` and
  `K7Q2-MX9P-…` are the same key. It needs at least 12 letters and digits.
- **Latin letters and digits only.** The firmware counts only Latin letters and digits in the key, while the Python
  bridge counts Cyrillic letters too, so a key with Cyrillic in it will never match between them. The safest choice is
  a key generated by the bridge itself: `python3 hub/walkie_hub.py --genkey`.
- Changed the key in `hub.json`? Restart the bridge: it reads its settings only at startup.
- The radio can store a key of up to 39 characters including hyphens — anything longer is cut off.
- Linked bridges (`peers`) must all share one key as well.

If no key is set at all, the radio keeps its access point open (purple "breathing"), and the page says «нет ключа
сети» (no network key).

### Amber flashes instead of green

**Cause:** the radio is connected to its own bridge, but one of the bridges in that bridge's `peers` list is
unreachable. Radios on this bridge hear each other, but radios behind the unreachable bridge don't.

**What to do:** on the bridge status page, look at the «Другие мосты» (Other bridges) section and the log («⚠ мост
пропал: …» (bridge lost: …)). Check that the second bridge is running, that the bridges can reach each other over
UDP 47000 (via the internet or a VPN), and that they have **different names** (`name` in `hub.json`): a bridge derives
its number from its name, and it will take a bridge with the same name for itself.
See [BRIDGE.md](BRIDGE.md).

### "Boo-boo-boo" when you press, though nobody is talking

- The bridge keeps the channel for the speaker for another 0.8 s after their last sound — press a little later.
- If people in two places press at the same moment, the same speaker wins on every bridge, and the other one gets the
  busy tone.
- Another radio's button is stuck: the bridge will end its transmission by itself after 90 s. The bridge status page
  shows who is talking.

### Audio breaks up or "gurgles"

**Cause:** network losses — most often a weak Wi-Fi signal at the radio.

**What to do:**

- Check the Wi-Fi signal on the radio page: −50 dBm is excellent, −70 dBm and below is weak. Move the radio closer
  to the router or add an access point.
- The board's antenna is the protruding end of the module, the part without the metal cover. Don't press wires or
  metal against it.
- The «Приём: сыграно / потеряно» (Received: played / lost) line on the radio page shows how many 20 ms chunks of
  speech were lost. The software radio gives a more detailed picture of delivery jitter:
  `python3 hub/sim_radio.py --hub 203.0.113.10 --key XXXX-XXXX-XXXX-XXXX listen --seconds 60`.
- The radio buffers 60–160 ms of speech and, after dropouts, automatically keeps a bigger margin.

---

## Audio

### Bridge reports «в передаче одна тишина — проверьте микрофон» ("only silence in the transmission — check the microphone")

**Symptom:** the bridge log shows «⚠ *radio name*: в передаче одна тишина — проверьте микрофон», and the radio list
shows «тишина в передачах: N» (silence in transmissions: N). Other radios hear only silence. The bridge raises this
warning if a transmission lasted more than 0.5 s and the sound level hardly rose above zero.

**The most common cause is an unsoldered pin of the pin header on the microphone module**, most often SCK. The
female-female jumper wire is pushed onto the pin, the pin has voltage on it, but the signal never reaches the module
itself.

**What to do.** Switch the radio on and check the pins of the INMP441 microphone with a multimeter (20 V range, black
probe on GND). Put the red probe **on the solder joint on the module side**, not on the header pin or the wire's
connector:

| Microphone pin | Should read |
|---|---|
| VDD | 3.3 V |
| SCK | ≈ 1.6 V (average of the pulses) |
| WS | ≈ 1.6 V |
| L/R | 0 V (connected to GND) |

In the faulty radio, SCK at the module did not show ≈ 1.6 V, and SD showed ≈ 0. Resolder **all six** header pins
and check again.

**Microphone on the right channel.** If L/R reads 3.3 V instead of 0, the microphone puts its sound on the right
channel, while the firmware listens to the left one. Either connect L/R to GND, or tick «Микрофон на правом канале
(вывод L/R на 3,3 В)» (Microphone on the right channel (L/R pin at 3.3 V)) on the radio page.

**Check without the bridge.** With the `micraw` command over USB (see [Where to look for clues](#where-to-look-for-clues))
the radio shows what is coming from the microphone: `L: min…max  R: min…max`. Say something and repeat the command: on
a working microphone the spread on its own channel (L, if L/R is on GND) grows noticeably with your voice. If both
channels sit on a single number (`0…0`, `−1…−1`), there is no data: check the soldering and the wires SD → pin 6,
SCK → pin 5, WS → pin 4.

**End-to-end check.** Run the "parrot" (`sim_radio.py … echo`), say a phrase and release the button — you should
hear yourself. Or use the `testmic 10` command to send 10 seconds of live microphone to the channel without the
button.

### No sound from the speaker

**Simple things first:**

- The volume isn't at zero: turn the knob — the white bar should grow.
- The sound isn't muted: if a single purple LED is lit or the ring "breathes" light blue, press the knob.
- The radio page doesn't show the line «Ошибка: звук не запустился (проверьте провода I2S)» (Error: audio didn't start
  (check the I2S wires)).
- The `show` command over USB says «усилитель: MAX98357A (I2S)» (amplifier: MAX98357A (I2S)). If it says PAM8403 but a
  MAX98357A is fitted — `amp i2s`.

**A common cause when the power is fine is a bad contact on the DIN or GND pin of the MAX98357A amplifier.** On one of
the radios, sound only appeared while the multimeter probes were pressed against these pins. Resolder the amplifier's
pin header.

**Multimeter check** on the amplifier pins (black probe on GND). To get some sound going, send a tone from the computer
(`sim_radio.py … tone --seconds 20`) or turn the knob — every step makes a click:

| Amplifier pin | Should read |
|---|---|
| Vin | 4.5–5 V |
| SD | ≈ 3.3 V while sound is playing and for another 3 s after. In silence the firmware puts the amplifier to sleep (so it doesn't hiss) — then SD reads 0 V, which is normal |
| BCLK, LRC | ≈ 1.6 V (average of the pulses) |

Also check that:

- the speaker is soldered to the "+" and "−" pads of the amplifier's **output**, and the speaker's minus is **not**
  connected to GND — both of the amplifier's outputs are driven;
- the amplifier's GAIN pin isn't connected to anything;
- the wires go: DIN → pin 7, BCLK → pin 15, LRC → pin 16, SD → pin 17.

### The other person sounds quiet or distorted

- «Автоусиление микрофона» (microphone automatic gain control, AGC) is on by default: the firmware adjusts the gain by
  itself (up to +36 dB) and keeps the sound from overloading. If it has been turned off, the microphone level is set by
  the «Усиление, если автоусиление выключено» (Gain if AGC is off) field — from 0 to 42 dB, default 24.
- Your own volume is set with the knob: 21 steps (0–20) of 2 dB each.

### Howl when two radios are close together

**Cause:** an acoustic loop. The talking radio's microphone hears the speaker of the radio next to it, the sound goes
round and round through the bridge and at some frequency builds up into a howl — ordinary walkie-talkies howl in
exactly the same way when they stand next to each other.

**What to do:** the firmware detects a growing howl by itself and cuts out its frequency with a narrow filter; if the
howl comes back in the same transmission, it also turns the microphone down. The radio page shows whether the
protection kicked in: «Свист рядом с другой рацией: гасили N раз, последний на … Гц» (Howl near another radio:
suppressed N times, last one at … Hz). But it's better not to let it come to that: move the radios apart or turn down
the volume on the listening one.

---

## LED ring and volume knob

### Ring doesn't light up

- «Яркость подсветки» (LED brightness) on the radio page is set to 0 — raise it (the default is 60).
- If it never lit up at all within 10 seconds of power-on — check the power switch and the 5 V lever connector.
- The data wire must go from pin 18 to the ring's **DI** (DIN) input, not to the DO output.

### Ring flashes the "wrong" colours

**Cause:** the 1N4007 diode is missing from the ring's 5 V power wire (or it is fitted the wrong way round). WS2812
LEDs powered from exactly 5 V don't read the 3.3 V signal from the board well; the diode drops their supply to about
4.3 V, and the signal is read reliably.

**What to do:** put the diode in series with the ring's 5 V wire, **stripe towards the ring**.

### Knob turns the wrong way

Tick **«Регулятор крутится наоборот»** (Volume knob turns the other way) on the radio page. Or swap the CLK and DT wires:
per the wiring diagram, CLK → pin 2, DT → pin 42 — then clockwise is louder.

### Knob doesn't respond

Check the wires: CLK → pin 2, DT → pin 42, SW → pin 41, "+" → **3V3** (3.3 V only, not 5 V!),
GND → G on the board. Pressing the knob cap should give a click: push the knob cap onto the shaft leaving a 1.5–2 mm
gap above the end of the bushing.

---

## Power and charging

### Radio "switches itself off" when idle

**Symptom:** the radio was lying unused and went completely dark — the ring is off and it doesn't respond to the
buttons.

**Cause:** the charger module is built like a power bank. If the radio draws too little current, the module decides
the load has been unplugged and switches off its 5 V output. That's why «Экономия аккумулятора» (Battery saving — Wi-Fi
dozes while the channel is quiet) is **off by default**.

**What to do:** don't turn power saving on with modules like these. If the box is ticked, untick it on the radio page.

If the radio doesn't go dark but **restarts**, look at the «Работает … · запуск: …» (Running … · started by: …) line
on the page:

| Start reason | What it means |
|---|---|
| «включение питания» (power-on) | a normal power-on with the power switch |
| «просадка питания» (brownout) | the voltage dropped for an instant. Right after switching on with the power switch — normal (see below). In the middle of operation — the battery is running low, or there is a bad contact in the lever connectors |
| «сторож: зависание» (watchdog: hang) | the program didn't respond for 30 s and the watchdog restarted the radio |
| «сбой программы» (software crash) | a bug in the firmware — save the log over USB and attach it to a bug report (issue) in the repository |
| «перезапуск программой» (restart by software) | after a firmware update, the «Перезапустить» (Restart) button or the `reboot` command, a settings reset, an amplifier change, adding a network through the access point, or 15 minutes without Wi-Fi |

### Radio reboots when shaken

**Cause:** a wire isn't held well in a lever connector. The thin core of a female-female jumper wire doesn't sit
reliably in the 5-way lever connector.

**What to do:** strip 20–22 mm off the end, fold the core in half and twist it tightly — you get a doubled tip of
10–11 mm. Push it all the way in, close the lever and give it a tug: the wire must not come out.

### Power switch doesn't turn the radio on after a battery swap

**Cause:** the battery was taken out and put back — the charger module has "fallen asleep" and doesn't turn on its 5 V
output.

**What to do:** plug in a USB-C charger for a couple of seconds. If the module has a button, you can press that
instead.

### Charger module LEDs stay on after the power switch is turned off

The firmware can't fix this: the board has no power at that moment. Possible causes:

- a charger is plugged in — then the LEDs are supposed to be on;
- the module has switched to "low current" mode (some modules turn it on by themselves);
- something is connected to the module's output bypassing the power switch — check that the OUT+ output goes **only**
  to the power switch;
- the module is from a different series and behaves in its own way.

A module like this slowly drains the battery even when the radio is switched off. One possible modification: move the
power switch to the battery wire (between the 18650 holder and the module's B+ pad). Then the module is completely
unpowered when the radio is off, but the battery only charges while the power switch is on.

### 0.4–0.5 V on the 5 V bus with the radio switched off

Normal. That's the leftover charge in the capacitors plus a tiny current through the battery voltage divider (two
100 kΩ resistors — hundredths of a milliamp). The voltage slowly falls; it doesn't drain the battery.

### Start reason «просадка питания» (brownout) after switching on with the power switch

Normal: at the moment you switch on, the capacitors on the boards charge up and the voltage sags for a fraction of a
second. It doesn't affect how the radio works.

### Charge shows «измеряется…» (measuring…) or doesn't change

That's by design: under load (Wi-Fi, speaker, ring) the battery voltage sags by 0.2–0.3 V, and an honest reading at
that moment would show too low a charge. The estimation rules (details in
[firmware/walkie/battery_est.h](../../firmware/walkie/battery_est.h)):

- **For the first ≈ 20 s after power-on** the charge isn't reported — «измеряется» (measuring): for 15 s the radio
  takes no readings (Wi-Fi connects at full power), then it collects 5 quiet readings.
- **While talking and receiving**, and also during a firmware update or a Wi-Fi search, no readings are taken — and
  none for another 3 s after. The charge freezes for that time and catches up right after the pause. If the radio is
  "busy" for more than 5 minutes without a break, readings are taken anyway — so that a discharge doesn't go
  unnoticed.
- A reading is taken once a second; the estimate is the highest of the last 30 quiet readings. It rises immediately and
  falls gradually, so short voltage dips don't throw off the percentage.
- **"Time to charge"** (red flashes and three falling notes) — only if the charge stays at 15 % or less for 30 s in a
  row. It clears when the charge rises to 18 %.
- The percentage is a rough voltage scale: 3.30 V is 0 %, 4.15 V is 100 %.
- While charging, the percentage reads high: the voltage is higher under charge. The accurate value comes after you
  unplug the cable.

The raw last reading is shown by the `show` command over USB («замер N мВ» (reading N mV)). If the percentage is far
off from the battery voltage measured with a multimeter, check that both divider resistors are 100 kΩ.

### Charge always shows «нет (питание от USB)» (no battery, USB power)

**Cause:** pin 8 sees less than 2.5 V when converted to battery voltage — the firmware decides there is no battery.

**What to do:** check the divider: 100 kΩ resistors to the **B+** and **B−** pads of the charger module (not to OUT+),
with the divider's midpoint wired to pin 8. Pin 8 should show about half the battery voltage (1.6–2.1 V). Without the
divider the radio works, but it won't know when the battery is running low.

---

## USB and firmware

### Board not visible over USB, or "No serial data received"

- **A charge-only cable** — use a cable that carries data.
- **The COM port doesn't work with a C-to-C cable.** On some boards the COM port lacks the necessary resistors, and
  with a USB-C to USB-C cable the board doesn't even get power. Connect to the **USB** port (the ESP32-S3's native
  port) or use a USB-A to USB-C cable.
- **Enter the bootloader by hand:** hold the **BOOT** button, press and release **RST**, release BOOT. Now flash. If
  after flashing through the USB port the board stays in the bootloader, press RST.
- On Windows you may need a driver for the USB-UART chip that the COM port goes through (on clone boards it is
  usually a CH340 or CH343).

How to flash — [SETUP.md](SETUP.md).

### Firmware won't update over the air

The radio downloads new firmware from the bridge only when all of these are true at once:

- the number in `walkie.json` on the bridge is **greater** than the radio's version (shown on the radio page, in the
  «Прошивка» (Firmware) section);
- the radio is connected to the bridge and nobody has talked for 20 seconds;
- the radio can reach the bridge's **TCP 47080** — the firmware is downloaded from there. If the port is closed, there
  will be no update either;
- the previous attempt didn't fail less than an hour ago. After a failure the radio waits an hour; restart it to try
  straight away.

The bridge status page should show the line «прошивка для раций: версия N» (firmware for radios: version N); while a
radio is downloading the file, the log shows «рация … скачивает прошивку N» (radio … is downloading firmware N), and a
blue bar grows on the ring. The bridge rescans its `firmware/` folder every 10 s.

By hand: radio page → «Загрузить прошивку» (Upload firmware) → the **`walkie.bin`** file. Don't confuse it with
`walkie-full-v…bin` — that one is only for the very first flash over a cable, at address 0x0.

### Radio settings page won't open

- The computer or phone must be on the same Wi-Fi network as the radio.
- `http://radio-xxxx.local` doesn't open everywhere (it often doesn't on Android) — find the radio's address in the
  router's device list or with the `show` command over USB.
- The access point always works: hold the PTT button while switching on, connect to `RADIO-XXXX`, open
  `http://192.168.4.1`. An access point opened with the button stays up until the radio is switched off.

### Reset settings

- **With the knob:** hold it pressed for 10 seconds. From the 5th second the ring flashes red — let go if you've
  changed your mind. This resets the Wi-Fi networks, volume, brightness and the other audio and ring settings. **The
  network key, the radio name and the bridge address are kept**, so the radio can be put back on the channel just by
  entering Wi-Fi from a phone.
- **Completely**, including the key and the bridge address — with the `factory` command over USB.

---

## Bridge

### Bridge status page won't open

- **TCP 47080** is closed — in the server's firewall or in the cloud provider's network rules.
- A reply of «нужен ?t=<токен>» (?t=<token> required) means `status_token` is set in `hub.json`; append `?t=` and the
  token itself to the address.
- The radios work without the page, but over-the-air (OTA) updates go through the same port 47080.

### Bridge won't start or keeps restarting

- Linux: `journalctl -u walkie-hub -f`. The service restarts the bridge if it crashes, and a self-check every
  2 minutes restarts it if it stops responding (`journalctl -t walkie-hub` — «мост не ответил на самопроверку —
  перезапуск» (bridge did not respond to the self-check — restarting)).
- Windows: the `hub.log` file next to `hub.json`; the Task Scheduler task is called `WalkieHub`.
- `hub.json` must be valid JSON and must contain `key`.
- Don't start a second copy of the bridge next to the running service: ports 47000 and 47080 must be free.
  A second copy will either fail to start ("Address already in use") or interfere with the first one.

Installing and setting up the bridge — [BRIDGE.md](BRIDGE.md).
