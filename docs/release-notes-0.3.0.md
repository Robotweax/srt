# Robotweax SRT 0.3.0 — Crypto Providers and Admission Controls

Status: **candidate preparation; not published**.
Final candidate, package, integration and signed-installer acceptance is pending.
The latest published release remains [v0.2.9](https://github.com/Robotweax/srt/releases/tag/v0.2.9).
See the [0.3.0 acceptance process](release-process-0.3.0.md).

## Versions and migration

Project version **0.3.0**, shared-library ABI line **0.3**, compatible SRT API
and `srt_getversion()` **1.5.7**. Rebuild applications and dependencies that
linked ABI line 0.2. Existing C export names remain in the frozen 0.3.0 snapshot;
this does not make a 0.3 shared library a binary replacement for SONAME 0.2.
Source-tree C++ consumers must also rebuild. OpenSSL/AES-CTR remains the default.
AES-GCM, session authentication and experimental features retain explicit opt-in.

## Crypto provider and admission observations

The [Mbed TLS provider](crypto-backends.md) supports the documented Mbed TLS
3.6.7+ 3.6.x configurations on Linux/macOS, with static/shared installation,
provider-isolated consumers and cross-backend CTR/GCM scenarios. Mbed TLS 4.x,
unsupported module configurations and unqualified platforms remain excluded.
Provider selection does not change the compatible wire format.

Bounded peer handshake metadata is available during admission and after setup,
without treating provisional or self-reported values as authenticated identity.
The [implementation identity experiment](implementation-identity.md) is separate,
default-off, self-reported and provisional.

## Group negotiation and statistics

Optional path-label and Standby RTT policies add OFF, REQUIRED and listener-only
IF_OFFERED behavior. A listener may admit ordinary peers and peers offering an
extension; callers remain strict. Group admission preserves mirror identity,
per-member peer pins and registry-authoritative listener bond scopes.
See [connection groups](connection-groups.md) and [options](socket-options.md).

Group `pktRetrans*`/`byteRetrans*` counters include all member retransmissions,
including departed members' synchronous/asynchronous linger and final send drain.
Asynchronous tails become visible when runtime retirement completes. Clearing a
group interval does not clear member intervals. This intentionally differs from
libsrt group rows that leave these fields zero.

## Lifecycle and resource corrections

Callback-triggered cleanup uses bounded handoff while preserving handler
replacement ordering. Wire ACK processing retires only submitted DATA, and
peer credit is bounded by the negotiated ceiling. Sensor gap expiration uses
bounded forward work. Listener setup preserves completion during actor start
and replays duplicate conclusions through the existing admission result.

SSH interoperability cleanup now retires same-group descendants after their
leader exits, including children ignoring SIGTERM. It retains leader identity
through final signaling and makes stop idempotent. This developer harness is
not a sandbox for programs that create a different session or process group.

File/Stream and Control/Message now preserve the negotiated retransmission
marking capability used by adaptive `SRTO_LOSSMAXTTL`. Positive bounds can learn
from reordered original packets and delay subsequent gap reports. The default
bound remains zero; peers without the capability retain immediate reporting.
FileCC also preserves its window/RTT fallback when receiver-rate feedback is
absent: a sparse startup ACK no longer substitutes a much slower local rate.
This fixes a captured startup failure mechanism; it does not guarantee finite
lossy transfers meet a deadline.

## ACP1 remains experimental

The [ACP1 prototype](experimental-acp1.md) is behind the default-off
`ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_ACP1` gate and strict bilateral socket opt-in.
It covers documented ordinary/group Caller/Listener and Rendezvous profiles.
It is unreviewed, unstable and not a supported security profile. Independent
cryptographic review and a separate go/no-go remain outstanding. Its inclusion
in candidate source does not close [#190](https://github.com/Robotweax/srt/issues/190)
or the legacy replay limitation [#112](https://github.com/Robotweax/srt/issues/112).
Ordinary Haivision interoperability applies with ACP1 disabled, not to ACP1 peers.

## Qualification and retained limits

Final 0.3.0 qualification is pending. Prior release or feature-PR results do not
substitute for exact-candidate source, media, package and signed-installer gates.
No 0.3.0 installer, package, bottle or release is accepted or published yet.

The release owner approved an exception for the incomplete comparative resource
qualification. The corrected candidate completed all five tested finite
File/Stream transfers within the original 60-second deadline; published 0.2.9
completed two of five. Only two comparison pairs are usable, so CPU, memory and
throughput regression thresholds are not qualified. All failed cases remain in
the release evidence. No general transfer-deadline guarantee is made. This
exception does not waive the remaining release checks.

Live final-loss behavior, File drain/error requirements, group callback domain
migration, negotiated extension boundaries and crypto replay limitations remain
as documented. No new automatic reconnect, RTT selection, universal WAN or
performance guarantee is introduced. See [compatibility](compatibility.md),
[limitations](limitations.md) and [encryption](encryption.md).
