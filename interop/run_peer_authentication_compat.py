# SPDX-License-Identifier: MIT
"""Black-box admission compatibility; executables use independently authored C API fixtures."""
import argparse
import json
import socket
import subprocess
from pathlib import Path


def scenario(listener_peer, caller_peer, listener_mode, caller_mode, expected):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    listener = subprocess.Popen(
        [str(listener_peer), "listener", str(port), listener_mode],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    try:
        # Listener emits readiness only after binding; impose a process deadline
        # on readiness as well as connect/accept so CI failures cannot hang.
        import selectors
        with selectors.DefaultSelector() as selector:
            selector.register(listener.stdout, selectors.EVENT_READ)
            if not selector.select(5):
                raise RuntimeError("listener did not become ready")
            if listener.stdout.readline().strip() != "READY":
                raise RuntimeError("listener setup failed")
        caller = subprocess.run(
            [str(caller_peer), "caller", str(port), caller_mode],
            capture_output=True, text=True, timeout=5,
        )
        output, error = listener.communicate(timeout=5)
        required_code = 0 if expected else 2
        accepted = "ACCEPTED" in output.splitlines()
        if caller.returncode != required_code or listener.returncode != required_code or accepted != expected:
            raise RuntimeError(
                f"{listener_mode}/{caller_mode}: caller={caller.returncode} listener={listener.returncode}\n"
                f"{caller.stdout}{caller.stderr}{output}{error}"
            )
        return {"listener_mode": listener_mode, "caller_mode": caller_mode,
                "caller_exit": caller.returncode, "listener_exit": listener.returncode,
                "accepted": accepted, "caller_output": caller.stdout.strip()}
    finally:
        if listener.poll() is None:
            listener.kill()
        listener.communicate()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--enabled-peer", type=Path, required=True)
    parser.add_argument("--legacy-peer", type=Path, required=True)
    parser.add_argument("--legacy-session-auth", action="store_true")
    parser.add_argument("--legacy-revision", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    enabled, legacy = args.enabled_peer.resolve(), args.legacy_peer.resolve()
    rows = []
    for enabled_listens in (False, True):
        listener, caller = (enabled, legacy) if enabled_listens else (legacy, enabled)
        for mode in (["legacy", "session"] if args.legacy_session_auth else ["legacy"]):
            rows.append(scenario(listener, caller, mode, mode, True))
        rows.append(scenario(listener, caller,
                             "required" if enabled_listens else "legacy",
                             "legacy" if enabled_listens else "required", False))
    rows.append(scenario(enabled, enabled, "required", "required", True))
    evidence = {"legacy_revision": args.legacy_revision, "enabled_peer": str(enabled),
                "legacy_peer": str(legacy), "scenarios": rows}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"validated {len(rows)} peer-authentication compatibility scenarios")


if __name__ == "__main__":
    main()
