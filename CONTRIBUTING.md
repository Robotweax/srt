# Contributing to Robotweax SRT

Robotweax SRT is an independently authored implementation of the SRT protocol.
Changes must preserve its provenance boundary, bounded runtime model, public C
ABI, and evidence-based compatibility claims.

## Ground rules

- Contribute only material that you have the right to license under the MIT
  License.
- Do not copy implementation source, tests, comments, or internal design from
  Haivision SRT or another implementation.
- Prefer public specifications, public API documentation, and reproducible
  black-box observations as evidence.
- If public upstream implementation source must be reviewed to resolve an
  interoperability ambiguity, record the exact immutable revision, files, and
  behavioral conclusion in the relevant public contract or test evidence.
  Independently author the Robotweax implementation; do not copy upstream
  implementation source, tests, or comments.
- Never commit credentials, passphrases, private interoperability inventories,
  unredacted packet captures, or confidential payloads.
- Classify surprising reference behavior before reproducing it: specification
  requirement, documented API behavior, interoperability requirement,
  version-scoped quirk, or reference defect.
- Report security-sensitive findings through [SECURITY.md](SECURITY.md), not a
  public issue.

The public support boundary is documented in
[Compatibility status](docs/compatibility.md). Unsupported behavior must fail
explicitly; it must not be approximated silently.

## Developer Certificate of Origin

Every commit contributed through a pull request must carry a `Signed-off-by`
trailer certifying the [Developer Certificate of Origin 1.1](https://developercertificate.org/).
The sign-off confirms that you have the right to submit the contribution under
the project's MIT License. It is a legal attestation, not an authorship credit
or a cryptographic signature.

Create a signed-off commit with:

```sh
git commit --signoff
```

Add the trailer to the current commit, after reviewing what will be amended,
with:

```sh
git commit --amend --signoff --no-edit
```

The trailer name and email must identify the contributor making the
certification. CI verifies every commit in the pull-request range. Commits
authored by the recognized `dependabot[bot]` identity are exempt only when the
GitHub pull-request user and workflow actor are also that configured bot and
the head branch uses the configured `dependabot/` namespace. That exception
applies only to automated dependency updates and must not be used to carry
human-authored changes. Authorship and credit remain available through Git
history; do not add per-file `Author:` or `Created by:` headers.

Installed public headers carry `SPDX-License-Identifier: MIT` for
machine-readable license identification. Preserve that identifier when editing
or redistributing a header. The complete terms and copyright notice remain in
the repository and installed package `LICENSE`; do not duplicate the full MIT
text or personal authorship metadata in individual source files.

## Development setup

Robotweax SRT requires CMake 3.20 or newer, a C++20 compiler, OpenSSL 3, and
Python 3.10 or newer when tests are enabled. Start with the
[build and installation guide](docs/building.md).

On POSIX development hosts, run Python tooling through `./tools/python`. The
launcher prefers `.venv`, rejects interpreters older than Python 3.10, and
supports the explicit `ROBOTWEAX_SRT_PYTHON` override.

```sh
./tools/python -m venv .venv
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DPython3_EXECUTABLE="$PWD/.venv/bin/python3"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Use a fresh build directory after changing compilers, source directories,
feature flags, or dependency roots. CMake caches are tied to the tree in which
they were generated and must not be copied between workspaces.

## Design expectations

- Parse untrusted input with bounded views and explicit size checks.
- Keep packet codecs allocation-free and independent from operating-system I/O.
- Use explicit-width wire types and wrap-safe sequence/timestamp arithmetic.
- Keep queues, retry state, replay state, and protocol bookkeeping bounded.
- Make clocks, randomness, and network effects injectable in deterministic
  tests.
- Avoid sleeps and wall-clock races in deterministic tests.
- Preserve the stable C boundary and keep internal C++ types private.
- Preserve one process-wide bounded scheduler model; do not add a permanent
  transport thread per connection.
- Treat startup, cleanup, close, callback, fork, unload, and error-state
  behavior as public lifecycle contracts.
- Add interoperability evidence before expanding a compatibility claim.

See [Architecture](docs/architecture.md) and the
[public API compatibility boundary](docs/api-compatibility.md) for the stable
contracts contributors must preserve.

## Formatting

The repository pins clang-format 22.1.8 and stores its style in
`.clang-format`. Always use the fail-closed repository frontend. Do not run a
raw recursive `clang-format -i` command.

Check changed C++ lines against `origin/main`:

```sh
./tools/python tools/clang_format.py check-changed origin/main HEAD
```

Inspect the proposed diff before applying formatting:

```sh
./tools/python tools/clang_format.py write-changed origin/main HEAD
git diff --stat
git diff
```

The frontend rejects an absent style file, the wrong formatter version, paths
outside the repository, and generated or third-party trees. A repository-wide
mechanical rewrite must be proposed separately.

## Testing changes

Run the narrow deterministic tests for the changed component first, then the
appropriate build and integration gates. The public command matrix is in
[Testing](docs/testing.md).

At minimum, a normal source change should pass:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./tools/python -m unittest discover -s interop/tests -p 'test_*.py'
```

Security, wire, lifecycle, timing, FEC, group, ABI, package, or platform
changes require the corresponding focused regression and interoperability
coverage. Do not weaken a deterministic assertion merely because a hosted
runner is slower; determine whether the test models protocol time, scheduler
time, or an environmental deadline.

`Required CI gate` aggregates all selected CI checks, including FFmpeg
integration. It waits for their results and rejects failures or cancellations;
intentionally unselected jobs may be skipped. Reference preparation and shards
are covered through the strict `Reference interoperability` aggregate. When
adding a CI job, update the final gate's dependencies, result environment, and
decision loop. The Python harness tests verify both coverage and the actual
gate shell's handling of unsuccessful results. This aggregate does not itself
configure GitHub branch protection or required status checks.

## Documentation changes

Public documentation is for users, integrators, and external contributors. It
must explain how to build, integrate, configure, operate, and troubleshoot the
released protocol implementation without relying on private project material.

- Keep public API, wire, migration, security, and limitation statements
  versioned and testable.
- Do not add internal roadmaps, CI incident histories, agent instructions,
  private infrastructure details, or speculative third-party defect reports.
- Prefer one canonical page for each contract and link to it instead of
  copying status tables between feature guides.
- Check every relative Markdown link before submitting.

## Pull requests

Keep changes focused and explain:

1. the user-visible or protocol behavior being changed;
2. the evidence or public contract supporting it;
3. the compatibility and resource-bound impact;
4. the tests run locally;
5. any intentionally unsupported combinations.

Do not mix broad formatting, unrelated refactoring, and protocol behavior in
one pull request. Preserve unrelated work already present in the tree.
