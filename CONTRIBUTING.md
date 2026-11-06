# Contributing

Start with the [README](README.md), [current status](docs/status.md), and [radio review](docs/radio-status.md). Use **transmitter** for the battery/probe unit and **gateway** for the LoRa-to-Wi-Fi unit. Existing `receiver` sketch names are intentional.

Before changing code, describe the concrete problem, evidence and smallest useful change. Keep the display/no-display variants explicit and do not mix their Arduino folders. Avoid unrelated refactors and hardcoded device IDs, locations or credentials.

For a pull request, record:

- The affected sketch and resulting behavior.
- Exact board package, library versions and IDE settings used to compile.
- Tests actually performed, including relevant hardware/power/antenna details.
- Data format/storage compatibility, expected data-loss boundaries and known limits.
- Any regression risk to radio reception, Wi-Fi setup, logging, button behavior or optional display.

Use [the verification checklist](docs/verification.md) as appropriate to the change. Documentation-only changes need link/content checks, not a full field test. Distinguish compiled, bench-confirmed and field-validated results. Do not label tests passed when they were not run.

Changes to packet format, identity, deployment history, archive/clear behavior, partitioning or storage recovery need an explicit compatibility/migration explanation. Never silently erase an existing nonblank filesystem to hide a mount problem.

Do not call a build FCC compliant without evidence for the exact configuration and integration. Do not derive battery percentage, consumed mAh or guaranteed solar runtime from voltage alone. Keep modeled weather distinct from measured probe temperatures and field observations.

Preserve third-party notices and the separate branding terms. See [SECURITY.md](SECURITY.md) for sensitive reports.
