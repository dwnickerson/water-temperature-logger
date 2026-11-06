# Security policy and current boundaries

This is a bench-stage device project, not a hardened hosted service.

## Local access

- The gateway serves HTTP on the local network without a dashboard login or HTTPS.
- Anyone with suitable local access may view readings and access available management functions. Request tokens and confirmations are not a substitute for user authentication.
- The setup hotspot is intentionally open during first setup or after a physical USER hold. Your home router still uses its own Wi-Fi password. Setup closes after success/timeout and does not automatically reopen during a router outage.
- Keep the gateway on a trusted network. Do not port-forward its web server or expose it directly to the public internet. Use a suitable isolated network where appropriate.
- Physical access and firmware access are outside the current access-control boundary. Saved Wi-Fi credentials must not be treated as tamper-proof storage.

## Radio and data

Application packets are not encrypted or authenticated. Factory IDs, CRC and sequence numbers help identify/decode reports but do not prove who sent them. A nearby compatible sender may imitate an ID or inject data. Sequence-gap diagnostics are not an integrity/security guarantee.

Cloud ingestion, device enrollment, server authentication, ownership transfer and authenticated probe data are future work. Do not add API keys, Wi-Fi passwords, real precise locations or private logs to this public repository.

The display weather client checks HTTPS certificates and requires valid time. Weather is optional and separate from probe ingestion; certificate or endpoint failures must not be worked around by disabling TLS validation.

## Reporting a vulnerability

Use GitHub’s private **Report a vulnerability** option if it is available for this repository. Otherwise open an issue requesting a private contact route without publishing credentials, an exploit, personal locations or sensitive details. Do not assume private reporting has been enabled merely because this policy exists.

Include the affected sketch/commit, hardware, relevant configuration, impact and minimal reproduction steps. Remove passwords, service keys, network identifiers and private data from logs/screenshots before sharing them.
