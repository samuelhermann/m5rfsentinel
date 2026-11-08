# M5 RF Sentinel EU

A small standalone surveillance-gear detector for the **M5StickC / M5StickC Plus**. No phone needed.
It listens to BLE adverts and WiFi frames (receive only) and lights up when a device's address
starts with a prefix from a watchlist: police and enforcement gear, body cameras, plate readers,
Flock Safety cameras, Ring doorbells and drones, with a European focus (including Belgium).

> **Vibe coded, adapted from two other projects.**
> This firmware was written with an AI assistant (Claude Code) and adapted from
> [RF Sentinel](https://github.com/CIS-C0/RFSentinel) (CIS-C0, GPL-3.0), whose European
> prefix presets, scoring and detection ideas it follows, and
> [OUI-SPY Unified](https://github.com/colonelpanichacks/oui-spy-unified-blue)
> (colonelpanichacks, MIT), whose Flock Safety, Ring, DJI, Parrot and Skydio prefix lists it uses.
> It has had little real-world testing and has not been audited. Expect false positives and
> misses, and treat a match as a hint, not proof. Credit for the underlying research belongs to
> those projects and the people they credit. See [Credits](#credits).

## What it does

The ESP32 listens to BLE adverts and WiFi frames (channels 1-13, receive only) and flags a device
when its address starts with a watchlist prefix. The list holds 88 prefixes, built from:

- the UK, France, Germany, Italy, Netherlands, Portugal, Spain and Sweden presets of RF Sentinel
- Belgium (`presets/belgium.json`): makers of gear Belgian sources document (Macq, Gatso, Jenoptik, Sepura, Airbus, Motorola Solutions), all low-confidence candidates
- the Flock Safety entries of RF Sentinel's global preset
- Ring, DJI, Parrot and Skydio (`presets/camera_drone.json`), each prefix checked against the IEEE registry

Like RF Sentinel, it ignores randomized addresses (WiFi locally-administered bit, BLE random type).
A MAC prefix only tells you the manufacturer, not where a device is used, so every match is a guess.
BLE and WiFi share the one ESP32 radio, so it alternates: 1.5 s of BLE, then one sweep of
WiFi channels 1-13 (110 ms each). Run together, BLE starves WiFi down to a frame every 10 s.

## Screen

| Idle | Alert |
|---|---|
| Bright (90%), breathing green `SCANNING`, packet counters, battery | Full brightness, strobes then holds a solid colour for 8 s after the last sighting |

| Colour | Meaning |
|---|---|
| Red | Body camera (Axon, Zepcam) |
| Orange | Plate reader or enforcement camera (Flock Safety, Jenoptik, Macq, Gatso, Selea, Elsag, Idemia) |
| Blue | Police radio and in-car gear (Motorola, Sepura, Airbus, Teltronic, Leonardo) |
| Purple | Doorbell and home security camera (Ring) |
| Teal | Drone or its controller (DJI, Skydio) |

The two numbers next to `BLE` and `WiFi` on the idle screen are running totals of every packet
heard since boot, not matches.

Score 70 and up (RF Sentinel's probable/strong tier) strobes for 3 s with three beeps and the red
LED on; weaker matches strobe 1.2 s with one beep and read `POSSIBLE`. The alert shows the
address, band (BLE or WIFI), signal in dBm and a signal bar. The StickC Plus has a buzzer;
the original StickC is silent.

Matches scored under 40 never alert. They are the chip makers (Liteon, Espressif, Silicon Labs...)
that the community list saw inside Flock cameras, which also sit in many unrelated devices, and
Parrot, whose blocks also cover car kits. They only appear in the hit list (marked `weak`) and are
logged once on serial. A weak entry is the first to leave a full list and never pushes out a strong one.
Flock Safety's own block (`B4:1E:52`, score 75) alerts like Axon.

## Buttons

| Button | Action |
|---|---|
| A (front), click | During an alert: dismiss it for 60 s. Idle: step through the last 8 devices heard (vendor, address, band, dBm, times heard, age). Past the last one it returns to idle; it also returns on its own after 15 s |
| A (front), hold | Scan mode: BLE + WiFi, then BLE only (WiFi sniffing off, BLE gets the whole radio), then WiFi only. The title shows `BLE` or `WIFI` when not on both |
| B (side) | Mute / unmute the buzzer |
| PWR (left), click | Stealth: screen off, but the red LED and buzzer still alert. Click again to wake |

## Build and flash

```sh
pip install platformio
pio run -t upload          # add --upload-port /dev/cu.usbserial-XXXX if there are several
pio device monitor         # 115200; every hit is printed, send `t` for a test alert
```

The original StickC's FTDI bridge only works at 115200 baud, so flashing takes about a minute.
The app uses about 93% of the flash, so there is little room left for more features.

## Updating the list

`src/oui_eu.h` is generated. After editing a preset, run `python3 tools/gen_oui.py` from the
repository root and rebuild. The generator merges the presets (a prefix in several countries is kept
once with its highest score) and checks every prefix in `presets/belgium.json` and
`presets/camera_drone.json` against the IEEE registry in `data/oui.tsv`, so a typo can't flag the
wrong maker.

- `presets/app/` holds copies of RF Sentinel's country presets and its global preset (only the Flock entries are used).
- `data/oui.tsv` is the IEEE vendor table bundled with RF Sentinel.

## Credits

- **[RF Sentinel](https://github.com/CIS-C0/RFSentinel)** by CIS-C0, GPL-3.0. The country presets in
  `presets/app/`, the IEEE table in `data/oui.tsv`, the scoring tiers and the matching approach come
  from it. This repository is therefore also released under the **GPL-3.0** (see `LICENSE`).
- **[OUI-SPY Unified](https://github.com/colonelpanichacks/oui-spy-unified-blue)** by colonelpanichacks,
  MIT, Copyright (c) 2026 colonelpanichacks. The Ring, DJI, Parrot and Skydio prefix lists in
  `presets/camera_drone.json` follow its OUI Database, and the Flock Safety prefix research it
  collects (community list by @NitekryDPaul and DeFlockJoplin, plus the firmware-derived `B4:1E:52`)
  is the basis of the Flock entries. The MIT notice applies to that material.
- Built on [M5Unified](https://github.com/m5stack/M5Unified) and
  [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino).

## Disclaimer

Receive only: it transmits nothing. It is for awareness and research. Laws on monitoring radio
signals differ by country, so check yours. It is provided as is, with no warranty, and it can be wrong.
No affiliation with RF Sentinel, OUI-SPY, Flock Safety, Ring, DJI or any vendor named here.
