# Package inputs and qualification for Robotweax SRT 0.3.5

On 23 November 2026 the release owner approved the 0.3.5 scope and directed the
freeze of product commit `3415d7c155d393755c8e49f88db4e68789fdf1b6`.
The project version is 0.3.5, shared-library ABI is 0.3, and the compatible SRT
API is 1.5.7. The public C export snapshot contains the same 86 entries as 0.3.4.
The [acceptance record](acceptance-0.3.5.json) captures the three residual risks.

## Frozen source

The product ref is `dev/release-0.3.5-product`. Two downloads of the GitHub
commit archive were byte-identical. All 1,005 regular files and executable bits
matched Git and the archive contained no extra files. The archive is 3,378,654
bytes. The [source manifest](source-0.3.5.json) records both digests and the
frozen paths.

The [release process](../docs/release-process-0.3.5.md) defines the required
evidence and the protected rebuild on the final tag.
