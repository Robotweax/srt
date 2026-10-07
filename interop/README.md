# SSH client resource limits

The `remote_interop.py` client treats remote agent output as untrusted. Each JSON
response is limited to 2 MiB, at most two responses may be pending, and only the
last 64 KiB of SSH stderr is retained. Oversized responses or a response flood
terminate the local SSH process and fail the operation.

Artifact downloads accept chunks of at most 96 KiB and require progress until a
boolean EOF marker is received. Each artifact is limited to 1 GiB and five minutes.
Downloads are written to a temporary sibling file and replace the destination
only after a complete, validated transfer. Failures remove the temporary file and
preserve any existing destination. These are client limits; they do not impose
quotas on processes or files created on the remote host.
