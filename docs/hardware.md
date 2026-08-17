# Hardware

Lixie-style edge-lit acrylic digit panels driven by WS2812B LEDs, on a Particle
Photon 1 (Gen 2) mounted to a custom shield.

## Wiring

| | |
|---|---|
| MCU | Particle Photon 1 (Gen 2, STM32F205) |
| LED data pin | **`D0`** |
| LED type | WS2812B, 800 kHz |
| LEDs per digit panel | **20** (two per numeral, 0–9) |
| Digits | 4 on both clocks; firmware supports 2–6 |
| Total LEDs | 80 |

A 220–470 Ω resistor in series with the data line is recommended, per the original
Make project.

## Digit mapping

Panel 0 is **leftmost**. Lighting numeral `n` on panel `p` drives two LEDs:

```
led = p * 20 + n * 2        and        led + 1
```

Panels fill from the right, so a 4-digit clock shows `hh:mm` and a 6-digit clock shows
`hh:mm:ss` with no per-size special casing. In 12-hour mode the hour's leading zero is
blanked by default (`9:05`, not `09:05`); the minute's is always shown.

This mapping is inherited from the original firmware and is confirmed correct on
hardware — a clock showing the right time in the right order is the check.

## Power

80 LEDs at full white would draw roughly 4.8 A, but the display only ever lights two LEDs
per digit — 8 of 80 on a four-digit clock — so real draw is a small fraction of that. The
default brightness of 60% reduces it further.

## Devices

| Name | Device ID | Notes |
|---|---|---|
| `shop-clock` | `290028001047363333343437` | Plain developer device. All development happened here. |
| `Clock2` | `270049000251353530373132` | Claimed to Particle Product 42326, but flashes normally with `particle flash` |

`shop-clock` sits at about **−71 dBm** RSSI. Workable, but the weak end of usable, and the
first thing to suspect if that clock ever misbehaves on the network. `Clock2` is far
healthier at about **−39 dBm**.

## Platform constraints worth remembering

The Photon 1 is deprecated by Particle and pinned to Device OS **2.3.1**, the terminal
2.x LTS release. It cannot run 3.x or later.

| Resource | Budget | Used (Phase 6) |
|---|---|---|
| Application flash | 128 KB | ~78 KB |
| RAM | ~55 KB usable | ~13.8 KB static, ~26 KB free heap |
| EEPROM | 2047 bytes | 576 |
| Filesystem | none | web UI is a gzipped blob in flash |

Because the firmware is cloud-optional, a future Particle shutdown of Gen 2 cloud services
would cost only OTA flashing. The clocks would keep time, run schedules, serve their config
page and talk to Home Assistant indefinitely.

### If these are ever rebuilt on newer hardware

The Photon 2 is **not** pin-compatible — it uses the Adafruit Feather footprint, is not
5 V tolerant, and drives WS2812 through the SPI peripheral, so the data line must move off
`D0` to the SPI MOSI pin. That is a board respin, not a swap. The firmware itself would
carry over: nothing in it depends on the Gen 2 constraints beyond the storage layer.
