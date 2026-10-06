[Русский](../BRIDGE.md) · **English**

# Your own bridge

The public firmware has **no bridge address set**. The radios won't hear each other until you run
your own bridge and connect them to it. This page explains how to set up a bridge on a cloud server
or on a home computer, connect radios to it and update their firmware over the air.

The bridge is a small Python program (`hub/walkie_hub.py` plus the `hub/wt_proto.py` module next to
it) with no extra libraries. It works the same way on Linux, Windows and macOS.

**Contents**

1. [What the bridge is and which option to choose](#1-what-the-bridge-is-and-which-option-to-choose)
2. [What you need](#2-what-you-need)
3. [Network key and page token](#3-network-key-and-page-token)
4. [Installing on a Linux server](#4-installing-on-a-linux-server)
5. [Installing on Windows](#5-installing-on-windows)
6. [Connecting radios to the bridge](#6-connecting-radios-to-the-bridge)
7. [Multiple bridges (the peers field)](#7-multiple-bridges-the-peers-field)
8. [Status page](#8-status-page)
9. [Over-the-air firmware updates](#9-over-the-air-firmware-updates)
10. [Testing without radios](#10-testing-without-radios)
11. [Security](#11-security)
12. [If the server address changes](#12-if-the-server-address-changes)
13. [Troubleshooting](#13-troubleshooting)

---

## 1. What the bridge is and which option to choose

The radios usually sit on different Wi-Fi networks: at home, in the office, in a warehouse, in
another city. They can't see each other directly. Each radio sends its packets to the bridge, and
the bridge:

- **decides who is on the channel right now.** The first one to press gets the channel; the others
  hear the busy tone. If the button is held longer than 90 seconds (by default), the bridge cuts the
  transmission itself (protection against a stuck button);
- **forwards speech** to all the other radios. Speech is encrypted with the network key. The bridge
  doesn't decrypt it; it forwards the packets as they are;
- **distributes firmware updates.** Put a new file into the bridge's folder, and the radios update
  themselves;
- **shows a status page**: which radios are online, how much battery each has, who is talking, an
  event log.

The radios talk to the bridge over UDP. UDP is a simple way to send short packets over the internet
without setting up a connection; it's the best fit for live audio.

### Three options

**A. Cloud server (VPS) — recommended.** A VPS is a rented computer in a data centre: it's always on
and reachable from the internet at a fixed address. Each radio has the server address entered, and
the radios work from any network: at home, in the office, over a phone hotspot.

```
  Radio "Home"     Radio "Office"   Radio "Warehouse"
       \                |                /
        \___________ internet __________/
                        |
          Bridge on a VPS: radio.example.com
```

**B. A computer on the same network as the radios.** The bridge runs on an ordinary PC at home or in
the office. The «Адрес моста» (Bridge address) field on the radios stays **empty**. A radio with no
address set looks for a bridge on its own network: it broadcasts a hello to every device on the
network on port 47000 and remembers the bridge that answers first. No internet is needed. But the
radios only work on that network, and the computer must always be on and never go to sleep.

**C. Several bridges linked together.** Each location has its own bridge (option B), and the bridges
know about each other through the `peers` field in their settings. A radio in one location hears the
radios in another. The bridges must be able to reach each other over the network: through a VPN (for
example, Tailscale), through port forwarding on the router, or because one of them is on a VPS.
Details are in [section 7](#7-multiple-bridges-the-peers-field).

| | A. VPS | B. PC on the radios' network | C. Multiple bridges |
|---|---|---|---|
| Bridge address in the radio | server address | empty | empty (or the address of its own bridge) |
| Radios on different networks | yes | no | yes, between locations with bridges |
| Works without internet | no | yes | within one location — yes |
| What must be switched on | only the server | the PC | every PC with a bridge |
| Cost | VPS rental | none | none (or a VPS) |

You can combine them: a bridge on a VPS for all radios plus a bridge in the office linked to it via
`peers` (see [section 7](#7-multiple-bridges-the-peers-field)).

---

## 2. What you need

**Python 3.8 or newer.** No other packages are needed; you won't have to run `pip install`. Ubuntu and
Debian already have Python installed (`/usr/bin/python3`). For Windows, download it from python.org.

**Ports.** A port is the number of a "door" on the computer through which a program receives packets.

| Port | Protocol | Purpose | Where it's set |
|---|---|---|---|
| **47000** | UDP | speech and service packets from radios, bridge-to-bridge links | `port` in `hub.json` |
| **47080** | TCP | status page and firmware downloads for radios | `http_port` in `hub.json` |

Both ports must be open for incoming traffic: in the computer's own firewall and (for a VPS) in the
provider's control panel. The radios send their packets from port 47001; you don't need to open it
on the bridge.

**A fixed address.** The radios know the bridge by its address. If the server address changes, the
radios lose it (see [section 12](#12-if-the-server-address-changes)). So you need either a **static
IP address** or a **domain name** (for example, `radio.example.com`): the radio understands both a
numeric address and a name.

**IPv4.** The bridge and the radios work over IPv4 only. Some cheap VPS plans come with IPv6 only:
those won't do.

**Resources.** The smallest VPS plan is enough. Traffic is small too:

- when idle, a radio and the bridge exchange short packets every 2 seconds, which is about 7 MB per
  day per radio;
- while someone is talking, each listening radio receives about 0.65 MB per minute (50 packets per
  second of 187 bytes each, plus network headers).

---

## 3. Network key and page token

Come up with both in advance: you'll need them when setting up the bridge.

### Network key

The network key is a secret shared by all radios and bridges of one network. Every packet is signed
with it and speech is encrypted with it. The bridge and the radios simply drop packets with a
different key.

The bridge can generate a good key for you:

```bash
python3 hub/walkie_hub.py --genkey
```

It prints a string like `XXXX-XXXX-XXXX-XXXX`: 16 random characters without look-alike symbols
(no `0`/`O` and no `1`/`I`).

If you make up a key yourself, follow these rules:

- **at least 12 Latin letters and digits.** Hyphens, spaces and letter case don't matter:
  `k7q2-mx9p…` and `K7Q2MX9P…` are the same key;
- **Latin letters and digits only.** The radio firmware skips Cyrillic letters but the bridge counts
  them, so the keys would no longer match;
- **no longer than 39 characters** including hyphens. The radio stores at most 39 characters and
  silently truncates a longer key.

### Status page token

The token keeps strangers out of the status page: without it the page won't open. You can generate
one like this:

```bash
python3 -c "import secrets; print(secrets.token_urlsafe(16))"
```

Use only Latin letters, digits, `-` and `_`, because the token becomes part of the page address.

---

## 4. Installing on a Linux server

These instructions are for Ubuntu and Debian. Below, `203.0.113.10` is an example server address and
`ubuntu` is an example user name. Substitute your own.

### 4.1. Rent a server

Any VPS provider will do, with the smallest plan running Ubuntu 22.04/24.04 or Debian 12. When
creating it:

- **order a static (reserved) public IPv4 address.** With many providers the regular address is
  "floating" and changes after the server is stopped;
- add your SSH key so you can log in without a password.

### 4.2. Open the ports at the provider

Most providers have their own network filter ("security group", "firewall", "access rules"). It
works separately from the firewall inside the server. Add two inbound rules:

- UDP, port **47000**, source — any (`0.0.0.0/0`);
- TCP, port **47080**, source — any (`0.0.0.0/0`).

Don't delete the SSH rule (TCP 22).

### 4.3. Copy the `hub` folder to the server

Option 1: from your own computer, from the root of this project. The `walkie-hub` folder must not
exist on the server yet:

```bash
scp -r hub ubuntu@203.0.113.10:~/walkie-hub
```

Option 2: on the server itself, with `git clone` of this repository. Then the bridge folder is `hub`
inside the clone.

```bash
git clone <link to this repository> ~/wifi-radio
# from here on, use ~/wifi-radio/hub instead of ~/walkie-hub
```

The folder path must not contain spaces: the installer writes it into the service without quotes.

### 4.4. Fill in `hub.json`

```bash
ssh ubuntu@203.0.113.10
cd ~/walkie-hub
cp hub.example.json hub.json
python3 walkie_hub.py --genkey                                  # network key, if you don't have one yet
python3 -c "import secrets; print(secrets.token_urlsafe(16))"   # page token
nano hub.json
```

Example of a filled-in file:

```json
{
  "name": "Cloud",
  "port": 47000,
  "http_port": 47080,
  "key": "XXXX-XXXX-XXXX-XXXX",
  "peers": [],
  "status_token": "TOKEN",
  "tot_seconds": 90,
  "firmware_dir": "firmware",
  "log_file": "hub.log"
}
```

(The bundled `hub.example.json` uses the Russian name `"Облако"`, which means "Cloud". Any name
within the limits in the table below will do.)

| Field | Default | Meaning |
|---|---|---|
| `name` | `Мост` ("Bridge") | Bridge name. It's shown on the radio's page and on the status page. At most 31 bytes: that's about 15 Cyrillic or 31 Latin letters. **Every bridge must have its own name**: the bridge number is derived from the name, and bridges with the same name can't see each other. |
| `port` | `47000` | UDP port for the radios. For option B leave it at 47000: radios search for a bridge on the network only on this port. If you change it, enter the address with the port in the radio: `radio.example.com:47100`. |
| `http_port` | `47080` | TCP port of the status page and firmware downloads. The radios learn it from the bridge automatically. |
| `key` | — | **Required.** The network key from [section 3](#network-key). Replace the placeholder `ВСТАВЬТЕ-КЛЮЧ-СЕТИ` ("INSERT-NETWORK-KEY") from the example: with it the bridge starts, but the radios won't connect to it. If the key is shorter than 12 characters, the bridge won't start. |
| `status_token` | empty | Status page token. If empty, the page is open to anyone who knows the address. |
| `tot_seconds` | `90` | How many seconds one radio may hold the channel. After that the bridge cuts the transmission. The radio itself cuts the transmission after 100 seconds, so there's no point setting more than 100. |
| `peers` | `[]` | Other bridges to link with: `["address", "address:port"]`. See [section 7](#7-multiple-bridges-the-peers-field). |
| `firmware_dir` | `firmware` | Folder for firmware updates (`walkie.bin` + `walkie.json`). The path is relative to the folder containing `hub.json`. |
| `log_file` | `hub.log` | Log file next to `hub.json`. When the file reaches 1 MB a new one is started, and 3 old ones are kept (`hub.log.1`…`hub.log.3`). An empty string `""` means don't write the log to a file. |
| `listen` | `0.0.0.0` | Which of the computer's addresses to listen on. The example doesn't include this field, and you don't need to touch it. `0.0.0.0` means "on all addresses". The self-check connects to `127.0.0.1` and won't work with any other value. |

The file must be valid JSON: strings in double quotes, commas between fields, no comma after the last
field, no comments.

### 4.5. Run the installer

```bash
cd ~/walkie-hub
sudo bash install_linux.sh
```

What `install_linux.sh` does:

1. Checks that `hub.json` is present next to it. If not, it stops with a hint.
2. Creates the systemd service **`walkie-hub`**. systemd is the Linux program manager that starts
   programs when the server boots. The service runs as your user (not as root), starts
   `python3 walkie_hub.py --config hub.json`, comes back up after a server reboot and restarts after
   3 seconds if the program crashes.
3. Creates the **self-check** `walkie-hub-check.timer`: 3 minutes after boot, and then every
   2 minutes, `healthcheck.py` sends the bridge a service packet. If the bridge doesn't answer (for
   example, it has hung), the service is restarted and the system log gets the message «мост не
   ответил на самопроверку — перезапуск» ("bridge did not answer the self-check — restarting").
4. If the `ufw` firewall is **enabled** on the server, opens `47000/udp` and `47080/tcp` in it. If
   `ufw` is disabled, it changes nothing and does not enable it.
5. Starts (or restarts) the bridge and shows the first lines of its status.

You can run the script again, for example after updating the bridge files: it rewrites the services
and restarts the bridge.

> If you enable `ufw` yourself, allow SSH first (`sudo ufw allow OpenSSH`), or you'll lose access to
> the server. If you changed the ports in `hub.json`, open the new ports both in `ufw` and at the
> provider: the installer only opens 47000 and 47080.

### 4.6. Check

On the server:

```bash
systemctl status walkie-hub             # should be active (running)
python3 healthcheck.py --config hub.json  # «мост отвечает» ("bridge is responding")
tail -n 20 hub.log                      # «мост «Cloud» запущен: UDP 47000, страница :47080, …» ("bridge 'Cloud' started: UDP 47000, page :47080, …")
```

From your own computer, open `http://203.0.113.10:47080/?t=TOKEN` in a browser. You should see the
page «Мост раций «Cloud»» ("Radio bridge 'Cloud'") with the text «Ни одна рация сюда ещё не
подключалась» ("No radio has connected here yet"). Without `?t=…` the bridge replies «нужен
?t=<токен>» ("?t=<token> required").

You can check that the UDP port is open from the outside with the software radio, right from your own
computer (see [section 10](#10-testing-without-radios)):

```bash
python3 hub/sim_radio.py --hub 203.0.113.10 --key XXXX-XXXX-XXXX-XXXX status
```

### 4.7. Managing the bridge

```bash
sudo systemctl restart walkie-hub        # restart, e.g. after editing hub.json
sudo systemctl stop walkie-hub           # stop
systemctl status walkie-hub              # status
journalctl -u walkie-hub -f              # live service log (exit with Ctrl+C)
tail -f ~/walkie-hub/hub.log             # the same log from the file
journalctl -t walkie-hub                 # restarts triggered by the self-check
systemctl list-timers walkie-hub-check.timer
sudo systemctl disable --now walkie-hub walkie-hub-check.timer   # remove from autostart
```

The bridge reads `hub.json` only at startup, so every edit needs a `restart`.

---

## 5. Installing on Windows

Suitable for option B (a PC on the radios' network) and option C (bridges in different locations).

1. Install Python 3 from python.org. In the installer, tick "Add python.exe to PATH" and leave
   "py launcher" enabled.
2. Copy the `hub` folder to a location with no spaces in the path, for example `C:\walkie-hub`.
3. Copy `hub.example.json` to `hub.json` and fill it in as in [section 4.4](#44-fill-in-hubjson).
   Notepad is fine; save it as UTF-8. Generate the key like this:
   ```powershell
   py -3 C:\walkie-hub\walkie_hub.py --genkey
   ```
4. Open **PowerShell as administrator** and run the installer:
   ```powershell
   powershell -ExecutionPolicy Bypass -File C:\walkie-hub\install_windows.ps1
   ```

What `install_windows.ps1` does:

- finds `pythonw.exe` (Python without the black console window) via PATH or via `py -3`;
- creates the Windows Firewall rules «Мост раций UDP 47000» and «Мост раций TCP 47080» ("Radio
  bridge UDP 47000" / "Radio bridge TCP 47080") for all network types (domain, private, public).
  Old rules with the same names are removed first;
- registers the Task Scheduler task **`WalkieHub`**: it starts when the computer boots (even before
  anyone logs in, as SYSTEM), without a window, with no run-time limit, including on laptop battery
  power. If the program exits with an error, Task Scheduler restarts it once a minute (up to 999
  times);
- starts the task and, 3 seconds later, checks that the process is running. If all is well, it prints
  «Мост запущен (процесс …). Страница: http://localhost:47080/» ("Bridge started (process …). Page:
  http://localhost:47080/"). If not, it suggests looking in `hub.log`.

Windows has no self-check every 2 minutes like Linux does. To check the bridge manually:
`py -3 C:\walkie-hub\healthcheck.py --config C:\walkie-hub\hub.json`.

Management (PowerShell as administrator):

```powershell
Stop-ScheduledTask  -TaskName WalkieHub        # stop
Start-ScheduledTask -TaskName WalkieHub        # start (after editing hub.json: Stop first)
Get-ScheduledTask   -TaskName WalkieHub | Get-ScheduledTaskInfo
Get-Content C:\walkie-hub\hub.log -Tail 20 -Wait   # live log
```

Status page on the same computer: `http://localhost:47080/?t=TOKEN`. From other devices on the
network, use the computer's address.

To keep the bridge always available, turn off sleep on the computer: "Settings → System → Power".

To remove the bridge (the firewall rule names are in Russian because that's what the installer
creates):

```powershell
Unregister-ScheduledTask -TaskName WalkieHub -Confirm:$false
Remove-NetFirewallRule -DisplayName "Мост раций UDP 47000", "Мост раций TCP 47080"
```

### Running manually (macOS, Linux, Windows)

To try the bridge without installing it, run it in a terminal window. The log appears right on the
screen; stop it with Ctrl+C:

```bash
cd hub
cp hub.example.json hub.json     # and enter the key
python3 walkie_hub.py --config hub.json -v     # -v — verbose log
```

On Windows, type `py -3` instead of `python3`, and `copy` instead of `cp`.

On macOS, the first time you run it the system may ask whether to allow Python to accept incoming
connections: allow it.

---

## 6. Connecting radios to the bridge

Each radio needs two values:

- **bridge address** — an IP or a name: `203.0.113.10` or `radio.example.com`. You can omit the port
  if it's the standard one (47000). If it's non-standard, write `radio.example.com:47100`. For
  option B leave the field empty;
- **network key** — the same one as in `hub.json`.

### Via the radio's page (from a phone)

1. Open the radio settings page. A radio with no network key opens the `RADIO-XXXX` access point by
   itself when switched on, even if its Wi-Fi is already set up. Connect to it with your phone, and
   the page opens by itself. If the radio is already on your Wi-Fi network, the page is at
   `http://radio-xxxx.local` (`xxxx` is the same 4 characters as in the access point name, only in
   lower case). You can also open the access point manually: hold the PTT (push-to-talk) button
   and switch the radio on. More in [Flashing and setup](SETUP.md#2-first-time-setup).
2. In the «Настройки» (Settings) section, fill in the **«Адрес моста»** (Bridge address) and
   **«Ключ сети»** (Network key) fields and press **«Сохранить»** (Save). You don't need to restart
   the radio: it immediately starts looking for the new bridge.
3. If the key is shorter than 12 letters and digits, the radio replies «Сохранено, но ключ не принят:
   нужно не меньше 12 букв и цифр» ("Saved, but the key was not accepted: at least 12 letters and
   digits are required").

The key is not shown on the page. Next to the field it only says «задан» (set) or «не задан» (not
set). Leave the field empty if you don't need to change the key.

### Over a USB cable (macOS / Linux)

```bash
python3 tools/provision.py --name Warehouse --server radio.example.com --key XXXX-XXXX-XXXX-XXXX
python3 tools/provision.py --show          # check what was written
python3 tools/provision.py --server auto   # go back to searching for a bridge on the local network (option B)
```

On macOS the script finds the board's port by itself; if several boards are connected, specify
`--port /dev/cu.usbmodem…`. On Linux always specify the port: `--port /dev/ttyACM0` (or
`/dev/ttyUSB0`). The script doesn't work on Windows. Connect to the board with any serial monitor at
115200 baud (for example, the Serial Monitor in the Arduino IDE with the line ending set to "New
Line") and type the commands (the full list is in [Flashing and setup](SETUP.md#5-usb-commands)):

```
server radio.example.com
key XXXX-XXXX-XXXX-XXXX
show
```

### How to tell the radio is connected

- The radio plays its "connected" sound, and the ring briefly flashes **green** every 3 seconds (on
  USB power, instead of flashing, the ring glows dim green steadily).
- On the bridge status page, the radio appears in the «Рации на этом мосту» (Radios on this bridge)
  table as «в сети» (online). Next to it you can see its name, battery, Wi-Fi strength, latency to the
  bridge and firmware version.

What the ring means when there's no connection:

| Ring | What's happening |
|---|---|
| two orange LEDs blinking | Wi-Fi is up, no bridge: wrong address, UDP port closed, bridge not running |
| red and purple alternating | key not set or doesn't match the bridge's key |
| amber flash instead of green | connected to the bridge, but one of the bridges in `peers` is unreachable |
| a running blue LED | the radio is searching for Wi-Fi |

### Your bridge address built into the firmware

If you have many radios, you can put the address into your own firmware build, in
`firmware/walkie/config.h`:

```c
#define DEFAULT_SERVER "radio.example.com"
```

Freshly flashed radios will know the bridge straight away; you'll only need to enter the key. An
address saved in the radio's memory takes priority over this value. The key is never built into the
firmware: it is stored only in the radio's memory.

---

## 7. Multiple bridges (the peers field)

Bridges linked through `peers` exchange service packets every 2 seconds. A bridge forwards speech
from its own radios to all linked bridges, and speech from a linked bridge only to its own radios.

Rules:

- **all bridges use the same network key**, but **different names (`name`)**;
- in `peers` you write the address of the other bridge: an IP or a name, with the port after a colon
  if it isn't 47000. The bridge re-resolves names to IP addresses every 5 minutes;
- **a link from one side is enough.** If bridge A knows bridge B's address but B doesn't know about
  A, B will still accept A, since they share the key. A then shows up at B as an unknown bridge and is
  forgotten after a minute of silence. So a bridge behind a router (at home, in the office) can be
  linked to a bridge on a VPS without opening any ports on the router: put the VPS address into the
  home bridge's `peers`, and the VPS bridge needs nothing;
- **a bridge doesn't relay other bridges' speech any further.** With three or more bridges, each one
  must be linked to every other. For example, two bridges behind routers that are linked only to the
  VPS won't hear each other: they also need a direct link between themselves (a VPN or port
  forwarding);
- if the button is pressed at the same moment in two locations, the same radio gets the channel on
  all bridges (the one with the lower number). The other hears the busy tone;
- while at least one bridge from the list is unreachable, the radios show an amber flash instead of a
  green one, and on the status page that bridge is marked «нет связи» (no connection).

Example: a Windows PC with a bridge at home and another in the office, radios with an empty bridge
address, the PCs linked through Tailscale. Tailscale is a VPN with a free plan for personal use: it
gives every computer a permanent address in your private network. The other computer's address is
shown in the Tailscale app.

`hub.json` at home:

```json
{
  "name": "Home",
  "key": "XXXX-XXXX-XXXX-XXXX",
  "peers": ["203.0.113.22"],
  "status_token": "TOKEN"
}
```

`hub.json` in the office:

```json
{
  "name": "Office",
  "key": "XXXX-XXXX-XXXX-XXXX",
  "peers": ["203.0.113.21"],
  "status_token": "TOKEN"
}
```

Here `203.0.113.21` (home) and `203.0.113.22` (office) are example Tailscale addresses of the PCs.
One side would be enough, but both sides is more reliable. The remaining fields can be left out: the
defaults from the table in [section 4.4](#44-fill-in-hubjson) are used.
If the VPN routes traffic through a relay server in another country, latency goes up and speech will
arrive with a delay.

Important for options B and C:

- a radio looks for a bridge only on **its own** subnet and only on port 47000. The computer with the
  bridge must be connected to the same router. Guest networks with "client isolation" often block
  this search;
- keep **one** bridge per network: a radio connects to whichever answers first.

---

## 8. Status page

Address: `http://ADDRESS:47080/?t=TOKEN`, for example `http://radio.example.com:47080/?t=TOKEN`.
The page refreshes itself every 5 seconds. It shows:

- the bridge name, its uptime and which firmware it has available for distribution;
- the channel: «эфир свободен» ("channel is free") or «сейчас говорит …» ("now talking: …"), with
  where and for how many seconds;
- **radios on this bridge**: name, number, address, «в сети» (online) / «говорит» (talking) / «нет
  связи» (no connection), battery (or «от сети» (on external power) when powered from USB), Wi-Fi
  strength in dBm, latency to the bridge, firmware version, «звук выключен» (sound off);
- **other bridges** (if any): connected or not, latency, which radios are on them;
- **log**: the last 60 events. For example: «в сети: Warehouse (…)» ("online: Warehouse (…)"),
  «эфир: говорит Office» ("channel: Office is talking"), «🔋 Warehouse: аккумулятор 12% — пора на
  зарядку» ("🔋 Warehouse: battery 12% — time to charge", at 15 % and below), «⚠ Warehouse: в
  передаче одна тишина — проверьте микрофон» ("⚠ Warehouse: only silence in the transmission — check
  the microphone"), «⚠ пакет раций с неверной подписью от … — не тот ключ сети?» ("⚠ radio packet
  with an invalid signature from … — wrong network key?").

The same data in machine-readable form, for example for your own monitoring:
`http://ADDRESS:47080/status.json?t=TOKEN`.

A radio is considered gone if no packets have arrived from it for 7 seconds. The radio itself sends a
hello every 2 seconds and decides the bridge is gone if it gets no reply for 6.5 seconds.

---

## 9. Over-the-air firmware updates

Radios that already work with the bridge update themselves: no need to open them up or plug in a
cable. The very first firmware on a new radio is flashed over USB (see
[Flashing and setup](SETUP.md#1-flashing)).

### How it works

- At startup, and then every 10 seconds, the bridge looks into the `firmware_dir` folder. It should
  contain `walkie.bin` (the firmware itself) and `walkie.json` with the version number:
  `{"version": 10}`.
- In every reply to a radio, the bridge reports the version number, the file size and its SHA-256
  checksum. This is the file's "fingerprint": if even one byte changes, the fingerprint is different.
- A radio downloads the update only if all of these conditions are met:
  - the version number on the bridge is **higher** than its own (`FW_VERSION` in
    `firmware/walkie/config.h`);
  - it hasn't already installed this same build;
  - for **20 seconds** nobody has pressed its button and nobody has been talking on the channel.
- The radio downloads `http://<bridge IP>:47080/fw/<first 16 characters of SHA-256>.bin`, checks the
  size and the fingerprint, writes the firmware and restarts. While downloading, the ring shows a
  growing blue bar. The bridge log gets the entry «рация … скачивает прошивку 10» ("radio … is
  downloading firmware 10").
- If anything doesn't match, the radio stays on the old firmware and tries again in an hour. You don't
  have to wait the hour: switch the radio off and on.
- **You can't roll back to an older version over the air**: the radio won't install a number lower
  than or equal to its own. To bring back old code, build it with a higher number.

### Step by step

1. Increase the number in `firmware/walkie/config.h`, for example `#define FW_VERSION 10`.
2. Build the firmware (requires `arduino-cli`):
   ```bash
   tools/build_firmware.sh
   ```
   `firmware/dist/` will then contain `walkie.bin` and `walkie.json` for the bridge, plus
   `walkie-full-v10.bin` for the first flashing over USB. **The bridge needs `walkie.bin`
   specifically**; the full `walkie-full-…` image won't install over the air.
3. Copy the files to the bridge **in this order: first `walkie.json`, then `walkie.bin` under a
   temporary name, and finally rename it**:
   ```bash
   ssh ubuntu@203.0.113.10 'mkdir -p ~/walkie-hub/firmware'
   scp firmware/dist/walkie.json ubuntu@203.0.113.10:~/walkie-hub/firmware/walkie.json
   scp firmware/dist/walkie.bin  ubuntu@203.0.113.10:~/walkie-hub/firmware/walkie.bin.new
   ssh ubuntu@203.0.113.10 'mv ~/walkie-hub/firmware/walkie.bin.new ~/walkie-hub/firmware/walkie.bin'
   ```
   On a Windows bridge, the same in PowerShell:
   ```powershell
   New-Item -ItemType Directory -Force C:\walkie-hub\firmware | Out-Null
   Copy-Item firmware\dist\walkie.json C:\walkie-hub\firmware\walkie.json
   Copy-Item firmware\dist\walkie.bin  C:\walkie-hub\firmware\walkie.bin.new
   Move-Item -Force C:\walkie-hub\firmware\walkie.bin.new C:\walkie-hub\firmware\walkie.bin
   ```
4. Within 10 seconds the bridge log shows «прошивка для раций: версия 10, … КБ» ("firmware for
   radios: version 10, … KB"), and the status page shows «прошивка для раций: версия 10» ("firmware
   for radios: version 10"). Each radio starts updating as soon as there have been 20 seconds with no
   button presses and no talking on the channel. After the restart, the new number appears in the
   «Прошивка» (Firmware) column.

**Why the order matters.** The bridge re-reads both parts only when the `walkie.bin` file changes.
If you put the new `walkie.bin` in place before `walkie.json`, the bridge may manage to read the new
file with the old version number, and the radios won't update. If that happens, restart the bridge.
The temporary name is there so the bridge doesn't read a half-copied file: renaming is instant.
Before every download, the bridge checks the file against the announced fingerprint. If the file was
replaced after the bridge read it, the radio gets an error and tries again in an hour (or right after
being switched off and on), and the bridge re-reads the file.

If linked bridges have radios of their own, put the update into the folder of **every** bridge: a
radio downloads firmware only from its own bridge.

The ready-made firmware from this repository (`firmware/dist/walkie.bin` + `walkie.json`) can also be
distributed this way: radios with the same or a higher number will ignore it.

---

## 10. Testing without radios

### Software radio `sim_radio.py`

Pretends to be a radio: it connects to the bridge from a computer, transmits and listens to the
channel. It needs nothing but Python and runs from any computer:

```bash
cd hub
python3 sim_radio.py --hub radio.example.com --key XXXX-XXXX-XXXX-XXXX status
```

The reply looks like this (the numbers are just an example):

```
мост «Cloud» ответил за 31 мс: раций в сети 3, мостов 0/0, прошивка для раций v10
```

(That is: "bridge 'Cloud' replied in 31 ms: radios online 3, bridges 0/0, firmware for radios v10".)

| Command | What it does |
|---|---|
| `status` | connect and show the bridge's reply: latency, how many radios are online, how many of the listed bridges are connected, the firmware version available for distribution |
| `tone --seconds 3 [--hz 800]` | "press the button" and transmit an 800 Hz tone for 3 seconds: all radios will hear it |
| `say file.wav` | transmit a recording: WAV 16 kHz, mono, 16-bit (`ffmpeg -i in.mp3 -ar 16000 -ac 1 out.wav`) |
| `listen --seconds 60 --out efir.wav` | listen to the channel for 60 seconds, record everything to WAV and show packet loss and latency jitter for each transmission |
| `echo --minutes 30 [--save folder]` | "parrot": say something into a radio and release the button, and what you said comes back on the channel. This tests the microphone, the speaker and the whole path through the bridge. With `--save`, every transmission is saved to WAV |

Common options: `--hub address[:port]`, `--key`, `--name` (the name on the bridge page, default
`Компьютер` ("Computer")), `--id` (number in hexadecimal). If you run two software radios at the
same time, for example `listen` in one window and `tone` in another, give them different numbers:
`--id A1` and `--id A2`. Otherwise the bridge will take them for one radio.

The software radio shows up on the status page like a normal radio. 7 seconds after it exits it is
marked «нет связи» (no connection), but the row stays in the table until the bridge restarts (just
like with real radios).

What the errors mean:

- «мост … не ответил за 5 с (адрес, порт UDP, брандмауэр, ключ?)» ("bridge … did not answer within
  5 s (address, UDP port, firewall, key?)") — packets aren't getting through. Check the address, the
  service on the server, and the UDP 47000 rule at the provider and in the firewall;
- «⚠ мост ответил: подпись не сошлась — не тот ключ сети» ("⚠ bridge replied: signature mismatch —
  wrong network key") — the bridge is reachable but the key is different. Right after that the
  program also prints «не ответил за 5 с» ("did not answer within 5 s"): the cause is the same, look
  for it in the key.

### Self-check `healthcheck.py`

Sends the bridge a service packet and waits up to 3 seconds for a reply. Prints «мост отвечает»
("bridge is responding", exit code 0) or «мост НЕ отвечает» ("bridge is NOT responding", exit
code 1). It doesn't appear in the radio list. It takes the key and the port from `hub.json`:

```bash
python3 hub/healthcheck.py --config hub/hub.json                        # bridge on this computer
python3 hub/healthcheck.py --config hub/hub.json --host 203.0.113.10    # remote bridge
```

---

## 11. Security

**What is protected.**

- Every packet is signed with the network key (HMAC-SHA256). The bridge and the radios drop any
  packet without a valid signature. Without the key, a stranger's radio or a scanner from the
  internet can't get onto the channel.
- Speech is encrypted (AES-128) with a separate key derived from the network key. The bridge doesn't
  decrypt speech. Someone who intercepts the packets on the way can't hear it without the key.

**What is not protected.**

- **Whoever knows the key is in the network**: they can connect their own radio, talk and hear
  everything. The key is stored unencrypted in the `hub.json` of every bridge and in the memory of
  every radio. If you lose a radio or the key gets into the wrong hands, change the key.
- Radio names, battery level, firmware version and service packets are signed but **not
  encrypted**.
- The status page uses plain HTTP without encryption, and the token is sent in the address as plain
  text. This protects against casual visitors, not against someone intercepting your traffic. Don't
  reuse a password from anything else as the token.
- The radio settings page has no password: anyone on the same Wi-Fi network can open it, change the
  settings and upload different firmware to the radio. It does not reveal the network key, though.
- The bridge serves the firmware file for radios (`/fw/…` on port 47080) without a token. Don't put
  anything secret into your firmware build. The network key never ends up in the firmware.

**Rules.**

- Don't publish `hub.json`, a link with your token, or screenshots showing the key. The files
  `hub/hub.json`, `hub/hub.log*` and `hub/firmware/` are already listed in `.gitignore`, so git won't
  push them to GitHub.
- Set `status_token`: without it, anyone who knows the address sees the radio names and their IP
  addresses.
- Log in to the server with an SSH key and keep the system updated (`sudo apt update && sudo apt
  upgrade`).
- **Changing the key:** generate a new one, put it into the `hub.json` of every bridge and restart
  them. Then enter the new key into every radio: through the page or with `provision.py --key`. Until
  the key is updated, the radio shows red and purple, and the bridge log shows «пакет раций с неверной
  подписью» ("radio packet with an invalid signature"). A settings reset with the volume knob button
  (hold for 10 seconds) does not erase the key, the name or the bridge address. Only a full reset with
  the `factory` USB command erases them.

---

## 12. If the server address changes

A radio knows the bridge by its address. If the server's IP has changed and the radios still have the
old IP:

- the radios lose the bridge: two orange LEDs, you can't talk;
- **there's no way to tell them the new address through the bridge**: both updates and the connection
  itself go through it. You'll have to enter the new address into every radio by hand (the radio's
  page or `provision.py --server`).

How to avoid this:

1. **Pin the address** at the provider: a "static" or "reserved" IP. Do this before you hand out the
   radios. Sometimes there's a small extra charge for it.
2. **Even more reliable — a domain name.** Create an A record at your domain registrar (for example,
   `radio.example.com` → `203.0.113.10`) and enter the name into the radios, not the digits. The radio
   looks up the IP for the name again when the settings are saved, every 15 seconds while there's no
   bridge, and every 10 minutes while connected. If the address changes, you only need to update the
   record at the registrar: the radios will find the bridge on their own as soon as DNS updates.
   Bridges that have names in `peers` refresh the addresses the same way, every 5 minutes. If you're
   planning a move, lower the record's time to live (TTL) to 5 minutes in advance.

**Moving to another server.** Move `hub.json` unchanged (same key and same name) along with the
`firmware` folder. Install the bridge on the new server and change the address in the domain's A
record. You won't have to set up the radios again.

---

## 13. Troubleshooting

Bridge startup errors are visible in `journalctl -u walkie-hub`. On Windows the bridge runs without a
window, so to find the error, stop the task (`Stop-ScheduledTask -TaskName WalkieHub`) and run the
bridge manually: `py -3 C:\walkie-hub\walkie_hub.py --config C:\walkie-hub\hub.json`.

| Symptom | What to check |
|---|---|
| `sim_radio.py … status`: «не ответил за 5 с» ("did not answer within 5 s") | Is the service running (`systemctl status walkie-hub`); is UDP 47000 open at the provider and in `ufw` (Windows: firewall rules); is the address right; does the server have IPv4 |
| «подпись не сошлась — не тот ключ сети» ("signature mismatch — wrong network key") | The key in the radio or in `--key` doesn't match `hub.json`. Check for extra characters and Cyrillic letters, and that it's no longer than 39 characters |
| Bridge won't start: `ключ сети слишком короткий` ("network key too short") | `key` has fewer than 12 letters and digits |
| Bridge won't start: `JSONDecodeError` | Error in `hub.json`: an extra or missing comma, quotes |
| Bridge won't start: `Address already in use` | The port is taken: the bridge is already running (the service and a manual run at the same time) or another program is using the port |
| The page won't open | Is TCP 47080 open at the provider and in the firewall; did you forget `?t=TOKEN` (without it you get «нужен ?t=<токен>», "?t=<token> required") |
| Radio: two orange LEDs | Bridge address in the radio, port, firewall. For option B — the bridge computer on the same subnet, `port` 47000, one network without client isolation |
| Radio: red and purple | Key not set or doesn't match |
| Radio: amber flash | One of the bridges in `peers` is unreachable. Look at the «Другие мосты» (Other bridges) section on the status page |
| Bridges can't see each other | Same `name` on two bridges; different keys; the address in `peers`; UDP 47000 between them |
| Radios don't update | The number in `walkie.json` must be higher than the radios'; is TCP 47080 open (the radio downloads over it); is it `walkie.bin` (not `walkie-full-…`); were the files copied in the right order (otherwise restart the bridge); the radio was left untouched for 20 seconds; after a failure the radio waits an hour (or switch it off and on) |
| Log says «в передаче одна тишина» ("only silence in the transmission") | The radio's microphone has most likely come loose or is wired incorrectly |

How packets, signatures and encryption work is covered in the [protocol description](protocol.md).
