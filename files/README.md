# AC and UPS alarm monitor

An ESP32 that watches precision air conditioning units and UPS systems on a
local network and raises an audible alarm on a desk, independently of any
existing monitoring system. It is a secondary warning, not a replacement for
the equipment's own alarm handling.

The board polls every unit on a fixed round, shows a summary on a small OLED,
and beeps once when something needs attention. Nothing is stored off the
device and nothing is sent anywhere.

## What it watches

Three kinds of device, handled differently because their firmware differs:

| Type | Source | Alarm condition |
|---|---|---|
| Older AC units | XML event log over HTTP | a log entry of type `Alarm` |
| Newer AC units | CSV variable query over HTTP | an alarm variable reading `1` |
| UPS units | SNMP v1 | battery temperature or charge outside limits |

On top of that, every AC unit is checked against its own setpoint: if the
measured temperature reaches setpoint plus three degrees, the alarm sounds.
That one is the useful part in practice, because it fires before the unit
itself decides anything is wrong.

## Hardware

- ESP32-WROOM-32 development board, 38 pin
- SH1106 128x64 I2C OLED, 1.3 inch, at address `0x3C`
- Active buzzer module

Wiring:

| Component | Pin | ESP32 |
|---|---|---|
| OLED | VCC | 3V3 |
| OLED | GND | GND |
| OLED | SCL | GPIO22 |
| OLED | SDA | GPIO21 |
| Buzzer | VCC | 5V |
| Buzzer | GND | GND |
| Buzzer | S | GPIO23 |

A passive buzzer works too, driven through LEDC at around 2750 Hz. The pin
has to be left as `INPUT_PULLDOWN` between beeps in that case, because
driving it `LOW` as a plain output was not enough to keep it quiet.

## Building

Requires [PlatformIO](https://platformio.org/).

```
git clone <this repo>
cd <this repo>
cp src/config.example.h src/config.h
```

Edit `src/config.h` with the WiFi credentials, the SNMP community and the
addresses of the units. That file is in `.gitignore` and is never committed.

Then:

```
pio run -t upload -t monitor
```

## How it behaves

A round runs every 90 seconds and visits each unit in turn, with a short gap
between them. Temperature readings are collected on a slower three minute
timer, because temperature moves slowly and reading it costs an extra request
on the older units.

When an alarm is raised the screen switches to the unit that caused it and the
buzzer sounds once. The screen holds that state for at least ten minutes, then
clears itself once the unit reports the condition has passed. There is no
silence button: the device is meant to be left alone.

Idle screen looks like this:

```
AC MONITOR
max AC-4 24.1C
UPS 27 28 26 29 27
last check 45s ago
idle
```

If a unit stops answering for five rounds in a row it is listed as
unreachable and the buzzer gives two short beeps. Silence never means "all
clear" on its own.

## Notes on the equipment

Worth recording, since none of it is obvious from the vendor documentation.

**Older units** run an embedded web server that is slow in a specific way:
some of them stall for a flat ~30 seconds before answering, seemingly on a
fixed internal timeout rather than under load, and the same stall appears
regardless of the size of the file requested. Timeouts here are set to 35
seconds to accommodate that. The event log is fetched with a `HEAD` first and
only downloaded when `Last-Modified` changes, which keeps the load to almost
nothing on a normal day. The response is parsed as a stream and the connection
is closed as soon as the last known record is reached, because the server does
not support range requests.

**Newer units** serve a JavaScript web interface that cannot be scraped from a
microcontroller, but the same data is available from a CGI endpoint that
returns plain CSV. Each alarm has its own named boolean variable, so alarm
names come out of the query rather than a lookup table.

An attempt to read the newer units over SNMP was abandoned: the values are
exposed as hundreds of unlabelled flags, and querying them one at a time
exhausted the ESP32's UDP buffers and stretched a round to thirteen minutes.
The CSV route is one request instead of four hundred.

**UPS units** answer on the standard UPS MIB (RFC 1628), two OIDs per unit per
round.

## License

MIT
