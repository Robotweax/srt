# Experimental Windows BCrypt backend

The optional `ROBOTWEAX_SRT_CRYPTO_BACKEND=bcrypt` CMake setting selects the
Windows CNG provider. The default remains `openssl`, on every platform.
No public SRT API or wire-format change is intended. This backend is under
qualification, not recommended as a production replacement yet.

```powershell
cmake -S . -B build-bcrypt -A x64 -DROBOTWEAX_SRT_CRYPTO_BACKEND=bcrypt
cmake --build build-bcrypt --config Release --parallel 2
```

Use a separate build directory for each backend. BCrypt is rejected on non-Windows
platforms and unknown backend values are errors. BCrypt builds link the native
`bcrypt` system library and do not discover or require OpenSSL. Installed CMake
and pkg-config metadata reflect the selected provider.

The **experimental Windows SDK installer for application developers** is the
separate packaging work requested in [issue #12](https://github.com/Robotweax/srt/issues/12),
not a standalone streaming application. It bundles public headers, static
Robotweax SRT and OpenSSL Crypto libraries for Debug/Release on Win32, x64 and
ARM64, and an MSBuild property sheet. See the
[Windows SDK packaging documentation](https://github.com/Robotweax/srt/blob/main/packaging/windows/README.md).
The installer candidate remains OpenSSL-based and is not yet a signed,
qualified release installer. The optional BCrypt backend is available in
`main`; it does not switch the installer packaging or the default backend.

## Implementation boundary

The existing internal `CryptoProvider` contract is used unchanged:

- System-preferred CNG RNG and PBKDF2-HMAC-SHA1 for SRT key establishment.
- AES-CTR constructed from batched AES-ECB counter blocks, incrementing the
  entire 128-bit counter big-endian. No padding and no packet-path C++ allocation.
- RFC 3394 AES key wrap/unwrap using the AES block primitive and A6 integrity
  register. Temporary unwrapped material is erased and is not exposed before
  integrity validation succeeds.
- Native AES-GCM with 12-byte IV, 16-byte tag, AAD and exact in-place operation.
  Authentication failures erase the entire destination, as required by the
  existing provider contract. Enabling the backend does not enable the separate
  experimental AEAD public API option.
- Per-cipher CNG key handles, destroyed before their algorithm handles. No shared
  mutable key or packet state. Session ownership rules remain unchanged.

## Qualification status

The dedicated CI workflow builds Win32, x64 and ARM64 Release variants with OpenSSL
discovery explicitly disabled. Win32/x64 run existing provider known-answer tests
and crypto-session tests. ARM64 is compile/link-only. Installed C/C++ consumers
also build without OpenSSL discovery. A separate x64 test-only executable links
both providers for byte-for-byte comparison, including counter rollover, partial
blocks, batch boundaries, all AES key sizes, key wrap, GCM tags, in-place data and
bad-tag rejection. OpenSSL in that executable is an oracle, not a runtime
dependency of the BCrypt SRT library.

The dedicated CI also covers x64 Shared/Release and Shared/Debug builds of both
Robotweax backends, installed C/C++ consumers and separately linked public-API
peers. Each peer loads its own adjacent Robotweax DLL. Existing harnesses test
bidirectional CTR with rotation, GCM (including rendezvous), and the encrypted
FileCC base profile. The Release job additionally builds the pinned Haivision
1.5.7 reference (`899348d8318eb9a3c5a5b6ec43c4a1114288773a`) locally and runs
CTR interoperability. No reference binaries are uploaded.

The above qualification jobs passed for commit
`5c428c82f25e20f77806cd37fe448d9ef56e4ca2` in
[PR #19](https://github.com/Robotweax/srt/pull/19), and their results were reviewed
on 2026-09-10. The selected standard CI checks, including platform builds,
sanitizers, reference interoperability and FFmpeg integration, also passed.
This supports merging the opt-in experimental backend; it is not a production
qualification or an independent cryptographic audit.

### Open intermittent setup observation

[Run 34444279850](https://github.com/Robotweax/srt/actions/runs/34444279850)
failed in the first Release AES-128 CTR baseline: the BCrypt caller timed out
(`reject_reason=16`), while the OpenSSL listener had accepted a connection but
received no payload. This is an unresolved setup observation, not a confirmed
cryptographic calculation failure or a proven fix.

On commit `087d3a3866870a17ab3af31e7deab27596c749f1`,
[diagnostic run 34444918718](https://github.com/Robotweax/srt/actions/runs/34444918718)
completed ten alternating direct/handshake-proxy pairs, each in both directions:
20 invocations and 40 successful transfers, with no reproduced failure. The
regular CI also passed. The proxy changes scheduling, so traced successes do
not replace direct controls. These results do not establish a failure-rate
bound or resolve the original timeout. The temporary repetitions were removed
to avoid recurring CI costs; normal baseline checks and timeouts remain intact.
The diagnostic recipe remains available in that commit. Reinvestigate with
direct controls and handshake traces if the setup failure recurs.

Before recommending or making this the Windows default, complete:

1. Broader error-path/input-size parity review, including injected CNG failures.
2. Expanded encrypted public-API interoperability with OpenSSL/Haivision peers,
   including rotation, retransmission, file mode and AEAD preview.
3. Non-x64 Shared/Debug coverage beyond the existing compile/link and static tests.
4. Serial benchmarks against assembler-enabled OpenSSL on the same hardware;
   no speedup is claimed from the historical numbers in issue #13.
5. Native ARM64 execution and a focused cryptographic implementation review.

References: [Issue #13](https://github.com/Robotweax/srt/issues/13),
[BCryptEncrypt](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptencrypt),
[CNG key ownership](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptgeneratesymmetrickey).
