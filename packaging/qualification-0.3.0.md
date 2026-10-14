# Package qualification 0.3.0

Status: **candidate finite transfers passed; comparative resource exception approved; remaining release gates pending**. Nothing is published.
Project version **0.3.0**, shared-library ABI line **0.3**, compatible SRT API
**1.5.7**. Existing applications and dependencies must rebuild for ABI line 0.3.
Previous [0.2.9 acceptance](qualification-0.2.9.md) remains unchanged.

## Current pacing-corrected product

- Product commit: `607d0034b7977f1e5003c98c42cc934bbdf9d431`.
- Downloaded public archive: `https://github.com/Robotweax/srt/archive/607d0034b7977f1e5003c98c42cc934bbdf9d431.tar.gz`.
- Bytes: 1948295.
- SHA-256: `6c6d19458838248a6c95153e8907334e6c16826c041d4e204f57b73a1750cb5c`.
- SHA-512: `d7c4914b5f1fbcf0006a67f1cc7ecbdecce88f712c615c294a2d70cfb14be9fe36a6f12e1ee374335d84a325885371e04fec8fc9c69427134a4845542b4aca0f`.
- All 643 tracked regular files and executable bits match Git.
- All package recipe pins select this archive; no trace instrumentation is included.

Static Debug CTest passed **69/69** and shared Release passed **20/20**.
The new fallback regressions fail against the preceding controller. Changed-line
formatting, DCO and installed documentation checks pass. Hosted CI, integrations,
providers and packages need renewed acceptance. A new ten-case File/Stream cohort
retains the original workload, 60-second deadline and thresholds. The corrected
candidate passed 5/5; published 0.2.9 passed 2/5. All ten cases were independently
validated, including the three failed baseline cases. Only two pairs completed,
so comparative resource acceptance remains **false**. Earlier results below
remain historical. See [the pacing and qualification record](pacing-0.3.0.json).

## Previous capability-corrected product and archive verification

- Product commit: `bc7e39cc942bbd129ff55b1c014edbe563be266d`.
- Downloaded public archive: `https://github.com/Robotweax/srt/archive/bc7e39cc942bbd129ff55b1c014edbe563be266d.tar.gz`.
- Bytes: 1946515.
- SHA-256: `2fa4db10dc3f74b6598e869907a676dbdfc13bfb0f424c8727a7cc55d693f8bd`.
- SHA-512: `6c21e67b1f2dc9811d995c10f747e145f3418578abfc20c93f9293f829dae5b0363399356c4c13812f75ba8d549dec6ba3ed97a6526372ebb239e0b4aee3206f`.
- All 643 tracked regular files and executable bits match Git.
- Homebrew, vcpkg, Fedora and Linux qualification inputs pin this archive.

Local static Debug CTest passed **69/69**, and shared Release passed **20/20**,
including exports and installed C/C++ consumers. The three new regressions also
reject an intentional mutation restoring discarded retransmission capability.
Documentation (82 files), changed-line formatting and DCO checks passed.

Hosted source/provider/ACP1 CI, ecosystem integrations, package qualification,
Windows SDK and resource acceptance for this corrected product are **pending**.
Previous results below belong to the pre-fix source and do not accept this
archive. Signing, tagging and publication remain on hold. No claim is made
that this opt-in reorder correction fixes default-bound startup pacing.

## Previous frozen product and archive verification

- Product commit: `1e7d7824a0420dac8e8ccaa950668ee301f99b9f`.
- Downloaded public archive: `https://github.com/Robotweax/srt/archive/1e7d7824a0420dac8e8ccaa950668ee301f99b9f.tar.gz`.
- Bytes: 1931583.
- SHA-256: `046813c64242afec5fc1393790f04094cb4c8eecb3a769b49eed77632046901b`.
- SHA-512: `d8de39577bb61ce5ec6a934e6596c4e0735dd6e38870b40903ad95378a1f3bb8990d97dcd9305681c7faa9ed7878f03ed8d74fc32c8be171d90b496088e5d088`.
- All 639 tracked regular files and executable bits match Git.
- The frozen 0.3.0 export inventory contains 84 symbols and preserves
  every prior release's declared C exports.

