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

## Bounded packet capture

`capture_command` now accepts only a single direct `tcpdump` executable (an
absolute path is also accepted). Existing custom argv and `sudo` launchers must
be replaced. Configure capture permissions for the SSH account outside this
harness, for example with narrowly scoped Linux capture capabilities. Do not use
a set-user-ID or set-group-ID binary. Linux captures explicitly retain the SSH
account with `-Z`, so the agent can signal the entire process group.

The controller fixes the capture options: 192-byte snapshots, at most 100,000
packets, and one output file. Each capture inherits a hard 32 MiB per-file limit
for the pcap and process logs. This is a diagnostic header capture, not a full
payload archive. A capture that reaches its packet limit before the scenario
starts fails setup. A cleanup or artifact error fails the run even when the
payload transfer succeeded. Shutdown acknowledges only after process cleanup;
cleanup attempts all owned groups and retains the remote directory on failure.

Arbitrary privileged capture wrappers are unsupported. General peer commands
remain operator-controlled and require external containment when adversarial;
these capture limits are not a host-wide disk quota or a sandbox for peer code.
