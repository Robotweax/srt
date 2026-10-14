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

# Remote process cleanup

The agent requires a POSIX host with `waitid`/`WNOWAIT` and default SIGCHLD
handling. Older macOS Python builds use a native binding to the same system call
when `os.waitid` is unavailable. macOS also requires `ps` to distinguish
zombie-only groups from live members when signaling returns `EPERM`; this probe
selects only the owned group. Each executable starts in a new session/process
group.
Status reports the executable's exit without reaping the leader; retaining that
wait identity prevents PID/PGID reuse until group cleanup completes.

Stop and controller disconnect send SIGTERM to the owned group even after the
leader exits. Remaining group members receive SIGKILL after the bounded grace
period (two seconds for controller stop, one second for disconnect). Only then
does the agent reap the leader and retire the group identity. Repeated stops
are harmless. On hosts where an unreaped zombie keeps the group visible, cleanup
may consume the full grace period even when the executable already exited.

This handles descendants that stay in the launched group. A program that creates
another session or process group can escape it. Use external containment such as
a cgroup or systemd scope, with appropriate account permissions and host quotas,
when running potentially adversarial executables.
