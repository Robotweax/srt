# Third-party components and references

Robotweax SRT source code in this repository is independently authored and is
licensed under the [MIT License](LICENSE). No third-party implementation source
is vendored into the Robotweax SRT source tree.

Compatibility research uses public protocol documents, public API
documentation, reproducible black-box observations, and, for the explicitly
identified profiles below, review of public upstream source at immutable
revisions. The project therefore describes its provenance as independently
authored rather than claiming a strict no-source-review process. Recorded
upstream references provide traceability; they do not make upstream source part
of Robotweax SRT or change the license of independently authored Robotweax
files.

## OpenSSL

The library links to OpenSSL 3 for cryptographic primitives. OpenSSL is a build
and runtime dependency and is not vendored in this repository. Its own license
applies to OpenSSL.

## Haivision SRT

Haivision SRT provides pinned interoperability targets and public API, ABI, and
wire-profile references. Robotweax currently pins these four revisions:

| Profile | Immutable Haivision SRT revision | Purpose |
| --- | --- | --- |
| SRT 1.5.7 compatibility baseline | [`899348d8318eb9a3c5a5b6ec43c4a1114288773a`](https://github.com/Haivision/srt/tree/899348d8318eb9a3c5a5b6ec43c4a1114288773a) | Default API/ABI and primary interoperability target since Robotweax 0.2.2 |
| SRT 1.5.5 backward-compatibility profile | [`b6b4ae990daa8193625a4ddeaeaed03023b23125`](https://github.com/Haivision/srt/tree/b6b4ae990daa8193625a4ddeaeaed03023b23125) | Retained encrypted backward-compatibility checks; historical Robotweax 0.2.0 baseline |
| SRT 1.5.6 security profile | [`c63c311e88aa55e430e3b7d94b89d790994f88c4`](https://github.com/Haivision/srt/tree/c63c311e88aa55e430e3b7d94b89d790994f88c4) | Encrypted security-baseline interoperability |
| AES-GCM extension profile | [`d99d2e1a3b1a213b03c7dfbea5133898935fdeea`](https://github.com/Haivision/srt/tree/d99d2e1a3b1a213b03c7dfbea5133898935fdeea) | Version-pinned GCM wire and interoperability evidence |

Historical release notes and the frozen Robotweax 0.2.0 AES-GCM manifest keep
their original version-scoped references. They do not redefine the current
default baseline. The Ubuntu bootstrap uses the primary 1.5.7 pin; older
profiles remain separate CI configurations.

Haivision SRT is distributed under the
[Mozilla Public License 2.0](https://www.mozilla.org/MPL/2.0/). CI and
`interop/bootstrap_ubuntu.sh` may clone and build the applicable revision in
ignored, temporary development directories. Those checkouts, Haivision
libraries and executables, and helpers linked against Haivision libraries are
not part of the Robotweax SRT source tree or release packages. They must not be
uploaded or published as GitHub Actions artifacts by this project.
Repository-scoped GitHub Actions caches may retain these exact, pinned build
trees solely to accelerate later CI jobs; those caches are not release assets
or downloadable workflow artifacts.

The AES-GCM manifest retains exact public upstream files reviewed while
freezing the extension contract. This record is intentional provenance
evidence, not vendored code. Contributors must not copy Haivision
implementation source, tests, or comments into MIT-licensed Robotweax files.

## Protocol documents

Protocol behavior is derived from applicable public SRT documents, including
the frozen `draft-sharabayko-srt-01`, and from public API documentation. Where
those documents are silent or ambiguous, behavior is established through
retained black-box tests against explicitly pinned releases.
