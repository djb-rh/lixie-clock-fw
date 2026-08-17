# Deploying to a clock

## 1. Build

```bash
cd ~/Developer/lixie-clock-fw
python3 tools/build_web_assets.py
particle compile photon --target 2.3.1 --saveTo /tmp/lixie.bin
```

`build_web_assets.py` regenerates `src/web_assets.h` from `web/index.html`. It is **not**
committed, so this step is required after any checkout and after any page edit.

## 2. Flash

```bash
particle flash shop-clock /tmp/lixie.bin
```

### If the device is claimed to a Particle Product

`Clock2` belongs to Product 42326. Product-claimed devices reject a direct developer flash
unless they are marked as a **development device**. If the command above is refused:

- **Particle Console** → Products → 42326 → Devices → `Clock2` → mark as a development
  device, then retry; **or**
- flash through the product:
  ```bash
  particle flash --product 42326 270049000251353530373132 /tmp/lixie.bin
  ```

This has not been exercised — `shop-clock` is a plain developer device and everything so
far was deployed to it. Expect one of the two paths to need a moment's fiddling.

### Verifying the flash actually landed

**Do not** poll an endpoint until it merely answers — the *outgoing* firmware answers too,
and you will read stale values and conclude your change did not work. This mistake was
made twice during development. Wait for the build stamp to *change*:

```bash
OLD=$(curl -s http://<ip>/api/state | grep -o 'build":"[^"]*')
particle flash <device> /tmp/lixie.bin
until NOW=$(curl -s -m 5 http://<ip>/api/state 2>/dev/null | grep -o 'build":"[^"]*'); \
      [ -n "$NOW" ] && [ "$NOW" != "$OLD" ]; do sleep 4; done; echo "$NOW"
```

## 3. Find the new clock on the network

A freshly flashed clock keeps its Wi-Fi credentials (those live in Device OS, not in this
firmware) but starts with **default settings**. To find its address:

```bash
particle get <device> ip
```

There is no mDNS — see `docs/phase6-results.md` for why. The IP is also reported as a Home
Assistant sensor once MQTT is configured.

## 4. Configure

Open `http://<ip>/` and set:

| Section | What to set |
|---|---|
| Time & Location | Timezone, latitude and longitude. The page explains how to get coordinates from a phone. |
| Display | Colour, brightness, effect, digit count, 12/24-hour |
| Home Assistant | Broker **IP** (`10.0.0.18`), port 1883, user `mqtt`, and the password |

Or paste a backup from another clock: **Backup → Copy settings** on the configured clock,
then **Backup → Paste settings** on the new one. Passwords are deliberately excluded from
the export, so the MQTT and config passwords must be typed on each clock.

### Broker addressing

Use the broker host's **IP address**. Neither of the obvious names works from a Photon:

- `core-mosquitto` is internal to Home Assistant's container network
- `homeassistant.local` is mDNS, which Particle's `WiFi.resolve()` does not speak
- the bare name `homeassistant` is NXDOMAIN on the LAN's DNS

Home Assistant has a static DHCP lease, so `10.0.0.18` is stable.

## 5. Confirm in Home Assistant

**Settings → Devices & Services → MQTT** should show a new device named
`Lixie Clock <id>`, where `<id>` is the last six characters of the Particle device ID. Each
clock gets its own id, so both appear separately.

`shop-clock` is `Lixie Clock 343437`.

## Rolling back

The previous firmware is in the old repo:

```bash
particle flash <device> ~/path/to/lixie-clock/photon_firmware_1773097580563.bin
```

Settings written by this firmware live in EEPROM and are ignored by the old firmware, so a
rollback is non-destructive and flashing forward again finds them intact.
