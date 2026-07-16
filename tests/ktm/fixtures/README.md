# XC2 Contract Fixtures

These fixtures contain only synthetic or redacted XC2 contract data. They are
derived from the approved `xc2-approved-2026-07-16` contract profile and are
used only by offline unit tests.

- VIN and dealer identifiers were replaced.
- Credentials, tokens, VCI serial numbers, network addresses, binaries, and
  firmware are not retained.
- Route and topic provenance is recorded in `manifest.json`.
- Fixture values must remain synthetic when the contract suite is updated.

`manifestRevisionDate` dates this offline manifest only; it is not a global
capture date. Provenance is per fixture:

- `rest/job-progress.json` and `stomp/progress-message.frame` are
  `synthetic-contract-fixture` data, have schema authority
  `served-app+patched-backend-jar`, and set `liveCapture` to `false`.
- `stomp/connected.frame` is an `observed-read-only-handshake` from the bounded
  CONNECT/DISCONNECT compatibility observation. It does not imply that a
  progress MESSAGE was captured.
- `stomp/error.frame` is a `synthetic-protocol-fixture` with `liveCapture` set
  to `false`.