The archive is downloaded GitHub content, not a locally generated substitute.
Later recipe/evidence commits must preserve `src`, `include`, `cmake`, `compat`
and `CMakeLists.txt` relative to the frozen product. Product changes require a
new pin and repeated package qualification. Recipe inputs point to this exact
archive rather than a moving branch or an uncreated release tag.

## Previous candidate acceptance (before capability correction)

| Gate | State | Evidence required |
| --- | --- | --- |
| Local static Debug and shared Release | Passed | Frozen product `1e7d7824a`: 69/69 static and 20/20 shared checks; includes exact 84 exports, installed C/C++ consumers, ABI 0.3 and installed documentation |
| Full source CI | Passed | [CI](https://github.com/Robotweax/srt/actions/runs/38565496566): 46 successful jobs on `a09b2cb30`, including Required CI, platform Debug/Release, sanitizers, fuzz, Mbed TLS and reference interoperability |
| Complete ecosystem integrations | Passed | [Release integrations](https://github.com/Robotweax/srt/actions/runs/38565513121): every selected FFmpeg/GStreamer/VLC/OBS platform and desktop profile and Required CI passed on `a09b2cb30` |
| Homebrew and six vcpkg profiles | Passed | [Package managers](https://github.com/Robotweax/srt/actions/runs/38565496678): all seven jobs passed on `a09b2cb30` |
| Ubuntu 24.04 / Fedora 44 | Passed | [Linux packages](https://github.com/Robotweax/srt/actions/runs/38565496740): both pinned-source package and consumer/coexistence/removal jobs passed |
| Windows SDK | Passed, unsigned candidate | [Windows SDK installers](https://github.com/Robotweax/srt/actions/runs/38565496487): twelve variants, both installers, real signed-0.2.9 upgrades, repair/rejection/coexistence and independent reconstruction passed; 0.3.0 signing remains separate |
| Protected signing | Not started | Main-only approved independent rebuild, trusted publisher/timestamp and exact signed-byte tests |
| Native Homebrew bottle | Not started | Separate tap review, native qualification and downloaded digest verification |
| Resource acceptance | Failed; both soaks passed | Original 38/40 resource cases; additional finite File/Stream cohort also has 60-second deadline failures. All cases and original thresholds retained; no failed case replaced |
| Available security findings | Reviewed | Prior finding corrected in frozen source; version/ABI delta reviewed; no new full Security Cloud scan or independent ACP1 review claimed |

Local acceptance passed 806 interoperability Python tests through CTest; the
subsequent full suite passed 810 after four FFmpeg linkage-guard regressions,
17 tooling tests, six API inventory checks, AEAD contract checks, all 82 installed
Markdown documents, pinned clang-format 22.1.8, action pin policy, shell/Ruby
syntax and whitespace checks. These local macOS results do not replace the
separate protected signing and final publication gates.

## Upgrade fixture provenance

The downloaded published signed 0.2.9 installer bytes were independently hashed
before updating the 0.3.0 upgrade fixtures:

| Installer | SHA-256 |
| --- | --- |
| OpenSSL | `9cc066dfee3278319d552bbfe47d48b39c285b7512f7fd836fad00241216cdd1` |
| BCrypt | `d063d68f4ae4d977b31ef129947c78bc3abc2947ae3e94a5ec28abfce0117c04` |

Windows CI must additionally verify their Authenticode signatures and execute
the real upgrade. Download/hash verification on macOS is not upgrade acceptance.

OpenSSL remains the package-manager default. Mbed TLS is a separately selected
provider; ACP1 remains experimental and disabled in normal packages. No new
PPA, COPR, registry, bottle or application distribution is implied. The
[candidate notes](../docs/release-notes-0.3.0.md) and
[release process](../docs/release-process-0.3.0.md) preserve the exact limits.

## Superseded preliminary freeze

Preliminary source `526ad4f32c80dbe999ec758350118d1f33082266` and its archive are excluded from acceptance.
Its first local runs passed 67/69 static and 18/20 shared checks; installed
consumer examples and a release-contract test still requested version 0.2.
The corrected source pin above includes those fixture updates. Original logs
and downloaded bytes are retained; the initial results are not passing evidence.

## Accepted candidate and remaining boundaries

All seven hosted workflows passed at immutable candidate
`a09b2cb30aae91e3d95d578ccdfa1f6f176db06e`. Recipe/evidence-only changes after
this point must preserve the frozen product identity. The full source workflow
intentionally excludes media jobs; the separate successful release-integration
run supplies those profiles. Optional Live-timing diagnostics were not selected.

[Experimental ACP1](https://github.com/Robotweax/srt/actions/runs/38565496889) passed on Linux and macOS, including enabled
mode partitions and cross-provider coverage. This is experimental test evidence,
not an independent cryptographic review or a supported-security qualification.
[Experimental BCrypt backend](https://github.com/Robotweax/srt/actions/runs/38565496716) passed its five routine jobs. Native
ARM64 runtime and serial Windows performance diagnostics were not selected;
ARM64 compilation does not establish native ARM64 runtime acceptance.

The PR SDK workflow intentionally skipped protected rebuild/sign/draft jobs.
Unsigned-candidate success does not qualify signed 0.3.0 installer bytes.

## Available-finding security delta review

The Standard Security Cloud scan at `3119aa208` had partial static coverage and
reported one low-severity SSH interoperability descendant-cleanup finding.
Commit `1c661e0d0` corrects it and is included in frozen source `1e7d7824a`.
Four real-process stop/disconnect regressions cover an exited leader and a child
ignoring SIGTERM; stale-identity, idempotence and Darwin compatibility/error
checks also pass in local/hosted suites.

Relative to that scan, `src` and `compat` are unchanged. Library-facing changes
are the project/header release version and the unchanged 84-symbol ABI snapshot.
The separate tooling correction retains an unreaped child identity through
final group signals, escalates independently of leader exit and makes repeated
stops harmless. The available reported finding is addressed. No new whole-tree
scan is claimed. ACP1 remains unreviewed and off by default; #112/#190 remain
open, and process groups remain unsuitable as adversarial-binary containment.

## Selected resource and finite-transfer evidence

The [compact resource record](performance-0.3.0.json) retains both separately
declared protocols, source/binary/archive hashes and every failed verdict. The
original cohort completed 40 resource cases: 38 passed, while one published
0.2.9 case and one candidate 0.3.0 case each missed the final 1316-byte Live
payload under loss. Live does not guarantee that final-packet recovery. The
three clean profiles passed all five pairs and the predeclared throughput,
CPU-per-Gbit and RSS thresholds; the lossy Live profile remains failed.

Both 32-stream, 300-second encrypted soaks passed, each transferring exactly
6,000,033,536 bytes with both peer exits zero. They cover CTR and the explicit
GCM/FEC/SESSIONAUTH profile with key refresh 10000 / preannounce 1000.

The additional File/Stream protocol preserves partial-I/O handling, full-byte
integrity, stream identity and sequence checks. It offered 400,021,888 bytes per
case over 32 streams, with 20ms delay / 5ms jitter / 0.1% loss, five alternating
baseline/candidate pairs, and a fixed 60-second deadline. Four cases passed and six timed out, with three failures on each version.
The File cohort is failed; incomplete pairs do not
establish resource-threshold acceptance or a candidate regression. FileCC
throughput and recovery time depend on transport feedback. The evidence does
not yet distinguish a workload deadline problem from transport performance
requiring correction. Further controlled diagnosis remains a release gate.

All 52 original and additional case seals and outcome verdicts were independently
replayed using the pinned trusted host validator. A successful validator exit
confirms evidence consistency, not that its reported failed cases passed.
Neither strict acceptance verifier was relaxed. Original failed cases were not
retried or replaced. UDP error counters did not increase and cleanup was verified.
Raw evidence remains local; these selected Linux loopback profiles do not qualify
WAN/NIC/24-hour/all-platform or general Group-selection performance.

## FileCC diagnostic follow-up

The [diagnostic record](diagnostics-0.3.0.json) adds 22 separately declared
network cases and deterministic native probes, using the exact original static
libraries. All case seals and verdicts were independently replayed: 21 network
cases passed and one short 32-stream candidate case timed out. These are
diagnostics, not replacement acceptance cases. The resource gate remains failed.

All four full-workload diagnostic transfers completed with exact payloads in
57.8–111.0 seconds, under a separately declared 180-second diagnostic limit.
Jitter without configured loss produced about 19,500/21,500 retransmissions and
21.7/21.6-second completion for candidate/baseline; clean controls took about
3 seconds. Per-socket traces show queued data advancing through small congestion
windows at low pacing rates. One pair per stochastic condition cannot establish
a version regression or equivalence, and the captured eventual completions do
not pass the original 60-second acceptance gate.

A deterministic reproducer confirms an existing File-mode reorder defect on
both versions. File session setup clears the retransmission capability flag
used to recognize reordered originals and age Fresh-Loss gaps. Even with
`SRTO_LOSSMAXTTL=64`, File tolerance remains zero. With the same reordered
arrivals and bound, the Live capability control grows tolerance and defers the
next gap report. Correct File capability propagation and add regressions before
requalification; keep the default bound zero and respect peers without the bit.

The startup traces also show early NAKs while the reported RTT remains at its
100ms startup value and pacing has slowed to 7–42ms per packet. Synthetic
controller probes reproduce an extreme slow-start exit after sparse initial
feedback on both versions. This supports a startup mechanism requiring further
review; it is not an exact replay of every slow connection's internal feedback.
Any runtime correction requires a new product freeze and renewed affected gates.
All original failed verdicts and package digests remain unchanged.

## File/Control capability correction and pacing review

File session setup now receives the negotiated retransmission-marking bit from
the connection runtime. It preserves that bit while clearing Live delivery
options. The default zero reorder bound and peers without the capability retain
immediate gap reporting. Marked retransmissions do not train reorder tolerance.
Native coverage exercises both API modes, positive/zero bounds and both negotiated
and legacy behavior. Runtime coverage checks actual NAK emission, gap maturation,
public reorder statistics and dynamic-bound changes in Live, File and Control.
All three new regressions fail when capability preservation is disabled.

The startup review traced the mechanism through `ArrivalRateEstimator::rates`,
Full ACK construction and `FileRateController::leave_slow_start`: one positive
arrival interval can produce a nonzero rate; an early NAK leaves slow start using
that rate or locally acknowledged progress. The local floor is capped at one
congestion window per RTT plus the control interval. After exit, that local floor
is no longer updated and faster ACK feedback adjusts the window, while the packet
period recovers through additive increases. This explains the synthetic sparse
feedback result; it is not a complete replay of the network failures.

No arbitrary rate floor or longer acceptance timeout is introduced. Existing
coverage explicitly preserves genuinely slow paths, so raising the startup rate
without additional delivery evidence could violate congestion behavior. Startup
pacing remains unresolved. Further work needs timestamped ACK/NAK replay and
bounded recovery tests that also cover slow paths, ACK compression, sustained
loss and explicit bandwidth caps. The capability fix needs new frozen-source
qualification; previous deadline failures remain failed and are not superseded
by the deterministic regression results.

## Exact startup feedback replay and correction

Four separately declared instrumented captures (three one-stream and one
32-stream jitter case) passed their diagnostic payload checks. All **25,516**
controller input/state records replay exactly against uninstrumented source.
The sealed private capture archive has SHA-256
`6296ab86e686460a0255e11084948922493e0f19166c9aefd60cf40554ad3047`.
Instrumentation can affect scheduling; these are not resource acceptance runs.

The severe captured path has no peer rate estimate: one cumulative ACK arrives
212,278 microseconds after controller creation, then loss ends slow start. A
local ACK floor accidentally became the selected rate even though peer feedback
was absent, replacing the existing window/RTT fallback and producing a 218,647
microsecond packet period. The correction keeps the fallback in this case;
replaying the same loss input produces 6,665 microseconds including the existing
3 percent loss backoff. Only that controller's states change in the captures;
all three one-stream replays remain identical. Replay does not predict changed
network feedback or transfer completion time.

Two new regressions fail against the old controller and pass with the correction.
Coverage includes captured inputs, idle time, bandwidth limits, genuinely low
stored peer estimates and repeated congestion loss. No arbitrary minimum rate,
new wire behavior, or increased acceptance timeout is introduced. The positive
but immature peer-rate case remains a separate sensitivity. The corrected
product still requires new prospective finite File/Stream qualification.

## Renewed finite File/Stream cohort

The newly declared five adjacent pairs used frozen product `607d0034b`, the exact
published-0.2.9 baseline library/peer bytes, and the unchanged finite-stream peer.
All ten workload configurations, ordering, thresholds and 60-second deadlines
match the original File cohort; only peer descriptor paths changed.

The corrected candidate passed **5/5**, each delivering exactly **400,021,888
bytes** in **58.035632–58.913319 seconds**. Both peer exits were zero. The published
baseline passed **2/5**; cases 04, 08 and 09 timed out and remain failed. Kernel UDP
error counters stayed zero and cleanup checks passed in every case. All raw
manifests, payload verdicts and process results were independently replayed using
the pinned validator. The VM returned to its original network/buffer settings,
and this run's processes, tmpfs and input staging were removed.

The archive SHA-256 is
`831601912d23a719a81ca36f9f9f9c5afc1ab25eb7d9bf63df0940620c0803be`;
the prospective declaration SHA-256 is
`da1468c377218ab80ef321bc619341892af42c354a728e91c982c8f44b8f0645`.
Only two pairs are complete. Their metrics are diagnostic and cannot qualify
the predeclared five-pair CPU/RSS/throughput thresholds. No failed or superseded
case is replaced, and the original resource and diagnostic JSON records remain
byte-for-byte unchanged. No broader deadline guarantee is inferred from five
candidate completions. The comparative resource gate remains open.

Renewed hosted checks were launched on `848fc6082` (same frozen product):
[CI](https://github.com/Robotweax/srt/actions/runs/38590217395),
[complete integrations](https://github.com/Robotweax/srt/actions/runs/38590272719),
[ACP1](https://github.com/Robotweax/srt/actions/runs/38590217263), and
[Windows SDK](https://github.com/Robotweax/srt/actions/runs/38590217443)
were still pending at evidence recording. Completed successful workflows were
[Linux packages](https://github.com/Robotweax/srt/actions/runs/38590217385),
[package managers](https://github.com/Robotweax/srt/actions/runs/38590217360), and
[BCrypt](https://github.com/Robotweax/srt/actions/runs/38590217574). Workflow success
does not qualify intentionally skipped optional/protected profiles. Subsequent
evidence changes preserve the frozen product; hosted final acceptance remains
separate from these local and network results.

## Release-owner resource exception — 2026-10-14

The release owner approved publication without a comparative resource claim,
provided every other required release check passes. This exception removes the
incomplete five-pair comparison as a release blocker; it does not mark the
resource cohort or its thresholds accepted. The candidate passed 5/5 finite
transfers, the baseline passed 2/5, and the three baseline timeouts remain failed.
All original evidence, deadlines and thresholds are retained. Machine-readable
resource acceptance remains false; the two complete pairs remain diagnostic.
No qualified CPU, memory or throughput comparison, or general finite-transfer
deadline guarantee, is asserted. All other release gates remain required.

## Final candidate CI corrections

The source CI run at `fc3186f0f` failed installed package-consumer validation
because the release-process document linked to a repository-only evidence file
with a relative path. Commit `7253b217e` replaces that link with the immutable
GitHub evidence URL. Both local static/shared installed-consumer checks pass.

The subsequent ACP1 Linux Mbed TLS job in run `38594187364` exposed an
asynchronous readiness race in the paired-group test: internal runtime attachment
precedes public CONNECTED publication, while ACP1_ACTIVE requires CONNECTED.
The helper now waits for both CONNECTED and the runtime before making the
unchanged ACP1_ACTIVE assertions. All four ACP1 test partitions passed locally
with each of OpenSSL and Mbed TLS, followed by ten successful paired-group
repetitions per provider. Hosted validation of this test correction is pending.
The failed run remains retained. Neither correction changes the frozen library
product, package archive pins or resource evidence.
