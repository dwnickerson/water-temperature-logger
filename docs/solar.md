# Solar-light power experiment

[Back to README](../README.md) · [Hardware](hardware.md)

The experiment reuses a solar garden light’s enclosure, panel, existing charging circuit and single-cell battery to power the **nRF52840 transmitter**. The gateway stays on USB power. This is a battery supply for the transmitter; the goal is not to charge the light’s battery through XIAO USB.

## What has been observed

Two different lights have been inspected. Their labels both state a 3.7 V, 1500 mAh, 5.55 Wh lithium-ion 18650 cell. These are nameplate values, not measured usable capacity. A cleaner-looking circuit board does not establish better charging or protection.

For the **newer light**, on November 5:

| Time | Measured battery voltage |
|---|---|
| 12:38 | 3.87 V |
| 14:16, after sunlight exposure | 3.96 V |

That is a 0.09 V increase over 98 minutes, consistent with charging. Temperature, load and settling can also affect terminal voltage. No charging-current or mAh result was measured. Earlier readings around 4.02–4.03 V belonged to the other light and must not be combined into this unit’s trend.

## Connection concept

Keep the light’s solar charging circuit attached to its battery. Take transmitter power from the **battery supply**, with correct polarity, to the XIAO’s designated battery connection. The LED load is not needed for this project.

The lamp output is not automatically the same as the battery supply; it may be switched by darkness or the light’s controller. The switch’s effect on charging depends on the actual circuit. Do not assume an unverified carrier VIN pin, LED output, solar-panel output or 3V3 pin is an interchangeable battery input.

Disconnect USB and remove/disconnect the cell before soldering to the battery-holder wiring. Verify the new unit’s polarity and voltage before connecting the transmitter. Keep USB disconnected during the solar/battery test so it does not alter the charging result. This guide does not establish the light controller’s protection functions or charge-temperature limits from photographs.

## Useful test

1. Label the light/unit and record its battery and panel details.
2. Run the transmitter with the LED load disconnected and record how the light’s switch is set.
3. Check battery voltage early each morning at a similar time, with the transmitter connected; note temperature, sunlight/cloud and any resets/missing packets.
4. Compare the gateway’s reported transmitter voltage with a multimeter.
5. Measure actual sleep/transmit current before calculating battery runtime or panel energy margin.

Stable morning voltage over several representative days supports that solar is keeping up during that period. It does not establish winter or all-weather performance. Voltage alone cannot measure consumed mAh or reliably determine battery percentage.
