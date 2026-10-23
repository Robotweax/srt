# Cryptographic backends

OpenSSL remains the default. Release 0.3.0 also supports
`ROBOTWEAX_SRT_CRYPTO_BACKEND=mbedtls` and the experimental Windows `bcrypt`
backend. Backend selection is a build-time choice; the SRT wire format, key
derivation, key exchange, rotation and session-authentication protocol stay the
same. Released 0.2.9 artifacts predate the Mbed TLS provider.

## Mbed TLS dependency and configuration

Support is limited to **Mbed TLS 3.6.7 and later 3.6.x**. Initial qualification
targets are Linux and macOS, with static and shared Robotweax libraries. Windows,
Android, iOS and custom hardware/ALT implementations are not qualified by this
work. Only `mbedcrypto` is used; TLS and X.509 libraries are not linked.

Mbed TLS 4.x/TF-PSA-Crypto is explicitly rejected. Its stock PSA cipher operations
allocate operation contexts and reinstall keys; it needs separate engineering
to preserve the prepared-key, allocation-free CTR contract. Mbed TLS 3.6 LTS is
maintained until March 2027, so the version-family decision must be reviewed
before that date. See the [upstream support policy](https://github.com/Mbed-TLS/mbedtls/blob/development/BRANCHES.md)
and [PSA migration guide](https://github.com/Mbed-TLS/TF-PSA-Crypto/blob/tf-psa-crypto-1.1.1/docs/1.0-migration-guide.md).

Required modules are `MBEDTLS_AES_C`, `MBEDTLS_CIPHER_MODE_CTR`,
`MBEDTLS_CIPHER_C`, `MBEDTLS_GCM_C`, `MBEDTLS_NIST_KW_C`, `MBEDTLS_PKCS5_C`,
`MBEDTLS_MD_C`, `MBEDTLS_SHA1_C`, `MBEDTLS_SHA256_C`, `MBEDTLS_ENTROPY_C` and
`MBEDTLS_CTR_DRBG_C`. The normal platform entropy source must be available.
Reduced AES-key-length/decryption builds, `MBEDTLS_USE_PSA_CRYPTO`, AES/GCM/KW
ALT implementations (including partial AES/hash replacements), alternate
threading/zeroization, null/disabled default or platform entropy, and
`MBEDTLS_CTR_DRBG_USE_128_BIT_KEY` are rejected. Configuration and link probes
fail during CMake configuration rather than silently dropping capabilities.

The provider supports AES-128/192/256. SHA-1 is retained specifically for SRT's
PBKDF2-HMAC-SHA1 derivation. Key wrapping uses RFC 3394 KW, not padded KWP.
HMAC-SHA256 supports the optional Robotweax session-authentication extension.
AES-GCM primitives are available, while transport use retains the existing
`ENABLE_AEAD_API_PREVIEW` gate and capability checks.

The process-wide provider owns an entropy/CTR-DRBG service protected by a mutex.
Seed/generation failures return a crypto error and erase the complete requested
random destination, including previously generated chunks. Independent
session-owned payload ciphers retain their key schedules. CTR resets stack-local
packet counters, and CTR/GCM support exact in-place operation. GCM failures erase
the entire destination before returning; cipher contexts are cleared on release.
The existing [fork/lifecycle boundary](api-compatibility.md) remains in force.

## Building and installed consumers

Use an existing compatible dependency prefix, or build the pinned qualification
dependency. The helper verifies the official release archive's SHA-256 before
extracting or building it; it does not install globally.

```sh
./tools/python tools/build_mbedtls.py --output output/mbedtls-dependency
cmake -S . -B build-mbedtls -DCMAKE_BUILD_TYPE=Release \
  -DROBOTWEAX_SRT_CRYPTO_BACKEND=mbedtls \
  -DMbedTLS_ROOT="$PWD/output/mbedtls-dependency/install" \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
cmake --build build-mbedtls --parallel
ctest --test-dir build-mbedtls --output-on-failure
```

`FindMbedTLS.cmake` supports upstream package exports and crypto-only packages
without CMake metadata. The installed Robotweax package includes the discovery
module and finds Mbed TLS instead of OpenSSL. Consumers using a custom prefix
should pass `MbedTLS_ROOT` as well. `robotweax-srt.pc` declares
`mbedcrypto >= 3.6.7` in `Requires.private`; include the dependency's
`lib/pkgconfig` directory in `PKG_CONFIG_PATH` when outside system paths.
Static consumers must provide the transitive crypto libraries. Redistributors
must include the dependency's applicable licenses/notices; the qualification
archive permits the Apache-2.0 license alternative.

`SRTO_ROBOTWEAX_CRYPTO_BACKEND` returns
`ROBOTWEAX_SRT_CRYPTO_BACKEND_MBEDTLS` (3) on valid sockets and groups. Values 1
and 2 continue to identify OpenSSL and BCrypt. This observation is local and
does not identify the remote implementation.

The OpenSSL RAND interposition test and OpenSSL-specific group throughput
diagnostic are selected only for OpenSSL. The separate
`tests/mbedtls_qualification` build deliberately links both providers for
differential checks and performance diagnostics; this does not add OpenSSL to
an Mbed TLS library build.

## Qualification and performance

The required CI gate includes Linux/macOS Mbed TLS jobs: unit/known-answer tests,
provider dependency faults, static/shared installed consumers, configuration
rejections, OpenSSL differential comparisons, isolated cross-backend encrypted
rotation/session-authentication, and pinned Haivision CTR/GCM interoperability.
Both platform jobs must run on backend feature PRs before platform qualification
is complete. The [CI selection policy](../CONTRIBUTING.md#ci-selection) describes
which unrelated changes need only the Linux backend suite.

For allocation instrumentation, build the dependency with
`tools/build_mbedtls.py --allocation-hooks`. The provider failure executable
then counts dependency allocations across 4,000 prepared CTR/GCM operations,
including unaligned in-place buffers, and requires zero. It also exercises seed
retry, partial random failure, allocation/key-setup/derivation/wrap/payload
faults, authentication erasure and concurrent provider RNG calls.

Local macOS ARM64 checks passed with Mbed TLS 3.6.7, OpenSSL 4.0.3 and AppleClang
21.0.0. The public peer matrix covers 24 combinations of direction, CTR/GCM,
128/192/256-bit keys and session authentication, with 512 messages in each
direction and a 32-packet refresh interval. Existing Haivision harnesses passed
10 CTR cases at `899348d8318eb9a3c5a5b6ec43c4a1114288773a` and 9 GCM cases/role
checks at `d99d2e1a3b1a213b03c7dfbea5133898935fdeea`. This reuses the established
profile boundaries; it does not qualify every application, platform or optional
wire extension with the new backend.

The standalone Release comparison alternates providers over three rounds,
checks outputs before timing, and measures batches of 128 independent packet
operations after warmup. For 1,316-byte buffers, median batch-average cost was:

| AES key | Operation | Mbed TLS | OpenSSL | Cost ratio |
| --- | --- | --- | --- | --- |
| 128-bit | CTR | 0.261 us | 0.153 us | 1.71x |
| 192-bit | CTR | 0.287 us | 0.166 us | 1.73x |
| 256-bit | CTR | 0.319 us | 0.185 us | 1.72x |
| 128-bit | GCM seal | 1.008 us | 0.247 us | 4.08x |
| 192-bit | GCM seal | 0.981 us | 0.257 us | 3.82x |
| 256-bit | GCM seal | 0.984 us | 0.275 us | 3.58x |

This is one host's crypto-only measurement, not transport throughput or a CI
performance gate. Architecture, dependency acceleration and payload size matter.
The material GCM difference preserves OpenSSL as the default. Reproduce the
comparison with:

```sh
cmake -S tests/mbedtls_qualification -B build-crypto-comparison \
  -DCMAKE_BUILD_TYPE=Release -DMbedTLS_ROOT="/path/to/mbedtls-prefix"
cmake --build build-crypto-comparison --parallel
build-crypto-comparison/compare_mbedtls --benchmark > provider.csv
```

Fixtures use synthetic data and keys. Packet IVs are fresh within each timed
measurement; do not supply production secrets to qualification programs.
