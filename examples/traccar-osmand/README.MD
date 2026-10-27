# Traccar OsmAnd protocol — SIM7670G Light sleep power consumption

- Hardware Board-ID: LILYGO_T_A7670
- The demo uses SIM7670G as a test sample. When the device starts, the instantaneous current consumption is about 200mA+. The ESP only acts as MCU control, and the WiFi and BLE functions are not turned on.

- When the device is successfully located, after waiting for the location to be sent, the average current consumption is the smallest, about 2~3mA. In the next cycle, the modem will be awakened, and the instantaneous current will rise to 120mA+. Since the modem is not turned off, the subsequent positioning speed is very fast, and it takes about a few seconds to continue to obtain the location, and the cycle is repeated.

## Protocol

- The protocol is **OsmAnd**. Documentation: https://www.traccar.org/osmand/
- Post format (see [`traccar-osmand.ino`](traccar-osmand.ino:57)):
  `deviceid=<id>&lat=<lat>&lon=<lon>&speed=<speed>&altitude=<alt>&batt=<batt>`

## Preconfigured example server

The sketch is preconfigured with the Traccar demo server 3 (see [`traccar-osmand.ino`](traccar-osmand.ino:55)):

| Item | Value |
|---|---|
| Domain | demo3.traccar.org |
| IP address | 188.245.151.94 |
| Port | 5055 |
| Device ID (Kennung) | lilygo-01 |
| URL | `http://demo3.traccar.org:5055/` |

> **Note:** The demo server speaks plain **HTTP** on port 5055, therefore the sketch uses the `http_*` modem functions (not `https_*`).
> For your own server, adjust `client_id` and `request_url` in [`traccar-osmand.ino`](traccar-osmand.ino:55).
