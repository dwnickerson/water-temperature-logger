# Next experiment: remote store-and-forward logging

**Recorded November 6, 2026. Planned only; not implemented in the current sketches.**

[Back to README](../README.md) · [Current status and order of work](status.md)

## Why I want to try it

After finishing the current gateway, probe, battery/solar and recovery checks, I want to test a transmitter at a remote site with no permanent gateway or Wi-Fi. It would keep measuring and saving locally. A month or so later, I could visit with a portable gateway and collect the readings accumulated since the previous successful visit.

This is the **next firmware experiment after the current build’s validation**, ahead of adding automatic website reporting. It is not a replacement for finishing the current tests. The current transmitter still has no stored log, receive/ACK window or backlog transfer, and the current gateway is LoRa receive-only.

## Intended exchange

1. **Measure, then save.** Each reading is committed to local flash before any attempt to send. Include a stable record identifier, measurement time/time-quality flag, temperature, battery voltage and sensor-quality status.
2. **Announce and briefly listen.** At its scheduled wake, the transmitter advertises its identity and available record range, then opens a short receive window. With no gateway response, it retains the records and sleeps.
3. **Request missing records.** On a visit, the gateway identifies the records it already holds and requests the missing range. First contact may take up to one normal reporting interval.
4. **Send bounded batches.** The transmitter sends a batch with identifiers and an integrity check. The gateway validates it, saves it durably and verifies the saved result before acknowledging it.
5. **Resume after interruption.** Both sides retain enough progress to recover after a reset, lost ACK, power failure or lost radio link. Replayed records are recognized rather than imported twice.
6. **Confirm and reclaim.** The gateway confirms a specific continuous range that is safely stored. Only those confirmed records become eligible for reclaim on the transmitter; later or unconfirmed readings stay intact. The transmitter then returns to sleep.

The final handshake must not be a vague “clear everything” command. If the last confirmation is lost, retaining/replaying records is safer than deleting them without evidence of durable receipt. Progress and reclaim boundaries must themselves survive power loss. An accepted backlog should remain available for export from the gateway after its own restart.

## Hardware first approach

Start with the **existing XIAO nRF52840 transmitter and ESP32S3 gateway**. The nRF52840 board has nominally 2 MB of external QSPI flash, separate from its internal program flash. The tested unit reported JEDEC `0x856015`; the present firmware only identifies that device and puts it to sleep. Reliable read/write/erase, usable capacity, recovery and sleep-current behavior have not yet been validated for logging.

An SD card or different transmitter board is **not the initial requirement**. Validate the onboard flash first. The gateway’s own free storage must also be checked before accepting a backlog, especially when collecting from multiple units.

Illustrative compact-record sizing at one reading every 15 minutes:

| Uncollected period | Records | Raw bytes at an assumed 32 bytes/record |
|---|---:|---:|
| 30 days | 2,880 | 92,160 (90 KiB) |
| 90 days | 8,640 | 276,480 (270 KiB) |
| 365 days | 35,040 | 1,121,280 (1.07 MiB) |

These are planning calculations, not an implemented format or measured retention guarantee. Allow room for metadata, checksums, erase-block management, recovery records, filesystem overhead and safety reserves. Define a visible full-storage policy before a remote trial; do not silently overwrite uncollected readings.

[Seeed board storage reference](https://wiki.seeedstudio.com/XIAO_BLE/)

## Decisions to test before a long deployment

- **Timekeeping:** A month-old measurement must keep its measurement time, not its download time. Establish UTC time before deployment, quantify drift, and explicitly handle resets/complete power loss and unknown-time records. A battery-backed RTC may be useful; it has not been selected or required yet.
- **Power:** Measure the added cost of flash writes, brief receive windows, retries and backlog transfer. Return the flash/radio to low-power states between events. Do not infer runtime from the storage calculation or voltage alone.
- **Durability:** Use versioned records and integrity checks, power-loss-safe append/recovery, durable gateway commits, duplicate handling and restart-safe transfer progress.
- **Deletion authority:** Current radio messages are unauthenticated. An arbitrary or spoofed ACK must not authorize deletion. Define how the field unit recognizes an approved collector and how a user recovers if a collector is replaced.
- **Transfer limits:** Set batch size, bounded retries, timeout and energy limits. Preserve pending records if the visit ends early. Measure actual download time and progress instead of promising an instant month-long transfer.
- **Compatibility:** Keep the present always-listening-gateway operation usable, or document an explicit protocol version/migration. Preserve transmitter identity and deployment attribution; do not attach newly entered settings retroactively to a month of unknown historical conditions.
- **Radio integration:** This experiment makes the gateway transmit ACKs and the transmitter receive, unlike the present one-way link. Revisit the exact radio configuration, antenna and host-integration review before field operation. This plan is not a compliance conclusion.

## Incremental test sequence

1. Finish the current build’s validation and preserve its working firmware.
2. Test a small local flash log: write/read/verify records, reset during writes, recover without losing earlier committed records, and measure current.
3. Transfer a small batch to a gateway that saves before acknowledging.
4. Interrupt power/link/ACKs at different points and prove restart, replay and duplicate handling.
5. Test confirmed-range reclaim, unknown/unauthorized confirmations, and full-storage behavior.
6. Establish clock accuracy and reboot/power-loss behavior.
7. Generate and collect a month-equivalent backlog; verify every expected record by identity, count and integrity checks, including gateway restart/export.
8. Run a controlled remote/solar trial and record runtime, retention, time error, download duration and gaps.

Only after those results should this become a supported remote deployment mode. Automatic website delivery and any model use remain separate later steps.
