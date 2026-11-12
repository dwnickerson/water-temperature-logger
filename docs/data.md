# Radio packets, logs and storage

[Back to README](../README.md)

## Radio packet version 1

A transmitter sends seven comma-separated fields:

```text
version,node_id,boot_id,sequence,temp_f,batt_v,quality
```

| Field | Meaning |
|---|---|
| `version` | `1` |
| `node_id` | 16 hexadecimal characters derived from the nRF52840 factory identifier |
| `boot_id` | Random decimal identifier for this transmitter startup |
| `sequence` | Incremented report counter within that startup |
| `temp_f` | Probe temperature in °F; blank on a sensor error |
| `batt_v` | Measured transmitter battery voltage in volts |
| `quality` | `ok` or `sensor_error` |

The ID is an identifier, not authentication. The radio uses private-sync-word point-to-point LoRa with CRC, not encrypted/authenticated application packets. The transmitter has no clock timestamp in the packet, stored backlog, acknowledgment or automatic resend. The local `TX result 0` message does not prove delivery.

## Current CSV schema

Both gateway variants use the same 19-column `log-v4.csv` schema:

```csv
timestamp_utc,temp_f,rssi,batt_v,time_valid,uptime_ms,node_id,boot_id,sequence,quality,deployment_revision,water_body_name,water_type,latitude,longitude,probe_depth_m,deployment_effective_utc,location_name,location_basis
```

| Fields | Meaning |
|---|---|
| `timestamp_utc`, `time_valid` | Gateway receipt time in UTC after internet time sync; pre-sync records use `no-time` and `0` |
| `temp_f`, `quality` | Probe temperature and quality flag; do not treat error rows as valid temperature observations |
| `rssi` | Received packet RSSI in dBm; SNR is live only and is not stored in this CSV |
| `batt_v` | Transmitter battery voltage; not calibrated battery percentage or consumed mAh |
| `uptime_ms` | Gateway uptime at receipt; not transmitter sample time |
| `node_id`, `boot_id`, `sequence` | Transmitter identity and report/run identifiers |
| `deployment_revision` | Saved deployment configuration attached to that row |
| `water_body_name`, `water_type` | Optional water body name and saved measurement type |
| `probe_depth_m` | Depth in meters; feet inputs are converted before storage |
| `deployment_effective_utc` | Effective time recorded for that saved settings revision; may be `no-time` if unsynchronized |
| `location_name`, `location_basis` | City/state or ZIP as entered; basis is `city_state_or_zip` |
| `latitude`, `longitude` | Compatibility columns from earlier schema; not entered in the current UI and normally blank |

City/state or ZIP describes an approximate location. It is not automatically replaced by coordinates in the CSV. Display weather geocoding stays separate from measurement metadata.

Saving deployment changes records a new history entry. New readings receive the new details; existing readings remain unchanged. Settings history can be downloaded separately. Older imported records retain blanks for metadata that was never recorded. Migration preserves earlier files rather than inventing missing information.

## Internal storage

The documented gateway partition provides **1.5 MiB** for LittleFS. This is separate from program space. Actual free log space is lower after filesystem overhead, settings history, logs and archives. Weather is cached in RAM and does not consume log space.

SD is disabled in both current gateway sketches. Older Heltec SD assignments are not verified Seeed wiring instructions.

Use **Save log** before **Clear current log**. Clearing affects the current log on active storage; it does not erase archived logs or deployment history. Archives are preserved, so free space still needs monitoring and management. There is no indefinite storage guarantee or fixed number of days without accounting for row length, history and archives. Download important data regularly.

Graphs show the current log with 24-hour, seven-day and all-current-log views. Archived files are accessed separately. A browser shows timestamped points in its own timezone.

## Interpreting diagnostics

RSSI and SNR describe packets that arrived; they do not establish delivery rate or distance. Sequence-gap diagnostics are grouped by transmitter and boot ID, between observed sequence numbers. They cannot count missed reports before the first/after the latest observed report or across resets. Receiver downtime and failed saves can also produce apparent gaps.

The gateway may receive packets from more than one transmitter, but there is only **one deployment configuration per gateway** in this revision. Do not describe it as a finished multi-probe metadata or dashboard system.

## Probe calibration (unreleased)

In **Settings → Probe calibration**, wait for an identified probe to report. Compare
the probe with a reference thermometer in the same stirred water after both settle.
Enter the reference temperature minus the probe temperature in °F (for example,
`-1.2` if the probe reads 1.2 °F high). The receiver saves one probe offset; saving
for another probe replaces it. Set it to `0` to turn correction off.

The v5 CSV retains the original 19 columns and adds `temp_raw_f` and
`calibration_offset_f`. `temp_f` is the value after correction and drives the
dashboard and OLED. Old rows keep their original values and have blank new
fields. Sensor-fault rows have no temperature or offset. The firmware retains the
v4 file as a separate backup and needs enough free flash to copy it. Download the
current CSV before updating, keep erase disabled, and verify logging after upload.
