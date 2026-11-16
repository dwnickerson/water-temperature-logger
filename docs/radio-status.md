# Radio configuration and FCC module grant

[Back to README](../README.md)

**Status as of November 16, 2026.** Seeed confirmed coverage of the running fixed-channel profile under the Wio-SX1262 DTS module grant. Verification of the assembled logger is still in progress.

## What the sketches actually request

| Parameter | Current value |
|---|---|
| Link | Custom point-to-point LoRa; not LoRaWAN/Meshtastic |
| Frequency | 907.8 MHz |
| Bandwidth | 500 kHz |
| Spreading factor | SF9 |
| Coding rate | 4/7 |
| Transmitter power setting | +10 dBm requested by software |
| Frequency hopping | Not implemented |
| Application security | No encryption/authentication; CRC and IDs are not authentication |
| Gateway LoRa behavior | Receive only; no ACK, relay or LoRa transmit |

## Seeed’s DTS clarification

Seeed support confirmed that the fixed-channel profile above falls within the DTS grant envelope for **FCC ID Z4T-WIO-SX1262**. The 125 kHz figure in its earlier reply came from the DSS frequency-hopping report **BL-SZ2480665-601**. DTS report **BL-SZ2480665-602** documents the 500 kHz mode. Seeed said 907.8 MHz was one of three channels tested and reported a **645.020 kHz measured 6 dB bandwidth** on that channel. These results describe the module test. The transmitter sketch requests +10 dBm and leaves Bluetooth disabled.

## Assembled logger checks

- **Deployed antenna.** The field transmitter uses the small flexible antenna supplied with the Seeed unit, mounted inside the housing. The [FCC DTS test report (BL-SZ2480665-602)](https://fcc.report/FCC-ID/z4twiosx1262/7738781.pdf) lists a 2.36 dBi flexible printed-circuit (FPC) antenna. The supplied antenna’s exact part number and match to the filing are unverified. Seeed separately identified SKU **318020691 / RFELA-3/58X**, a 5.8 dBi fiberglass kit with an N-type male connector and a 1 m CFD-200 cable.
- **Label and installation.** If the module label is hidden, label the exterior **Contains FCC ID: Z4T-WIO-SX1262**. Follow the grant conditions, including at least 20 cm separation from people and its antenna co-location and simultaneous-operation restrictions.
- **Host emissions.** Seeed advised verifying the finished host as a digital device under Part 15B and recommended checking radiated spurious emissions with the actual enclosure and antenna installed.

## References

- [Seeed Wio-SX1262 DTS module grant](https://files.seeedstudio.com/Seeed_Certificate/documents_certificate/113010003-FCC.pdf)
- [Filed module integration instructions](https://fccid.io/Z4T-WIO-SX1262/User-Manual/Users-Manual-7738695.pdf)
- [47 CFR § 15.212](https://www.ecfr.gov/current/title-47/section-15.212) and [§ 15.247](https://www.ecfr.gov/current/title-47/section-15.247)
