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
and pkg-config metadata reflect the selected provider. The Windows SDK installer
remains OpenSSL-based; its packaging is not switched by this change.

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

The next CI stage adds x64 Shared/Release and Shared/Debug builds of both
Robotweax backends, installed C/C++ consumers and separately linked public-API
peers. Each peer loads its own adjacent Robotweax DLL. Existing harnesses test
bidirectional CTR with rotation, GCM (including rendezvous), and the encrypted
FileCC base profile. The Release job additionally builds the pinned Haivision
1.5.7 reference (`899348d8318eb9a3c5a5b6ec43c4a1114288773a`) locally and runs
CTR interoperability. No reference binaries are uploaded. These are validation
jobs, not evidence of success until their CI results have been reviewed.

Before recommending or making this the Windows default, complete:

1. Successful Windows CI and review of error-path/input-size parity.
2. Expanded encrypted public-API interoperability with OpenSSL/Haivision peers,
   including rotation, retransmission, file mode and AEAD preview.
3. Review the new Shared/Debug CI results; non-x64 Shared/Debug coverage remains open.
4. Serial benchmarks against assembler-enabled OpenSSL on the same hardware;
   no speedup is claimed from the historical numbers in issue #13.
5. Native ARM64 execution and a focused cryptographic implementation review.

References: [Issue #13](https://github.com/Robotweax/srt/issues/13),
[BCryptEncrypt](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptencrypt),
[CNG key ownership](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptgeneratesymmetrickey).
