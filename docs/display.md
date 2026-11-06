# SH1106 display and weather

[Back to README](../README.md) · [Wiring](hardware.md#oled--gateway)

Use the **display gateway sketch, revision 8**, for the 1.3-inch SH1106 blue OLED. Use the **no-display sketch, revision 6**, if you do not want an OLED or internet weather requests. Both retain the local browser dashboard and logs.

## Screens and buttons

| Screen | Behavior |
|---|---|
| Startup | The Southern Bluegill fish logo alone, centered, for about three seconds; initial storage startup can extend the splash |
| Wi-Fi setup | Actual setup network name and setup IP; stays visible while setup is active |
| Connection success | Home dashboard IP for about ten seconds after connection |
| Probe | Large probe temperature, report age and transmitter voltage; ten seconds in normal loop |
| Clock/weather | Local time and modeled outdoor air conditions; eight seconds |
| Dashboard | Current home IP; six seconds |
| Logo | Logo alone; three seconds, then return to probe |

All screen text is centered. Long text is shortened. The OLED **stays on continuously**; the previous one-minute timeout is removed. Short USER press toggles it off/on. Hold USER five seconds to reopen Wi-Fi setup. Turning off the display does not stop reception, logging or the website.

“Bench test” is the saved measurement-type label, not an error. Bench/air mode overrides the water body name. For a water deployment, select a water measurement type and save an optional water body name to show that name above the temperature.

The sketch continues if no OLED is detected at 0x3C or 0x3D. It does not auto-detect SSD1306 versus SH1106.

## Weather location

Weather uses **City, state or ZIP** from the same deployment settings page. Water body name and probe depth do not affect lookup. The geocoder is restricted to US results; ambiguous/unresolved places request a ZIP instead of silently selecting a different city. Coordinates are cached in RAM, not added as new user fields or extra flash records.

The clock synchronizes using NTP. Before location resolution it falls back to a labeled US Central clock. With weather resolved, it uses that location’s UTC offset, refreshed with weather. A daylight-saving transition may take until the next successful refresh to appear. CSV timestamps remain UTC receipt times.

## What the weather means

**AIR** is modeled outdoor air temperature from Open-Meteo, not the DS18B20 reading or a local weather-station measurement. It is not fed into the probe log or FishCast model. Weather refreshes about every 30 minutes; failures retry after five minutes. Data older than 90 minutes by its source timestamp is labeled stale. The display reports cache age as “Updated … min ago.”

Network requests and JSON parsing run in a separate FreeRTOS task. That task does not access radio, log files, settings files or the OLED; results pass through queues to the main loop. This design avoids waiting for HTTP in the radio loop. Actual reception under weather/TLS load and runtime heap use still require hardware verification.

Requests use HTTPS certificate checking with ISRG Root X1; NTP time must be valid first. Issuer changes may require a certificate update. There is no insecure TLS fallback. An internet outage can stop weather updates without intentionally stopping local logging.

## Service terms and attribution

Weather: [Open-Meteo](https://open-meteo.com/). Location data: [GeoNames](https://www.geonames.org/) through the Open-Meteo geocoder.

The included public weather endpoints are intended for **personal, non-commercial bench testing**. Before commercial deployment/distribution, arrange suitable service access or a site-owned endpoint. Do not assume the MIT software license grants weather-service usage rights. No weather API key or probe-upload endpoint is embedded in this build.

- [Weather API documentation](https://open-meteo.com/en/docs)
- [Geocoding documentation](https://open-meteo.com/en/docs/geocoding-api)
- [Service terms](https://open-meteo.com/en/terms)
