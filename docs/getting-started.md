# Install, upload and connect

[Back to README](../README.md) · [Wiring](hardware.md)

## 1. Download the project

On GitHub, choose **Code → Download ZIP**, then extract it. Choose one gateway folder:

- **With SH1106:** `firmware/gateway/display/receiver_xiao_esp32s3/receiver_xiao_esp32s3.ino`
- **Without OLED:** `firmware/gateway/no-display/receiver_xiao_esp32s3/receiver_xiao_esp32s3.ino`

The transmitter is `firmware/transmitter/xmiter_nrf52840/xmiter_nrf52840.ino`.

Open each `.ino` directly in Arduino IDE. Keep each file inside its matching folder. Do not open both gateway variants as tabs in the same sketch. If a download adds `(1)` to a filename, restore the original folder/file names.

## 2. Install the board packages and libraries

Add the following URLs in **File → Preferences → Additional Boards Manager URLs**, alongside existing entries:

```text
https://espressif.github.io/arduino-esp32/package_esp32_index.json
https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json
```

### Gateway — both variants

| Setting | Selection |
|---|---|
| Boards Manager package | **esp32 by Espressif Systems 3.3.2** |
| Board | **XIAO_ESP32S3** |
| USB CDC On Boot | **Enabled** |
| Flash size | **8 MB** |
| Partition scheme | **Default with spiffs (3MB APP/1.5MB SPIFFS)** |
| Erase All Flash Before Sketch Upload | **Disabled** |
| Serial Monitor | **115200 baud** |

The firmware uses **LittleFS** in that filesystem partition. Do not choose Maximum APP / No FS. For routine updates, keep the existing partition layout and erase setting unchanged; export a CSV backup first.

| Gateway variant | Install in Library Manager |
|---|---|
| No display, revision 6 | **RadioLib 7.1.2** |
| SH1106 display, revision 8 | **RadioLib 7.1.2**, **U8g2 2.36.17**, **ArduinoJson 7.4.2** |

WiFi, FS, LittleFS, Preferences, WebServer, SD, DNSServer, HTTPClient and WiFiClientSecure are supplied by the ESP32 core. The no-display version does not need U8g2 or ArduinoJson.

### Transmitter

| Setting | Selection |
|---|---|
| Boards Manager package | **Seeed nRF52 Boards 1.1.13**, **non-mbed** |
| Board | **Seeed XIAO nRF52840** — not Sense, Plus or ESP32 |
| SoftDevice | Leave default **S140 7.3.0** |
| Serial Monitor | **115200 baud** |

Install **RadioLib 7.1.2**, **OneWire 2.3.8**, **DallasTemperature 4.0.5**, **Adafruit SPIFlash 5.1.1**, and **SdFat - Adafruit Fork 2.3.103**. Accept required Library Manager dependencies. Linux may also need `adafruit-nrfutil` for the package’s uploader.

## 3. Upload the gateway first

1. Use a USB **data** cable, select the gateway’s port, and verify/upload the chosen sketch.
2. Open Serial Monitor at **115200**. If blank, reselect the application port and tap XIAO RESET once.
3. Look for **log initialization: OK** and **LoRa listening**. A new unit without saved Wi-Fi opens **Bluegill-Setup-XXXX**.

**Upload stuck at Connecting / Errno 71:** Close Serial Monitor and unplug USB. Hold **XIAO BOOT**, plug in USB, then release BOOT once connected. Reselect the resulting port and upload. If needed, hold BOOT, briefly press/release XIAO RESET, then release BOOT. Keep the radio board attached if BOOT is reachable. Try another known-good cable/direct USB port if USB errors continue. XIAO BOOT/RESET are not the carrier USER button.

**Storage mount error:** A completely erased filesystem initializes automatically. A nonblank partition that fails to mount is preserved. Only for a genuinely new unit with no logs to keep, the Serial command `INIT NEW FLASH` followed by a newline erases/reinitializes its log partition. This is not a recovery command for existing data.

## 4. Set up home Wi-Fi

1. On your phone, join **Bluegill-Setup-XXXX**. There is no setup-network password.
2. Stay connected if the phone warns “No internet.” If the portal does not open, visit **http://192.168.4.1/wifi**, or the address shown on the OLED/Serial Monitor.
3. Choose your home **2.4 GHz** Wi-Fi network, enter its router password, and tap **Connect and save**. The ESP32S3 does not support 5 GHz Wi-Fi.
4. Note the home-network dashboard address. Setup closes about ten seconds after success. Rejoin home Wi-Fi on your phone and open that address.

**Close setup** closes the gateway’s hotspot; it cannot switch the phone back to home Wi-Fi. Setup also expires after ten minutes. Hold carrier **USER for five seconds** to reopen it. Changing Wi-Fi preserves logs. A normal router outage does not automatically reopen the open setup network.

The home IP is assigned by the router and can change. Use the current address shown on the display or Serial Monitor; an example IP in a screenshot is not universal. Saved Wi-Fi reconnects after restart.

## 5. Wire and upload the transmitter

Follow the [probe wiring table](hardware.md#probe--transmitter), with power disconnected. Then select the nRF52840 board and its port and upload the transmitter sketch.

If no upload port appears, double-tap the XIAO RESET button to enter its bootloader, select that port, and upload. Do not select NRF-OTA or erase/update the bootloader for this sketch.

Look for:

- A 16-character transmitter ID generated automatically.
- **DS18B20 devices found: 1**.
- **TX result 0**, followed by the packet. This reports local radio transmission success, not proof the gateway received it.

The transmitter sends soon after startup, then about every 15 minutes. Resetting it starts a new boot ID and sequence run. OneWire’s fallback-mode warning on nRF52 does not by itself mean the probe failed; check the runtime result.

If external flash prints `0x000000`, disconnect **both USB and battery**, wait ten seconds, and cold-start it. The tested unit reported `0x856015` and accepted the sleep command. Neither message measures actual idle current.

## 6. Save the measurement details

On the gateway dashboard, open **Settings: location and depth**:

- **City, state or ZIP:** required; for example `Tupelo, MS` or `38801`.
- **Water body name:** optional.
- **Measuring:** keep bench/air tests marked as testing; select the correct water measurement type when installed.
- **Probe depth:** enter the depth and units when applicable.

Save. These details apply to subsequent readings; older readings are not relabeled. The transmitter ID is automatic—there is no ID field to fill in. In the display build, **Bench / air test overrides the water body name with “Bench test.”** A water measurement type allows the saved water body name to display.

Finish the [verification checklist](verification.md). See [display instructions](display.md) if using the OLED.
