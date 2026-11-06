# Hardware and wiring

[Back to README](../README.md) · [Setup](getting-started.md)

## Current Seeed hardware

| Role | Board and radio kit | Kit SKU |
|---|---|---|
| Transmitter | Seeed XIAO nRF52840 + through-hole Wio-SX1262 | 102010710 |
| Gateway | Seeed XIAO ESP32S3 + matching 30-pin B2B Wio-SX1262 | 102010611 |
| Probe | One three-wire DS18B20 temperature probe | Check probe vendor specification |
| Optional display | 1.3-inch SH1106, 128×64, four-pin I²C, blue pixels | Check controller, not screen size alone |

These carriers have different wiring. Do not interchange their pin maps. The previous Heltec V3 prototype is historical and is not the target of the sketches in this repository.

## Probe → transmitter

Disconnect USB and battery before wiring. Use externally powered, three-wire operation.

| Probe / adapter terminal | XIAO nRF52840 |
|---|---|
| VCC / VDD / + | **3V3** |
| GND / − | **GND** |
| DATA / DQ / S | **D6** (also labeled UART TX on the XIAO) |

Use **one 4.7 kΩ pull-up between D6 and 3V3**. If the probe adapter already includes that resistor, do not add a second one. Identify wires from the probe/adapter specification rather than colors alone. Do not connect the data line to 5 V or use parasite-power wiring.

The probe connects only to the transmitter. D6 is available because this sketch does not use GNSS or Serial1.

## OLED → gateway

| OLED label | XIAO ESP32S3 |
|---|---|
| GND | **GND** |
| VCC | **3V3** |
| SCK / SCL | **D5 / GPIO6** |
| SDA | **D4 / GPIO5** |

Use the display gateway sketch. It checks 0x3C and 0x3D. SCK on this four-pin module means the I²C clock, not the LoRa SPI clock. An SSD1306 screen needs a different display driver; the included display sketch targets SH1106.

## Radio connections already provided by the kits

These are implementation references, not instructions to rewire a correctly assembled kit.

| Signal | nRF52840 transmitter carrier | ESP32S3 gateway carrier |
|---|---|---|
| NSS / CS | D4 | GPIO41 |
| DIO1 / IRQ | D1 | GPIO39 |
| Reset | D2 | GPIO42 |
| Busy | D3 | GPIO40 |
| Receive enable | D5 | GPIO38 |
| SPI SCK | D8 | GPIO7 |
| SPI MISO | D9 | GPIO8 |
| SPI MOSI | D10 | GPIO9 |
| Carrier USER | D0 (not used by this transmitter sketch) | GPIO21 |

The gateway USER button is separate from the XIAO BOOT and RESET buttons. Hold USER five seconds for Wi-Fi setup. Short USER toggles the OLED in the display build; the no-display build uses the webpage and Serial Monitor.

## Power and assembly

The gateway is intended to stay powered over USB. The transmitter can run from its battery connection; USB power affects battery measurements and sleep-current testing. For the experimental solar-light arrangement, see [solar testing](solar.md). Do not substitute the 3V3 pin for the battery input.

Secure and solder connections before relying on the unit outside. Loose headers were a source of uncertainty in the earlier SD test. SD remains disabled; the dormant SD definitions in the sketches are not a verified wiring guide for these Seeed kits.

Match antennas to the correct Wi-Fi and LoRa connectors. Radio antenna selection and host integration remain subject to the [open radio review](radio-status.md).

## Manufacturer references

- [Seeed nRF52840 + Wio kit](https://wiki.seeedstudio.com/xiao_nrf52840%26_wio_SX1262_kit_for_meshtastic/)
- [Seeed nRF52 Arduino core and variant definitions](https://github.com/Seeed-Studio/Adafruit_nRF52_Arduino)
- [Wio-SX1262 for XIAO schematic](https://files.seeedstudio.com/products/SenseCAP/Wio_SX1262/Schematic_Diagram_Wio-SX1262_for_XIAO.pdf)
- [DS18B20 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/ds18b20.pdf)
