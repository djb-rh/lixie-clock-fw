# Phase 5 — Home Assistant

`shop-clock` appears in Home Assistant automatically, is controllable from it, and that
control cleanly outranks the schedule.

```
Flash  76,168 / 131,072   Static RAM  13,748   Free heap  ~26,800
```

## Broker: what was told to me vs what works

The reported hostname was `core-mosquitto`. That is the add-on's **internal Docker
hostname**, resolvable only inside Home Assistant's container network — a Photon on the
LAN cannot reach it. `homeassistant.local` does resolve, but only via **mDNS**, which
Particle's `WiFi.resolve()` does not speak, and the bare name `homeassistant` is NXDOMAIN
on the LAN's DNS server.

So the clock uses **`10.0.0.18:1883`** directly. Verified before storing anything:

| Check | Result |
|---|---|
| Port 1883 reachable | open |
| user `mqtt` + password | accepted |
| anonymous connect | refused — auth is enforced |

Home Assistant has a static DHCP lease, so the literal IP is safe.

## Entities published

One device, `Lixie Clock 343437`, carrying eight entities:

| Kind | Entity | Payload |
|---|---|---|
| light | the clock | 501 B — `rgb` colour mode, brightness, 8 effects |
| button | Return to schedule | 213 B |
| sensor | Wi-Fi signal, Uptime, Free memory, Last NTP sync, IP address, Controlled by | 261–355 B each, all `entity_category: diagnostic` |

Discovery uses **full Home Assistant key names**, not the abbreviated forms. There is
ample flash and buffer for it, and a half-remembered abbreviation fails as a silently
ignored key — an entity that turns up missing a feature with nothing in any log to explain
why. The MQTT buffer is 1536 bytes against a largest payload of 501.

Discovery is retained and republished on every connect, so a Home Assistant restart
re-learns the entities without the clock needing one. A last will marks the clock
unavailable if it drops off without saying goodbye — the normal case for a power cut.

The effect list comes from `EFFECT_NAMES`, the same array the config page reads over
`/api/config`, so the firmware, the web UI and Home Assistant cannot disagree about which
id is which effect.

## The override is sticky, as specified

`Control` gained a third layer above the schedule. Once Home Assistant sets anything it
holds **indefinitely** — only an explicit release hands control back. A schedule
transition arriving later does not quietly stomp an automation's work.

Release happens two ways: the `Return to schedule` button in Home Assistant, or the banner
that appears in the config page while an override is active.

Two details worth calling out:

- **Partial commands merge into what is showing.** A command that only sets brightness
  does not reset colour and effect to defaults.
- **Setting a colour drops hue-driven effects only.** Rainbow Flow, Rainbow Cycle and Warm
  Glow ignore the base colour, so asking for a colour while one is running would do
  nothing visible — those fall back to Solid. Breathe, Pulse, Comet and Twinkle *modulate*
  the chosen colour, so they survive. This distinction cost two wrong test assertions
  before I got the test to describe the actual design rather than my first guess at it.
- **`OFF` means dark**, not 1% brightness.

## Verified end to end

`scratchpad/ha_test.py` publishes the same payloads Home Assistant would, then reads the
clock's own REST status to confirm what took effect — checking the MQTT state topic alone
would only prove the clock agrees with itself.

```
ok  source switched to Home Assistant
ok  brightness 64/255 -> 25%
ok  Breathe kept, because it modulates the chosen colour
ok  rainbow dropped, since it would ignore the colour
ok  brightness preserved across a partial command
ok  schedule did NOT take control back
ok  display is dark, not merely dim
ok  clock reported OFF back to HA
ok  override cleared, schedule resumed with its own settings
```

The schedule-does-not-stomp check is the one that matters: a schedule entry was installed
and allowed to fire *while* the override was active, and control stayed with Home
Assistant.

## A bug I nearly introduced

The MQTT state topic reported `brightness: 255` and `effect: Breathe` when I expected
solid amber at 60%, and I went looking for a serialisation bug. There wasn't one — the
base config genuinely was Breathe at 100%, because the clock's owner had changed it from
the config page while I was working. Checking `/api/config` before "fixing" anything is
the only reason a working feature did not get reverted.

## Still outstanding

- **Router-reboot recovery** — written in Phase 1, still never exercised. Phase 6 soak.
- **`Clock2` has never been flashed.** Everything so far is `shop-clock`. Its
  Product-claimed OTA path remains unproven, and its MQTT device id will differ.
- The config page still has not been opened in a real browser from this side, though its
  owner has now used it (they switched the clock to Breathe).
