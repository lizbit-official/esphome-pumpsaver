# esphome-pumpsaver

Monitor a **SymCom / Littelfuse PumpSaver Plus** pump-protection relay in
Home Assistant. The only hardware is an ESP32 and a $0.30 IR phototransistor.
Nothing is wired to the relay, and nothing is transmitted.

PumpSaver relays constantly broadcast their internal state over infrared,
meant for SymCom's discontinued *Informer* handheld: live **volts / amps /
watts / power factor**, lifetime **pump-start** and **run-time** counters, and
the **last-20-faults history**. This component decodes the broadcast passively
and publishes it all, including a fault notifier the original tooling never
had.

Tested on the **233-P**. The same hardware ships rebranded as **Pentek /
Pentair SPP-233P / SPP-235P / SD-F30x** (also sold under Berkeley, Myers and
Sta-Rite labels) and as **Goulds / CentriPro "PumpSaver by SymCom"** units.
The wire format was reverse engineered and verified in the companion
[pumpsaver-ir-protocol](https://github.com/lizbit-official/pumpsaver-ir-protocol)
repo; [PROTOCOL.md](https://github.com/lizbit-official/pumpsaver-ir-protocol/blob/main/PROTOCOL.md)
has the full specification.

<!-- TODO: photo of the phototransistor mounted at the relay + HA dashboard -->

## Quick start

You need an ESP32 and a **bare IR phototransistor** (~$0.30; *not* a TSOP-style
"IR remote receiver", see [Hardware](#hardware)). Wire the phototransistor
between a GPIO and GND, then:

```yaml
external_components:
  - source: github://lizbit-official/esphome-pumpsaver@v0.3.0
    components: [pumpsaver]

remote_receiver:
  id: ir_rx
  pin:
    number: GPIO27          # your phototransistor pin
    inverted: true          # active-low: IR light pulls the pin down
    mode: { input: true, pullup: true }
  filter: 10us              # real pulses are >= ~95 us
  idle: 40ms                # one frame per ~0.5 s transmission
  buffer_size: 32kb
  tolerance: 45%
  receive_symbols: 2048
  # clock_resolution: 500000   # required for idle > 32ms on ESP32-C3/S3/C6

pumpsaver:
  receiver_id: ir_rx

sensor:
  - platform: pumpsaver
    voltage: { name: "Pump Voltage" }
    power: { name: "Pump Power" }
    pump_starts: { name: "Pump Starts" }
    signal_rate: { name: "PumpSaver Signal Rate" }   # the aiming tool
```

Flash it, open the device page, and **move the phototransistor until
`Signal Rate` reads ~40 words/s**. That is a perfect view of the broadcast,
and entities update within seconds.

The complete config with every entity (current, power factor, run time, fault
history, diagnostics, raw registers, pump-running binary sensor) is
[`example.yaml`](example.yaml).

## Hardware

### Microcontroller

- **Any ESP32** with a free RMT-capable GPIO (the component sits on top of
  ESPHome's [`remote_receiver`](https://esphome.io/components/remote_receiver.html)).
  Classic ESP32, S2, S3, C3, C6 all work. On variants other than the classic
  ESP32/S2 the RMT idle limit is 32767 µs at the default clock, so use
  `idle: 30ms` or add `clock_resolution: 500000`.
- ESP8266 is untested and not recommended (no RMT; software capture may drop
  the 100 µs pulses).

### IR sensor: read this before buying anything

The PumpSaver broadcast is **baseband IR: there is no 38 kHz carrier**.

> **Safety: stay optical.** Everything here is receive-only, at a distance.
> Don't open the relay or tap its IR LED electrically: it is a 240 V
> line-connected device whose internals may be line-referenced, and the IR
> link already gives you galvanic isolation for free.


> **TSOP-style demodulating IR receivers (TSOP38238, VS1838B, KY-022 modules,
> anything sold as an "IR remote receiver") WILL NOT WORK.** They band-pass
> filter for a 38/56 kHz carrier this signal does not have, and will output
> nothing.

Use a **bare IR phototransistor**. The development installation uses an
**OSRAM SFH 309 FA** (DigiKey `475-SFH309FA-4/5-ND`, ~$0.60), a good default:
its built-in daylight filter (730–1120 nm) rejects visible ambient light
optically, and its 7 µs switching has 13x margin on the signal's shortest
pulses. Note its narrow ±12° acceptance angle: aim it at the IR window (the
`signal_rate` diagnostic makes that easy). Any cheap 3 or 5 mm IR
phototransistor also works (PT334-6C, L-53P3C, or whatever your parts drawer
labels an "IR receiver diode"; unfiltered clear-package parts are more
sensitive to ambient light). Three-lead phototransistors (e.g. Vishay
BPW77NA) also work but add pitfalls: wire collector and emitter only, leave
the base floating, and beware that unfiltered high-sensitivity parts can
saturate in room light and read as dead. Requirements: resolves 100 µs to 3 ms pulses
(≥10 kHz bandwidth, which practically any phototransistor has), ~850–950 nm,
and wired to pull the GPIO low with the internal pull-up as the load (hence
`inverted: true`):

```
        ESP32                    IR phototransistor
      ┌────────┐
      │   3V3  │                    (no external
      │        │                     resistor needed:
      │ GPIO27 ├──────────┬─────┐    internal pullup
      │        │ (internal│     │    is the load)
      │        │  pullup  │   collector
      │        │  enabled)│     │      ▄▄
      │        │          │     ├──── ▐░░▌ ◄─── IR from the
      │        │          │     │      ▀▀       PumpSaver's
      │        │          │   emitter           display window
      │   GND  ├──────────┴─────┘
      └────────┘
   IR light on  → transistor conducts → GPIO low  (active)
   IR light off → pullup wins         → GPIO high (idle)
```

Two-pin phototransistors are easy to wire backwards. Reversed polarity is
harmless at 3.3 V but reads nothing: **if you get zero signal, swap the two
legs first.**

### Placement

- Within **1–10 ft** of the relay, aimed at the IR window on its face. The
  relay broadcasts whenever it is powered; no pairing needed.
- Avoid direct sunlight or strong ambient IR on the sensor. Daylight-filtered
  parts (like the SFH 309 FA) handle this optically; for unfiltered
  clear-package parts, a short piece of heat-shrink makes a good lens hood.
- Use the `signal_rate` diagnostic to aim (see [Troubleshooting](#troubleshooting)).

## Configuration reference

One hub (`pumpsaver:` with `receiver_id`) plus entities on three platforms:

| Option (platform) | Reads | Unit / class | Updates |
|---|---|---|---|
| `voltage` (sensor) | live line voltage | V, `voltage` | ~1.5 s, on change |
| `current` (sensor) | live current ¹ | A, `current` | ~1.5 s, on change |
| `power` (sensor) | live active power | W, `power` | ~1.5 s, on change |
| `power_factor` (sensor) | live power factor | `power_factor` | ~1.5 s, on change |
| `pump_starts` (sensor) | lifetime starts counter | `total_increasing` | on change |
| `run_minutes` (sensor) | lifetime run-time | min, `total_increasing` | +1/min while pumping |
| `last_fault_at` (sensor) | run-clock minutes of the newest fault | min | once at boot, then **only when a new fault lands** |
| `last_fault` (text_sensor) | newest fault, rendered ² | text | same as `last_fault_at` |
| *(binary_sensor)* `threshold:` | pump running (power ≥ threshold) | `running` | on state change |
| `signal_rate` (sensor) | decoded words/s | diagnostic | every ~30 s |
| `decode_errors` (sensor) | bad bursts/min | diagnostic | every ~30 s |
| `register: 0xNN` (sensor) | any raw 16-bit register ³ | none | on change |

¹ **Current reads ~2x true motor current while the pump runs** (the relay's
current channel sees both hot legs of the 240 V circuit under load; idle
readings are single-count and correct). `power` is true watts and needs no
correction. For true running amps add `filters: [multiply: 0.5]` and accept
halved idle readings. Details: [PROTOCOL.md §5](https://github.com/lizbit-official/pumpsaver-ir-protocol/blob/main/PROTOCOL.md#5-register-map).

² Example: `dry well / underload - 774 W, 241.7 V, 5.70 A @ 22d 14h 52m`
(code, W/V/A snapshot, run-clock timestamp, like the Informer showed it).
Dry-well is a proven code name; `overcurrent?` / `voltage fault?` /
`rapid cycle?` follow the documented family ordering, pending confirmation.

³ For protocol exploration; see the
[register map](https://github.com/lizbit-official/pumpsaver-ir-protocol/blob/main/pumpsaver_ir/registers.json).
Values are raw integers; scale with ESPHome `filters:`.

Everything publishes on change only (the broadcast repeats every ~1.5 s for
live values, ~5.8 s for the fault ring), so Home Assistant is not spammed.
Each entity also publishes once shortly after boot.

## In Home Assistant

The counters are `total_increasing`, so long-term statistics work out of the
box. Daily starts and runtime are one
[`utility_meter`](https://www.home-assistant.io/integrations/utility_meter/) away:

```yaml
# configuration.yaml
utility_meter:
  pump_starts_today:
    source: sensor.pumpsaver_monitor_pump_starts
    cycle: daily
  pump_runtime_today:
    source: sensor.pumpsaver_monitor_pump_run_time
    cycle: daily
```

And the fault entities give you something the Informer never could: a push
notification when the relay trips, with the full fault record.

```yaml
# configuration.yaml (or the automation editor)
automation:
  - alias: "Pump fault alert"
    trigger:
      - platform: state
        entity_id: sensor.pumpsaver_monitor_pump_last_fault_at
    condition:
      - "{{ trigger.from_state.state not in ['unknown', 'unavailable'] }}"
    action:
      - service: notify.mobile_app_your_phone
        data:
          title: "⚠️ Well pump fault"
          message: "{{ states('sensor.pumpsaver_monitor_pump_last_fault') }}"
```

(Bonus flourish: graph `Pump Voltage` and watch your house's line voltage sag
a few volts at the instant of pump inrush.)

## Troubleshooting

**The `signal_rate` diagnostic is the aiming tool.** The broadcast is a fixed
237 words per 5.81 s cycle, so the number is absolutely calibrated:

| Reading | Meaning |
|---|---|
| ~38–41 words/s | Perfect view. You're done |
| 15–35 | Partial view. Re-aim, move closer |
| 1–15 | Marginal: off-axis, too far, or strong ambient IR |
| 0 / no reading | Wrong sensor type (TSOP?), wiring, or reversed phototransistor legs |

- **Nothing decodes:** nine times out of ten it's the sensor. TSOP/38 kHz
  receivers output nothing on this signal. Otherwise: swap the
  phototransistor's legs, move closer, aim at the IR window, re-check
  `inverted: true` and the pullup.
- **Nothing decodes and the GPIO reads constant low:** an unfiltered,
  high-sensitivity part is likely saturated by ambient light (the transistor
  is fully on). Shade it, test in the dark, or use a daylight-filtered part.
  On 3-lead parts, also confirm the base lead is floating.
- **Words decode but entities don't update:** entities publish on change only;
  constant registers publish once after boot and then hold.
- **Config error on `idle: 40ms` (ESP32-C3/S3/C6):** RMT idle limit. Use
  `idle: 30ms` or add `clock_resolution: 500000`.
- No `signal_rate` configured? `logger: level: VERBOSE` logs
  `Decoded 20 word(s) ...` about twice a second.

## Notes & caveats

- **All values come from the relay's own metering.** This component adds no
  measurement of its own; accuracy is whatever the relay's calibration gives
  you.
- Tested against the **233-P** only. Other Informer-era models (231-P Insider,
  233-1.5-P, 234-P, 235P, 236-P, 111P and the Pentek / Goulds rebrands) very
  likely share the framing but may map registers differently. The generic
  `register:` sensor is your friend there, and captures are very welcome
  (below).
- Coexists with other `remote_receiver` users (e.g. TV-remote binary sensors):
  it claims only frames that decode as PumpSaver words.

## Capturing raw data for protocol work

To help map the remaining registers, or add support for a different model,
dump raw timing frames in the NDJSON format the
[protocol repo](https://github.com/lizbit-official/pumpsaver-ir-protocol)'s
reference decoder consumes directly:

```yaml
mqtt:
  broker: !secret mqtt_broker

remote_receiver:
  # ... same receiver config as above ...
  on_raw:
    then:
      - mqtt.publish:
          topic: pumpsaver/ir/raw
          payload: !lambda |-
            std::string payload = "{\"ts\":" + to_string(millis()) +
                                  ",\"len\":" + to_string(x.size()) + ",\"data\":[";
            for (size_t i = 0; i < x.size(); i++) {
              if (i) payload += ",";
              payload += to_string(x[i]);
            }
            payload += "]}";
            return payload;
```

Collect with `mosquitto_sub -t pumpsaver/ir/raw > capture.ndjson` and file it
via the protocol repo's
[capture form](https://github.com/lizbit-official/pumpsaver-ir-protocol/issues/new?template=01-submit-capture.yml).

## License

MIT © 2026 Elizabeth Camporeale. Not affiliated with or endorsed by
Littelfuse, Inc. "SymCom", "PumpSaver" and "Informer" are trademarks of their
respective owners.
