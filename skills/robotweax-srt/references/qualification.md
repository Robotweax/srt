# Qualify the exact peer combination

Start with the pinned release's documented common profile and immutable Haivision upstream reference. For v0.3.5, the primary reference is stock v1.5.7 at `899348d8318eb9a3c5a5b6ec43c4a1114288773a`. Build it unchanged or verify matching distribution provenance. Run it in a separate process and retain the build flags, executable and library hashes. A stock Haivision build is a reference baseline. Also prioritize the actual FFmpeg and GStreamer builds customers deploy, followed by available OBS, VLC and named commercial devices and firmware. [Compatibility][compat] · [Official Haivision reference source][haivision] · [Tagged interop harnesses][interop]

## Matrix and acceptance

Create one row per exact combination and keep release evidence distinct from customer evidence. Minimum columns:

| Robotweax tag, commit, artifact/build flags | Peer product/version/build/provider | OS/architecture at both ends | Robotweax role | Robotweax media direction | Mode, address family, exact options and security | Result | Evidence and owner |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Pinned manifest | Pinned manifest | Target pair | Caller, Listener, or Rendezvous | Send or receive | Per-end settings and effective values | PASS / FAIL / UNSUPPORTED / NOT RUN | Command, logs, media/hash result and unresolved owner |

Expand Caller/Listener tests into both role assignments and both data directions as supported by each adapter. Give incompatible capabilities explicit UNSUPPORTED rows with the source or observed rejection. Preserve FAIL artifacts and rerun entries, and use NOT RUN for unavailable peers, builds or equipment. Documented but untested combinations stay unqualified.

- Verify the selected artifacts and security options before media. Run positive and negative requirements separately. Include mismatch rejection where required, with both sides' diagnostics and no security fallback.
- Drive representative media end to end. Decode moving picture and the selected audio or channel mapping at the receiving application. Record content evidence, duration, dropouts and decode errors. For File/Stream compare sender and receiver hashes and lengths and verify EOF after draining. Report media identities, selected tracks and timestamps if synchronization is a requirement.
- Stop media and close normally. Confirm process exit/close, then exercise disconnect and reconnect in the deployed topology, including listener reuse where advertised. Verify content and security on the new session. Exercise adverse-network behavior, sustained workload and shared-bottleneck Live/File priority as the deployment requires. A restart check covers endpoint reuse after both applications exit. It gives no evidence for in-process listener reuse.
- Attach the exact commands, OS and architecture, binaries/plugins and provider proof, effective configuration, fixture identity, artifacts and failures. Remove secrets and keep results specific to each combination. [Tagged testing guide][testing] · [File qualification][file] · [Statistics][stats]

## Local clear Live smoke

Run on glibc Linux or macOS with Python 3, the pinned Robotweax shared build and UDP bridge, the candidate peer with a shared SRT provider, and a separate FFmpeg that can generate and decode MPEG-2 video and MP2 audio in MPEG-TS. See the [tagged bridge build][bridge]. This check uses only synthetic content and IPv4 loopback UDP ports. It captures decoded frame/sample hashes for both tracks, captures loader evidence for both processes and repeats each of the four role/direction combinations on the same ports after controlled shutdown. It inspects shared image load paths and rejects an unexpected library hash or multiple SRT libraries. Verify the plugin identity separately. For static builds or duplicate symbol cases, use the [provider verification workflow](integration.md#provider-verification).

Install the canonical `robotweax-srt` directory, with all its references and scripts, as `.agents/skills/robotweax-srt/` for Codex or `.claude/skills/robotweax-srt/` for Claude Code. Start the agent in the external project and invoke `$robotweax-srt` in Codex or `/robotweax-srt` in Claude Code. The installed skill needs the documented tools and built artifacts for the checks. Its release references resolve to the public tagged repository. [Codex skill installation][codex-skill] · [Claude Code skill installation][claude-skill] From the installed skill directory, for example:

```sh
python3 scripts/smoke_live.py \
  --bridge /path/to/robotweax_srt_udp_bridge \
  --robotweax-library /trusted/robotweax/lib/librobotweax-srt.0.3.so \
  --robotweax-build 'v0.3.5:fd9dac31675b12022579d28a748abb0839004cde:OpenSSL:shared' \
  --peer gstreamer --peer-bin /path/to/gst-launch-1.0 \
  --peer-library /trusted/haivision/lib/libsrt.so.1.5 \
  --peer-build 'replace with exact GStreamer and provider build IDs' \
  --decoder /path/to/ffmpeg --output /path/to/new-local-evidence
```

Use `--peer haivision` with a stock `srt-live-transmit`, or `--peer ffmpeg` with an FFmpeg build supporting SRT, UDP, and raw data input/output. The raw data adapter transports the same TS bytes into the receiver's independent decoder. Qualify the customer's normal demux/mux pipeline separately. Inspect the generated `results.json`, fixture, captures and per-process logs. Treat any failed scenario as FAIL. Report the exact profile captured by the smoke. Use the tagged or customer harnesses for encrypted or extended profiles, loss, long duration and application-specific reconnect. See the [compatibility matrix](compatibility-matrix.csv) and [implementation evidence](validation.md) for the exact local builds and results.

[compat]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/compatibility.md
[haivision]: https://github.com/Haivision/srt/tree/899348d8318eb9a3c5a5b6ec43c4a1114288773a
[interop]: https://github.com/Robotweax/srt/tree/v0.3.5/interop
[testing]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/testing.md
[file]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/file-mode.md
[stats]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/statistics.md
[bridge]: https://github.com/Robotweax/srt/blob/v0.3.5/docs/udp-srt-bridge.md
[codex-skill]: https://learn.chatgpt.com/docs/build-skills
[claude-skill]: https://code.claude.com/docs/en/skills
