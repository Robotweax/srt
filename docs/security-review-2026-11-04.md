# Security source review — 2026-11-04

The maintainer supplied a completed Codex Security whole-repository review of
commit `cdacfcb58330e3a9b172f9fa32ddfbfe9c3c857f`, completed at
06:46 UTC on 2026-11-04. All 735 inventoried files were in scope, with no path
exclusions. The report records independent source-level validation of candidate
findings and their attacker reachability.

Reviewed surfaces include cryptography, key-material negotiation and rotation,
authenticated payload handling, secret lifecycle, groups, multicast membership,
reliability, FEC, public APIs, tooling and release workflows. No reportable
cryptographic or multicast issue was established. This completes the previously
pending source review of the integrated multicast design.

One medium-severity finding concerned accept-capacity enforcement after expensive
plaintext listener setup. [PR #376](https://github.com/Robotweax/srt/pull/376)
addresses that finding by reserving capacity before allocation and setup, with
failure-path and group-admission regressions.

The review was static: no build, network proof of concept, runtime benchmark or
external-service check was performed. Operating systems, cryptographic dependency
implementations, deployment settings and external signing controls were outside
its independent audit scope. The report does not establish cryptographic
revocation on multicast leave or sender identity among shared-key holders; those
limits remain in the [encryption contract](encryption.md#multicast-dispatch-groups-one-traffic-key-per-group).

The 0.3.2 multicast empty-interval key-progression correction and transactional
key replacement follow this reviewed commit. Their regression evidence is
separate from the scan: retained-key/randomness failure tests, sequence-wrap
tests, and repeated receiver departure/rejoin with plaintext, CTR and GCM.
Physical-network multicast scaling remains a separate qualification activity.
