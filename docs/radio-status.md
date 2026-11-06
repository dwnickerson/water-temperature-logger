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

## Manufacturer correspondence — November 4 reply and follow-up

**Updated November 6, 2026.** This is a summary of the email chain supplied by the project owner, not an independently verified certification opinion. The full email, addresses and support-ticket headers are not published here.

### What Seeed support said on November 4

| Topic | Support’s statement | Project status |
|---|---|---|
| Bandwidth | The existing DTS certification did not directly cover our 500 kHz configuration; support said it covered 125 kHz. | **Our 500 kHz profile was not confirmed as covered.** We asked the RF compliance team to identify the applicable DTS/DSS report and tested configuration. |
| Frequency and power | Support said 907.8 MHz had been tested, cited 19.62 dBm peak power, and compared it with the earlier proposed +19 dBm setting. | That comparison addressed the earlier proposal. The running sketch requests **+10 dBm**, and numerical power comparisons alone do not confirm coverage of the complete profile. |
| Antenna | Support identified SKU **318020691 / RFELA-3/58X** as covered and suggested adapting the module’s IPEX connector to the antenna connection. | The filing exhibit, exact cable/connector arrangement, length/loss and installation conditions remain requested. A generic adapter suggestion is not a verified assembly specification. |
| Other radios | Support discussed the XIAO’s separate authorization and disabling Bluetooth or avoiding simultaneous radio operation. | The transmitter firmware does not enable Bluetooth. Exact host/coexistence conditions still need to be documented; this is not a blanket approval of every host arrangement. |
| Host/exposure | Support discussed the battery-powered mobile/fixed case, 20 cm separation and its view of the permissive-change question. | Treat this as support’s response for the stated scenario, not a general definition of Class II permissive changes or a replacement for filed integration requirements. USB/charging, labeling, installation and any additional testing remain part of the clarification request. |

### What I asked in the follow-up

I corrected the current profile to **fixed 907.8 MHz, 500 kHz, SF9, coding rate 4/7, requested +10 dBm**, one report about every 15 minutes, with Bluetooth disabled. The original October 29 proposal used SF12 and +19 dBm; it is not the running profile.

I asked Seeed’s RF compliance team for:

1. The report pages identifying the programmed LoRa bandwidth and measured 6 dB bandwidth at 907.8 MHz, and clarification of whether the “125 kHz” answer referred to DSS hopping tests or DTS tests.
2. Confirmation of coverage for the exact running fixed-channel profile, or a documented covered fixed-channel alternative with frequency, bandwidth, spreading factor, coding rate and power.
3. The filing exhibit covering antenna SKU 318020691, plus the exact permitted cable/connector assembly, length and loss requirements.
4. Host installation, labeling and testing requirements for the battery-powered logger with Bluetooth disabled and the stated separation distance.

The bandwidth question remains open: [§15.247(a)(2)](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15/subpart-C/section-15.247) specifies a minimum measured 6 dB bandwidth of 500 kHz for the digital-modulation provision. A programmed LoRa bandwidth, the support email and a grant frequency range are different pieces of evidence; the report/configuration must be reconciled rather than assuming either profile is authorized.

**As of this update, the supplied chain contains the November 4 support reply and our follow-up, not a final RF compliance-team answer confirming the current profile.** The next step is to obtain those report references and resolve the integration conditions. No firmware/radio settings were changed by this documentation update.

The original change away from Heltec was motivated in part by uncertainty about the applicable authorization. Choosing Seeed hardware alone does not close that review.

## Before describing a deployment as compliant

Identify the exact board/module revision and FCC ID, applicable authorization and test report configuration, permitted antennas/integration conditions, exposure conditions, labeling/instructions and any host evaluation needed. Resolve the fixed-channel/bandwidth issue using authoritative documentation and appropriate qualified review. Record the decision and evidence before calling a reference build deployment-ready.

This repository deliberately makes no claim that either a generic “500 kHz rule” or the manufacturer’s email alone settles all of those questions. Do not use this bench profile as a legal shortcut for field transmission or distribution.

## Reference documents

- [Seeed Wio-SX1262 module grant PDF](https://files.seeedstudio.com/Seeed_Certificate/documents_certificate/113010003-FCC.pdf)
- [47 CFR § 15.247](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15/subpart-C/section-15.247)
- Exact integration instructions, report pages and antenna guidance remain part of the outstanding review; this repository does not invent missing documentation.
