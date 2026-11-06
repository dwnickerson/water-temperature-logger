# Bench and field verification

[Back to README](../README.md) · [Status](status.md)

Copy this checklist into a test record or GitHub issue. Record the date, firmware commit, board/carrier revision, antenna, power source, probe, measurement type and location. Keep bench/air tests distinct from accepted water observations.

## First boot and local logging

- [ ] Correct gateway variant and board selected; expected Serial banner appears.
- [ ] Log initialization succeeds; no unexpected format/erase occurred.
- [ ] Wi-Fi setup works from a phone and closes after success or Close setup.
- [ ] Home Wi-Fi reconnects after a gateway-only restart; old readings/settings remain.
- [ ] One probe is detected; factory transmitter ID is stable across restarts.
- [ ] Two consecutive transmitter reports arrive about 15 minutes apart and save successfully.
- [ ] Exported CSV has the expected header, units, identifiers, quality and receipt timestamps.
- [ ] A transmitter restart starts a new boot/sequence run and is not treated as one continuous sequence.
- [ ] New deployment details appear in subsequent CSV rows; earlier rows remain unchanged.
- [ ] Clear current log requires confirmation; exported backups, archives and settings history are handled as documented.

## Display build only

- [ ] Logo appears alone and centered; all text is centered/readable.
- [ ] Probe, clock/weather, address and logo cycle; screen stays on beyond one minute.
- [ ] Short USER toggles display; five-second hold reopens setup without clearing logs.
- [ ] Bench/air mode says Bench test; water mode can show the saved water body name.
- [ ] Current dashboard IP appears after Wi-Fi connects and agrees with the browser address.
- [ ] City/state or ZIP resolves to the intended place; Serial reports the resolved place.
- [ ] Clock timezone and air-weather labels are correct; stale/unavailable data is distinguishable.
- [ ] Radio reports continue during weather/TLS requests and internet loss.
- [ ] Runtime heap remains adequate across repeated successful/failed weather requests and restarts.

The display build uses a 35-minute OLED overdue threshold. Web gap/staleness diagnostics have their own documented logic; do not infer delivery guarantees from either indicator.

## Reliability and measurement checks

- [ ] Compare probe readings against an appropriate reference over the intended temperature range; record residuals and uncertainty.
- [ ] Check transmitter voltage against a multimeter, with power source recorded.
- [ ] Measure battery-only idle/transmit current; do not derive consumed mAh from voltage alone.
- [ ] Test probe disconnection/sensor_error and reconnect without silently accepting invalid temperature.
- [ ] Exercise gateway power interruptions, router outages, no internet and low storage; confirm data-loss boundaries.
- [ ] Verify installed range with sequence continuity, not RSSI alone.
- [ ] Test enclosure/seals, cable entry/strain relief, condensation, battery temperature and representative weather.
- [ ] Track solar morning voltage and report continuity through cloudy periods; keep results separate for each light/unit.
- [ ] Close the documented radio authorization/integration review before representing a field build as compliant.

A successful compile is not proof of hardware operation. A successful radio transmit return is not proof of receipt. A few clear-day solar results are not a year-round energy budget.
