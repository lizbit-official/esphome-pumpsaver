# esphome-pumpsaver

Monitor a **SymCom / Littelfuse PumpSaver Plus** pump-protection relay in
Home Assistant. The only hardware is an ESP32 and a $0.30 IR phototransistor.
Tested on the **233-P** — the same hardware also ships rebranded as
**Pentek / Pentair SPP-233P / SPP-235P / SD-F30x** and as
**Goulds / CentriPro "PumpSaver by SymCom"** units.

PumpSaver relays constantly broadcast their internal state over infrared,
intended for SymCom's discontinued *Informer* handheld:

- live **volts, amps, watts, power factor**
- lifetime **pump-start** and **run-time** counters
- trip-point configuration and the last-20-faults history

This component listens passively — nothing is wired to the relay and nothing
is transmitted — and publishes it all as ESPHome sensors.

The wire format was reverse engineered and verified in the companion
[pumpsaver-ir-protocol](https://github.com/lizbit-official/pumpsaver-ir-protocol)
repository; [PROTOCOL.md](https://github.com/lizbit-official/pumpsaver-ir-protocol/blob/main/PROTOCOL.md)
has the full specification.

## Hardware

### Microcontroller

- **Any ESP32** with a free RMT-capable GPIO (the component sits on top of
  ESPHome's [`remote_receiver`](https://esphome.io/components/remote_receiver.html),
  which uses the ESP32's RMT peripheral). Classic ESP32, S2, S3, C3, C6 etc.
  all work; on variants other than the classic ESP32/S2 the RMT idle limit is
  32767 µs at the default clock, so use `idle: 30ms` or set
  `clock_resolution: 500000` (see the example).
- ESP8266 is untested and not recommended (no RMT; software capture may drop
  the 100 µs pulses).

### IR sensor — read this before buying anything

The PumpSaver broadcast is **baseband IR: there is no 38 kHz carrier**.

> **TSOP-style demodulating IR receivers (TSOP38238, VS1838B, KY-022 modules,
> anything sold as an "IR remote receiver") WILL NOT WORK.** They band-pass
> filter for a 38/56 kHz carrier that this signal does not have, and will
> output nothing.

Use a **bare IR phototransistor or photodiode module** instead. Requirements:

- Must resolve pulses of **100 µs to ~3 ms** (≥10 kHz bandwidth — practically
  any IR phototransistor works; very slow "ambient light" sensors don't).
- Wavelength ~850–950 nm (standard IR).
- Open-collector / phototransistor output wired to **pull the GPIO low** when
  it sees IR, with the ESP32's **internal pull-up** enabled. The logic is
  therefore active-low, which is why the config uses `inverted: true`.

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

Concretely: any cheap 5 mm IR **phototransistor** works — e.g. PT334-6C,
L-53P3C, or whatever your parts drawer labels an "IR receiver diode" (the
2-pin part, ~$0.30). Two-pin phototransistors are easy to wire backwards:
reversed polarity is harmless at 3.3 V but reads nothing — **if you get zero
signal, swap the two legs first.**

### Placement

- Within **1–10 ft** of the PumpSaver, aimed at the IR window on the 233-P's
  face (the broadcast comes from the display/indicator area).
- Avoid direct sunlight or strong ambient IR (incandescent lamps, heaters) on
  the sensor — a short piece of heat-shrink over the phototransistor as a
  "lens hood" helps in bright rooms.
- The relay broadcasts continuously whenever it is powered; no pairing or
  triggering is needed.

### Field-proven `remote_receiver` settings

From the working installation this component was developed against:

```yaml
remote_receiver:
  id: ir_rx
  pin:
    number: GPIO27
    inverted: true
    mode:
      input: true
      pullup: true
  filter: 10us          # real pulses are >= ~95 us; drop shorter glitches
  idle: 40ms            # one frame per ~0.5 s transmission
  buffer_size: 32kb
  tolerance: 45%
  receive_symbols: 2048
  # clock_resolution: 500000   # optional; required for idle > 32ms on C3/S3/C6
```

## Installation & example

```yaml
esphome:
  name: pumpsaver-monitor
  friendly_name: PumpSaver Monitor
  min_version: 2025.9.0

esp32:
  board: esp32dev
  framework:
    type: esp-idf

logger:

api:

ota:
  - platform: esphome

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

external_components:
  - source: github://lizbit-official/esphome-pumpsaver@v0.1.0
    components: [pumpsaver]

remote_receiver:
  id: ir_rx
  pin:
    number: GPIO27
    inverted: true
    mode:
      input: true
      pullup: true
  filter: 10us
  idle: 40ms
  buffer_size: 32kb
  tolerance: 45%
  receive_symbols: 2048

pumpsaver:
  receiver_id: ir_rx

sensor:
  - platform: pumpsaver
    voltage:
      name: "Pump Voltage"
    current:
      name: "Pump Current"
    power:
      name: "Pump Power"
    power_factor:
      name: "Pump Power Factor"
    pump_starts:
      name: "Pump Starts"
    run_minutes:
      name: "Pump Run Time"

  # Advanced: expose any raw register (see the protocol repo's register map).
  # Values are the raw 16-bit integers; apply scaling with ESPHome filters.
  - platform: pumpsaver
    register: 0x02
    name: "Calibration Voltage"
    unit_of_measurement: V
    accuracy_decimals: 1
    filters:
      - multiply: 0.1

binary_sensor:
  - platform: pumpsaver
    name: "Pump Running"
    threshold: 200  # watts; idle self-draw is ~26 W, running ~820 W
```

(Same file as [`example.yaml`](example.yaml).)

## What you get in Home Assistant

With the example config, the device shows up via the native API as
**PumpSaver Monitor** with these entities:

| Entity | Type | Classes | Notes |
|---|---|---|---|
| `sensor.pumpsaver_monitor_pump_voltage` | Voltage (V, 1 dp) | `voltage` / `measurement` | Live line voltage — you can watch it sag when the pump starts |
| `sensor.pumpsaver_monitor_pump_current` | Current (A, 2 dp) | `current` / `measurement` | ~0.1 A idle → ~8.7 A running on the test system |
| `sensor.pumpsaver_monitor_pump_power` | Power (W) | `power` / `measurement` | The relay's own wattage reading |
| `sensor.pumpsaver_monitor_pump_power_factor` | PF (3 dp) | `power_factor` / `measurement` | ~0.78 on the test system |
| `sensor.pumpsaver_monitor_pump_starts` | Counter | `total_increasing` | Lifetime pump starts — long-term statistics work out of the box |
| `sensor.pumpsaver_monitor_pump_run_time` | Minutes | `total_increasing` | Lifetime run-time; ticks once per minute while pumping |
| `sensor.pumpsaver_monitor_calibration_voltage` | Voltage (V, 1 dp) | — | Example of the generic `register:` sensor |
| `binary_sensor.pumpsaver_monitor_pump_running` | Running | `running` | Derived from power ≥ `threshold` |

Because the counters are `total_increasing`, HA's statistics engine tracks
them automatically — e.g. daily starts/run-time with a
[`utility_meter`](https://www.home-assistant.io/integrations/utility_meter/):

```yaml
# Home Assistant configuration.yaml
utility_meter:
  pump_starts_today:
    source: sensor.pumpsaver_monitor_pump_starts
    cycle: daily
  pump_runtime_today:
    source: sensor.pumpsaver_monitor_pump_run_time
    cycle: daily
```

## Sensors

| Config key | Register | Unit | Scaling | Update behavior |
|---|---|---|---|---|
| `voltage` | 0x11 | V | raw × 0.1 | live block, rebroadcast every ~1.5 s; published **on change only** |
| `current` | 0x12 | A | raw × 0.01 | live block, ~1.5 s; on change only |
| `power` | 0x10 | W | raw × 1 | live block, ~1.5 s; on change only |
| `power_factor` | 0x13 | — | raw × 0.001 | live block, ~1.5 s; on change only |
| `pump_starts` | 0x0F | starts | raw × 1 | live block, ~1.5 s; on change only (total_increasing) |
| `run_minutes` | 0x17 | min | raw × 1 | live block, ~1.5 s; on change only (total_increasing) |
| `register: 0xNN` | 0x01–0xFE (known: 0x01–0x75) | — | raw value | live block ~1.5 s, fault-ring block ~5.8 s; on change only |
| `binary_sensor` (running) | 0x10 | — | power ≥ `threshold` | on state change only |

Every register is rebroadcast continuously (~1.5 s for the live block
0x01–0x18, ~5.8 s for the fault-history ring 0x19–0x75). To avoid spamming
Home Assistant, this component publishes a sensor only when its raw register
value changes — plus once on the first decode after boot, so entities become
available within a few seconds.

## Notes & caveats

- **All values come from the PumpSaver's own metering.** This component adds
  no measurement of its own — accuracy is whatever the relay's calibration
  gives you.
- **Factor-2 open question on running power:** at idle the reported power
  matches V·I·PF exactly, but while the pump runs it reads almost exactly
  half of V·I·PF — either the current channel double-counts both hot legs of
  the 240 V circuit, or register 0x13 is displacement-PF. See
  [PROTOCOL.md §7](https://github.com/lizbit-official/pumpsaver-ir-protocol/blob/main/PROTOCOL.md#7-open-questions).
  Treat `power` (device-reported) as the authoritative wattage.
- The decoder runs whenever **any Informer-era PumpSaver Plus** is
  broadcasting in view. It has been tested against the **233-P** only; other
  models (231-P Insider, 233-1.5-P, 234-P, 235P, 236-P, 111P — and their
  Pentek SPP-series / Goulds CentriPro rebrands) very likely share the
  framing but may map registers differently — the generic `register:`
  sensor is your friend there.
- Registers 0x01–0x18 refresh every ~1.5 s; 0x19–0x75 (fault history) every
  ~5.8 s. Fault-ring semantics are only partially mapped; see the protocol
  repo's register map (`pumpsaver_ir/registers.json`).
- The component coexists with other `remote_receiver` users (e.g.
  `remote_receiver` binary sensors for TV remotes): it claims only frames
  that decode as PumpSaver words.

## Troubleshooting

- Set `logger: level: VERBOSE` — a working setup logs
  `Decoded 20 word(s) from ... timings` about twice a second
  (`VERY_VERBOSE` additionally logs every register word).
- **Nothing decodes:** nine times out of ten it's the sensor — TSOP/38 kHz
  demodulating receivers output nothing on this signal (see the hardware
  section). Otherwise: swap the phototransistor's legs, move it closer and
  aim it at the IR window, and re-check the pin config (`inverted: true`,
  internal pullup enabled).
- **Words decode but entities don't update:** entities publish on change
  only; constant registers publish once after boot and then hold.
- **ESP32-C3/S3/C6 config error on `idle: 40ms`:** the RMT idle limit is
  32767 µs at the default clock — use `idle: 30ms` or add
  `clock_resolution: 500000`.

## Capturing raw data for protocol work

If you have a different PumpSaver model, or want to help map the remaining
registers, you can dump raw timing frames in the NDJSON format that the
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

Collect the messages into a file (one JSON object per line), e.g.
`mosquitto_sub -t pumpsaver/ir/raw > capture.ndjson`, then run them through
`pumpsaver_ir/decoder.py`. Captures from non-233-P models are very welcome as
issues/PRs on the protocol repo.

## License

MIT © 2026 Elizabeth Camporeale. Not affiliated with or endorsed by
Littelfuse, Inc. "SymCom", "PumpSaver" and "Informer" are trademarks of their
respective owners.
