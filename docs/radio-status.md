# Radio configuration and open review

[Back to README](../README.md)

**Status: unresolved for the finished build.** These files document the running bench profile. They do not certify a device, establish that a particular transmission is authorized, or provide a ready-to-deploy FCC configuration.

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

Reported/software settings are not independently measured occupied bandwidth, radiated power, PSD, or emissions results. Reducing the power setting or selecting a nominal bandwidth does not by itself establish compliance with a module grant and host requirements.

## Manufacturer correspondence

In November 2026, Seeed support stated that the module’s existing DTS certification covered 125 kHz rather than the project’s 500 kHz configuration. That response does **not** confirm this 500 kHz build is covered. Follow-up clarification was requested.

Support also stated that antenna SKU 318020691 was covered and discussed radio coexistence and mobile/fixed exposure conditions. These statements still need reconciliation with the actual grant, test reports, integration instructions, antenna type/gain/cable and completed host configuration. Do not treat that correspondence as finished-system approval or extrapolate a chip specification to a board authorization.

The original change away from Heltec was motivated in part by uncertainty about the applicable authorization. Choosing Seeed hardware alone does not close that review.

## Before describing a deployment as compliant

Identify the exact board/module revision and FCC ID, applicable authorization and test report configuration, permitted antennas/integration conditions, exposure conditions, labeling/instructions and any host evaluation needed. Resolve the fixed-channel/bandwidth issue using authoritative documentation and appropriate qualified review. Record the decision and evidence before calling a reference build deployment-ready.

This repository deliberately makes no claim that either a generic “500 kHz rule” or the manufacturer’s email alone settles all of those questions. Do not use this bench profile as a legal shortcut for field transmission or distribution.

## Reference documents

- [Seeed Wio-SX1262 module grant PDF](https://files.seeedstudio.com/Seeed_Certificate/documents_certificate/113010003-FCC.pdf)
- [47 CFR § 15.247](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15/subpart-C/section-15.247)
- Exact integration instructions, report pages and antenna guidance remain part of the outstanding review; this repository does not invent missing documentation.
