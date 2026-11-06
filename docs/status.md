# Project status — November 6, 2026

[Back to README](../README.md)

## What I have running

I have moved from the original Heltec V3 prototype to a Seeed XIAO nRF52840 transmitter and XIAO ESP32S3 gateway, each with its matching Wio-SX1262 carrier. The transmitter reads one DS18B20 and sends about every 15 minutes. The gateway saves reports internally and serves a local dashboard over Wi-Fi.

The gateway is a custom LoRa-to-Wi-Fi gateway, not a LoRaWAN gateway. The `receiver` filenames are retained to avoid disrupting Arduino sketch names.

## Evidence and limits

| Area | Evidence available | Still to establish |
|---|---|---|
| Probe and radio | User-confirmed DS18B20 readings and successful gateway saves | Calibration and representative installed-range performance |
| Gateway baseline | Wi-Fi setup, reconnection after power loss, dashboard and CSV confirmed during bench use | Long-duration retention/outage/low-storage tests on the deployed build |
| Transmitter identity | Factory-derived 16-character ID appears in reports | Fleet provisioning/ownership workflow |
| External flash sleep | Tested board reported JEDEC `0x856015`; sleep command sent and QSPI released | Actual battery-only idle current and runtime |
| No-display gateway | Original Seeed revision 6 baseline included | New repository validation results recorded in the changelog/checklist |
| Display gateway | Revision 8 compiled November 5; user subsequently reported the “Bench test” label on the screen | Complete logo/cycle/button/weather behavior and radio reception during TLS requests |
| Weather | Live API checks resolved Tupelo, MS and ZIP 38801 and returned weather/time offset | Device TLS/heap/offline behavior; appropriate service arrangement for rollout |
| Solar experiment | New light’s battery rose from 3.87 V at 12:38 to 3.96 V at 14:16 on November 5 after sunlight exposure | Charging current, energy balance with transmitter attached, cloudy-day performance and seasonal runtime |
| Radio authorization | Seeed’s November 4 reply said the 500 kHz profile was not directly covered and identified antenna SKU 318020691; an RF-team follow-up requested report pages, exact profile, antenna/cable and host conditions | Final RF-team clarification and documented authorization/integration path for the exact finished build |
| Cloud reporting | Planned; absent/disabled in current firmware | Authenticated ingestion, buffering/recovery, validation and model eligibility |

The solar voltage change is consistent with charging, but it is not a measurement of mAh added or proof of sustainable operation. USB-powered voltage reports are not battery-discharge tests.

See the [dated manufacturer-response summary](radio-status.md#manufacturer-correspondence--november-4-reply-and-follow-up) for what Seeed answered and what is still awaiting clarification. Receipt of a support reply is not confirmation of the running radio profile.

## Next steps in order

1. Resolve the radio configuration, approved antenna and host-integration questions for the reference build. See [radio status](radio-status.md).
2. Finish gateway revision 8 checks: settings/log retention, display controls, weather/offline behavior and reception while weather updates.
3. Calibrate/check the probe against an appropriate reference and keep bench/air data clearly marked.
4. Measure battery-only transmitter current and track morning battery voltage during the solar trial, including cloudy periods.
5. Test weather protection, installation, range, restarts, router outages and storage limits.
6. **Next experiment after the current validation: remote store-and-forward.** Log on the transmitter’s onboard flash with no permanent gateway; collect missing records during a site visit using verified batches and safe acknowledgments. Start with existing hardware and test timekeeping, power loss, interrupted downloads and confirmed-record reclaim. See the [experiment plan](store-and-forward.md). This is not implemented yet.
7. Add private, authenticated website reporting in stages. Keep test readings separate from accepted field observations.
8. Consider model use only after measurement quality, metadata, continuity and validation support it.

There is no measured “runs forever” solar result, guaranteed field battery life, finished national fleet management, or completed FCC compliance claim.

## Firmware provenance

- Display gateway revision 8: the exact sketch delivered in `Bluegill-receiver-revision-8-SH1106.zip` on November 5, 2026; no functional changes made for this repository import.
- No-display gateway revision 6: imported from the previous project download dated November 3, 2026. Original Git blob: `c85ac014cb597523d591538e07ed9520f67e1663`.
- Transmitter flash sleep fix revision 2: imported from that same download set. Original Git blob: `f60c907d039d2ebf17db9b884beae6233e155252`.

The previous journal/downloads were hosted with [The Southern Bluegill project page](https://thesouthernbluegill.com/observations/projects/water-temperature-logger/). This dedicated repository now groups the current Seeed firmware and documentation. Historical Heltec firmware is not a current build target here.
