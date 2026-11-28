# Socket configuration and security

For pinned v0.3.5 use the option table and the adapter docs together. Record requested values, units, stage, setter results and meaningful readbacks separately from observations.

1. Own `srt_startup()` and final `srt_cleanup()` at process level. Create a socket and set `SRTO_TRANSTYPE` first (it selects defaults), then any permitted `SRTO_MESSAGEAPI` settings and remaining options. Set PRE-BIND before binding or an implicit bind, and PRE before connection/listen; configure listener inheritance before establishing sessions. Configure the peer consistently. Prefer an explicitly documented single-socket HSv5 Caller/Listener profile without extension features for the first mixed-vendor test. Preserve the required cipher and authentication policy. Choose a media payload/format both peers support. Caller/Listener sets who initiates the connection independently of which end sends media. [Socket phase table][options] · [Integration lifecycle][integration] · [Compatibility][compat]
2. Live/Message preserves message boundaries. Size the receiver for the whole message and keep payload within the negotiated MSS after IP/UDP/SRT and active crypto/FEC overhead. Use 1316 bytes as a common MPEG-TS starting point only where both peers and path allow it. Set Caller Stream ID before connecting. Verify it via listener admission or accepted-socket metadata, and treat it as routing input unless the chosen authentication contract binds it to an identity. Redact sensitive Stream IDs. Configure latency in milliseconds for the native API and check the effective TSBPD delays in each media direction (local receive latency versus peer-advertised minimum). FFmpeg latency URLs use microseconds. Check adapter units independently. Socket/UDP buffers are bytes; flow control is packets. Check effective SRT slot/window and kernel-clamped UDP buffers against bitrate, RTT and bursts. [Live timing][live] · [Socket options][options] · [FFmpeg units][ffmpeg]
3. File/Stream transfers ordered bytes. Loop over partial sends/receives, drain buffered data through EOF, and verify size and hash. Treat a nonzero file offset/resume and any app-level completion ACK as application protocol decisions. Test Live audio/video tail handling separately. A broken connection needs a fresh correctly configured socket, bounded backoff, and a new qualification trial where relevant. After normal traffic, stop input, drain per the applicable contract, close connected sockets and all listener/epoll/group handles, then release final process startup ownership. Capture SRT errors and rejection reasons with credentials removed. [File/Stream][file] · [Integration][integration]

## Bandwidth controls

At v0.3.5 use the correct typed setter and getter on the connected sender or documented owning handle. POST options may also be set before connection as allowed by the tagged table. Read back configured values and measure under offered load using sender/receiver stats and, where required, wire counters. Use a sufficiently long steady sampling window, distinguish payload goodput from wire rate, and observe drops, retransmission and queue growth. [Socket options][options] · [Statistics][stats]

| Option | v0.3.5 Live use |
| --- | --- |
| `SRTO_MAXBW` (`int64_t`, B/s) | Positive is absolute pacing, zero selects relative pacing, and -1 selects the documented 1 Gbit/s Live ceiling. |
| `SRTO_INPUTBW` (`int64_t`, B/s) | With `MAXBW=0`, use the measured or configured input payload rate. A configured zero selects automatic estimation. |
| `SRTO_OHEADBW` (`int32_t`, percent) | With `MAXBW=0`, adds the configured percentage of overhead allowance to the input-rate basis, for example 25 means 25%. It does not add to a positive absolute limit. |

For instance, for an input of 4,000,000 bit/s, set `MAXBW=0`, `INPUTBW=500000`, `OHEADBW=25` in the Live relative profile. Confirm the flags and observe rates under representative recovery load. For File/Stream use its own congestion-controller contract and positive `MAXBW` when an explicit cap is needed. Read it back and test the effect under loss. A socket pacing limit controls only that sender. Give Live priority at the actual shared bottleneck, reserve headroom including repair traffic, and shape or pause File to use measured residual capacity. Test both workloads together during congestion and verify Live delivery, latency and loss targets. [File congestion control][file] · [Tagged bandwidth rules][options]

## Capability gates

| Requirement | Check before attempting a connection |
| --- | --- |
| Passphrase-based confidentiality | Use a verified encrypted profile with enforced encryption, matching passphrase policy and supported key lengths. The common AES-CTR profile provides confidentiality. Requirements for payload integrity or peer identity need separate capabilities. Keep secrets out of process arguments and saved evidence. |
| FEC | Match exact filter negotiation, geometry, ARQ policy, effective payload and transport/security combinations on both peers. Verify actual recovery with controlled loss and observe filter counters. |
| Connection groups | Check build/API, roles, admission, group type and member policy. Prove failover/recovery and member metadata. A verified single socket gives no group evidence. |
| AES-GCM | Pin the opt-in GCM build (v0.3.5 uses `ENABLE_AEAD_API_PREVIEW=ON`), explicit crypto mode, peer build/API and compatible transport. AES-GCM File/Stream is verified Robotweax-to-Robotweax in the stated scope. Cross-implementation GCM File/Stream is unclaimed. |
| Robotweax pinned-key peer authentication | Experimental and default-off in v0.3.5. REQUIRED needs compatible Robotweax builds/policies, compatible crypto backend, SESSIONAUTH, enforced encryption, AES-GCM and pinned identities. Keep experimental qualification distinct from production support. Stock Haivision cannot satisfy REQUIRED. Do not disable REQUIRED or substitute AES-CTR or a Stream ID check. |

When no verified common profile meets the requirement, preserve the unmet row and give the customer the choice of qualifying a compatible peer, changing the deployment topology within their requirements, or deferring deployment. For a separately authorized experimental check, also prove rejection of mismatched settings through the application's normal entry point. [Encryption][crypto] · [AES-GCM profiles][gcm] · [FEC][fec] · [Connection groups][groups] · [Peer authentication][auth] · [v0.3.5 support boundaries][release]

[options]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/socket-options.md
[integration]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/integration.md
[compat]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/compatibility.md
[live]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/live-timing.md
[ffmpeg]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/ffmpeg-integration.md
[file]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/file-mode.md
[stats]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/statistics.md
[crypto]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/encryption.md
[gcm]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/aes-gcm-contract.md
[fec]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/packet-filter.md
[groups]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/connection-groups.md
[auth]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/peer-authentication.md
[release]: https://github.com/Robotweax/srt/releases/tag/v0.3.5
