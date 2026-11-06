# Changelog

## 2026-11-06 — Next experiment recorded

- Recorded remote store-and-forward logging as the next attempt after the current build’s validation, before automatic website reporting.
- Added the visit-and-collect exchange, onboard-flash sizing assumptions, safe acknowledgment/reclaim requirements, timekeeping questions and interruption/power-loss tests.
- Existing hardware is the first test candidate; no SD card, RTC or replacement board has been selected as a requirement.
- Planning/documentation only; no firmware behavior changed.

## 2026-11-06 — Dedicated repository foundation

- Added the project README, explicit transmitter/gateway terminology, wiring, Arduino setup and troubleshooting.
- Included both gateway variants in separate folders: no-display revision 6 and SH1106 display revision 8.
- Included the existing nRF52840 transmitter with flash sleep fix revision 2.
- Added data/storage documentation, verification checklist, status/roadmap, solar observations and unresolved radio review.
- Added MIT licensing for original code/documentation, separate branding terms, third-party notices, security policy and contribution guidance.
- Firmware imported without functional changes. This entry is not a newly tested field release.

## 2026-11-05 — Gateway display revision 8

- Added the supplied centered logo without accompanying text and centered OLED text layouts.
- Added continuous probe / time-weather / dashboard / logo cycling; removed the one-minute display timeout.
- Retained short USER display toggle and five-second hold for Wi-Fi setup.
- Added optional queued background weather lookup using the saved city/state or ZIP, with NTP/local clock handling and stale/unavailable indications.
- Kept CSV schema, local storage, deployment history, Wi-Fi settings, SD-disabled state and cloud-off behavior.
- Compiled for XIAO_ESP32S3 on ESP32 core 3.3.2 with RadioLib 7.1.2, U8g2 2.36.17 and ArduinoJson 7.4.2: **1,282,831 bytes program (38%)**, **51,120 bytes static RAM (15%)**. Runtime TLS/task allocation is additional.
- Live API checks returned Tupelo, MS / 38801 and weather/time-offset data. Full hardware/field validation remains open.

## 2026-11-03 — Seeed baseline download set

- No-display XIAO ESP32S3 gateway revision 6 with local logging, Wi-Fi setup, deployment metadata, graphs and radio diagnostics.
- nRF52840 transmitter with factory-derived ID, 15-minute cadence, sensor-quality reporting and flash sleep fix revision 2 (source dated November 2).
- Bench reception/logging and home Wi-Fi reconnection reported working. These imports have not been freshly compiled in the November 6 documentation session.
