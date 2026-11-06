# Attribution and license scope

The root MIT License covers the original project software and documentation unless a file states otherwise. It does not relicense third-party dependencies, manufacturer documents, service data, or The Southern Bluegill logo/branding.

## Branding

The supplied fish logo in `assets/logo.png` and its embedded bitmap in the display sketch are The Southern Bluegill branding. They are excluded from the MIT grant; see [assets/LICENSE.md](assets/LICENSE.md). The software license does not grant trademark rights or imply endorsement. Replace the logo/bitmap when using the project under another identity unless you have permission.

## Dependencies and external material

Dependencies are installed separately, not vendored or relicensed here. Follow each upstream project’s own license and notices, including requirements that apply when redistributing firmware binaries.

- [RadioLib](https://github.com/jgromes/RadioLib)
- [U8g2](https://github.com/olikraus/u8g2)
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson)
- [OneWire](https://github.com/PaulStoffregen/OneWire)
- [DallasTemperature](https://github.com/milesburton/Arduino-Temperature-Control-Library)
- [Adafruit SPIFlash](https://github.com/adafruit/Adafruit_SPIFlash)
- [SdFat — Adafruit fork](https://github.com/adafruit/SdFat)
- [Espressif Arduino ESP32 core](https://github.com/espressif/arduino-esp32)
- [Seeed nRF52 Arduino core](https://github.com/Seeed-Studio/Adafruit_nRF52_Arduino)

The display sketch embeds the ISRG Root X1 public certificate for TLS trust. The certificate is third-party trust material, not original project authorship. [ISRG certificate information](https://letsencrypt.org/certificates/).

Weather is supplied by Open-Meteo; geocoding uses GeoNames through its geocoding service. API access and data terms are separate from this software license. See [display documentation](docs/display.md#service-terms-and-attribution).

Manufacturer schematics, datasheets, grants and linked material remain with their respective owners. Historical Meshtastic variant files were implementation references; this project is not Meshtastic firmware and is not affiliated with or endorsed by those projects or manufacturers.
