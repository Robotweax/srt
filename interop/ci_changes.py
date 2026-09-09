#!/usr/bin/env python3
"""Conservative path classifier for Robotweax SRT CI jobs."""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, fields
from pathlib import PurePosixPath


CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"}
DOCUMENTATION_FILES = {
    "CODE_OF_CONDUCT.md",
    "CONTRIBUTING.md",
    "LICENSE",
    "README.md",
    "SECURITY.md",
}
FORMAT_TOOLING_FILES = {
    ".clang-format",
    ".clang-format-ignore",
    "tools/clang_format.py",
    "interop/tests/test_clang_format_tool.py",
}
PYTHON_TOOLING_FILES = {
    "tools/mobile_configure.py",
    "interop/tests/test_mobile_configure.py",
    "tools/check_dco.py",
    "tools/python",
    "interop/tests/test_dco.py",
    "interop/tests/test_python_launcher.py",
    "interop/tests/test_ci_gate.py",
    "interop/tests/test_ci_artifact_license.py",
    "interop/tests/test_ci_build_scope.py",
}
DOCUMENTATION_TOOLING_FILES = {
    "tests/package_consumer/check_documentation.py",
}
# Only known package inputs bypass the full build-system fallback. New files
# and the top-level CMakeLists remain conservative until classified explicitly.
PACKAGE_FILES = {
    "cmake/RobotweaxSRTConfig.cmake.in",
    "cmake/srt.pc.in",
    "cmake/robotweax-srt.pc.in",
    "cmake/public-symbols.txt",
    "tests/package_consumer/CMakeLists.txt",
    "tests/package_consumer/run.cmake",
    "tests/package_consumer/c_consumer.c",
    "tests/package_consumer/native_c_consumer.c",
    "tests/package_consumer/cpp_consumer.cpp",
    "tests/package_consumer/pkgconfig_consumer.cpp",
    "tests/package_consumer/ffmpeg_configure_probe.c",
} | {
    f"cmake/abi/robotweax-srt-{version}.txt"
    for version in ("0.1.0", "0.2.0", "0.2.1", "0.2.2", "0.2.3")
}
AEAD_CONTRACT_FILES = {
    "compat/robotweax-0.2-aead-fixtures.json",
    "compat/robotweax-0.2-aead.json",
}
API_CONTRACT_FILES = {
    "compat/srt-1.5.7-api.json",
}
THREAD_TOKENS = {
    "connection",
    "dispatcher",
    "epoll",
    "lifecycle",
    "listener",
    "registry",
    "runtime",
    "socket",
    "thread",
    "udp",
}
FUZZ_TOKENS = {
    "codec",
    "crypto",
    "extension",
    "fec",
    "handshake",
    "key_material",
    "packet",
}
BROAD_INTEROP_TOKENS = {
    "connection",
    "handshake",
    "packet",
    "reliability",
    "session",
    "socket_io",
    "socket_options",
    "srt_api",
    "transport_runtime",
    "udp",
}

REFERENCE_PROFILE_FIELDS = (
    ("handshake", "handshake_interop"),
    ("encrypted", "encrypted_interop"),
    ("aead", "aead_interop"),
    ("aead-ipv6", "aead_interop"),
    ("aead-file", "aead_interop"),
    ("aead-file-ipv6", "aead_interop"),
    ("aead-fec", "aead_fec_interop"),
    ("aead-group", "aead_group_interop"),
    ("aead-timing", "aead_timing_interop"),
    ("file-base", "file_base_interop"),
    ("file-encrypted-base", "file_encrypted_base_interop"),
    (
        "file-encrypted-rendezvous",
        "file_encrypted_rendezvous_interop",
    ),
    ("file-encrypted-faults", "file_encrypted_faults_interop"),
    (
        "file-encrypted-rollover",
        "file_encrypted_rollover_interop",
    ),
    ("file-peer-error", "file_peer_error_interop"),
    ("file-rollover", "file_rollover_interop"),
    ("file-resilience", "file_resilience_interop"),
    ("file-rendezvous", "file_rendezvous_interop"),
    ("fec", "fec_interop"),
    ("rendezvous", "rendezvous_interop"),
    ("scalability", "benchmark"),
)


@dataclass
class ChangeSet:
    docs_only: bool = True
    documentation: bool = False
    code: bool = False
    cpp: bool = False
    core_tests: bool = False
    package: bool = False
    python: bool = False
    examples: bool = False
    format: bool = False
    portable: bool = False
    debug: bool = False
    shared: bool = False
    ffmpeg: bool = False
    aead_platform: bool = False
    sanitizers: bool = False
    thread_sanitizer: bool = False
    fuzz: bool = False
    benchmark: bool = False
    timing: bool = False
    interop: bool = False
    handshake_interop: bool = False
    encrypted_interop: bool = False
    aead_interop: bool = False
    aead_fec_interop: bool = False
    aead_group_interop: bool = False
    aead_timing_interop: bool = False
    file_interop: bool = False
    file_base_interop: bool = False
    file_encrypted_base_interop: bool = False
    file_encrypted_rendezvous_interop: bool = False
    file_encrypted_faults_interop: bool = False
    file_encrypted_rollover_interop: bool = False
    file_peer_error_interop: bool = False
    file_rollover_interop: bool = False
    file_resilience_interop: bool = False
    file_rendezvous_interop: bool = False
    fec_interop: bool = False
    rendezvous_interop: bool = False
    full: bool = False

    def enable_all(self) -> None:
        self.docs_only = False
        for field in fields(self):
            if field.name != "docs_only":
                setattr(self, field.name, True)

    def enable_all_interop(self) -> None:
        self.interop = True
        self.handshake_interop = True
        self.encrypted_interop = True
        self.aead_interop = True
        self.aead_fec_interop = True
        self.aead_group_interop = True
        self.aead_timing_interop = True
        self.enable_all_file_interop()
        self.fec_interop = True
        self.rendezvous_interop = True

    def enable_all_file_interop(self) -> None:
        self.interop = True
        self.file_interop = True
        self.file_base_interop = True
        self.enable_all_encrypted_file_interop()
        self.file_peer_error_interop = True
        self.file_rollover_interop = True
        self.file_resilience_interop = True
        self.file_rendezvous_interop = True

    def enable_all_encrypted_file_interop(self) -> None:
        self.interop = True
        self.file_interop = True
        self.file_encrypted_base_interop = True
        self.file_encrypted_rendezvous_interop = True
        self.file_encrypted_faults_interop = True
        self.file_encrypted_rollover_interop = True

    def output_lines(self) -> list[str]:
        lines = [
            f"{field.name}="
            f"{str(bool(getattr(self, field.name))).lower()}"
            for field in fields(self)
        ]
        lines.append(
            "reference_matrix="
            + json.dumps(
                {
                    "include": [
                        {"profile": profile}
                        for profile in self.reference_profiles()
                    ]
                    or [{"profile": "none"}]
                },
                separators=(",", ":"),
            )
        )
        return lines

    def reference_profiles(self) -> tuple[str, ...]:
        profiles = tuple(
            profile
            for profile, field_name in REFERENCE_PROFILE_FIELDS
            if bool(getattr(self, field_name))
        )
        if self.interop and not profiles:
            # Fail closed if a future classifier path enables reference
            # interoperability without assigning a narrower profile.
            return tuple(
                profile
                for profile, _ in REFERENCE_PROFILE_FIELDS
                if profile != "scalability"
            )
        return profiles


def is_documentation(path: str) -> bool:
    return (
        path in DOCUMENTATION_FILES
        or path.startswith("docs/")
        or path.startswith(".github/ISSUE_TEMPLATE/")
        or path == ".github/PULL_REQUEST_TEMPLATE.md"
        or path.endswith(".md")
    )


def is_full_validation_path(path: str) -> bool:
    return (
        path == "CMakeLists.txt"
        or path.startswith("cmake/")
        or path.startswith("tests/package_consumer/")
        or path.startswith(".github/workflows/")
        or path in {
            "interop/ci_changes.py",
            "interop/tests/test_ci_changes.py",
            "interop/ci_schedule.py",
            "interop/tests/test_ci_schedule.py",
        }
    )


def enable_cpp(change_set: ChangeSet, path: str) -> None:
    change_set.code = True
    change_set.cpp = True
    change_set.core_tests = True
    change_set.format = True
    change_set.portable = True
    change_set.sanitizers = True
    if path.startswith(("src/", "include/")):
        change_set.ffmpeg = True
        change_set.examples = True
        # Preserve the former examples-triggered ABI matrix for production
        # changes. Only isolated demo changes lose that implicit dependency.
        change_set.aead_platform = True
    normalized = path.lower()
    if any(token in normalized for token in THREAD_TOKENS):
        change_set.thread_sanitizer = True
    if any(token in normalized for token in FUZZ_TOKENS):
        change_set.fuzz = True
    if path.startswith("include/") or path.startswith("src/compat/"):
        change_set.shared = True

    if path.startswith("include/") or any(
        token in normalized
        for token in (
            "crypto",
            "socket_options",
            "srt_api",
            "transport_runtime",
        )
    ):
        change_set.aead_platform = True

    production = path.startswith(("src/", "include/"))
    if not production:
        return
    if normalized.startswith(
        (
            "src/compat/caller_handshake_events.",
            "src/compat/caller_handshake_sources.",
            "src/compat/caller_handshake_steps.",
            "src/compat/caller_handshake_exchange.",
            "src/compat/listener_connection_setup.",
            "src/compat/listener_connection_setup_sources.",
            "src/compat/listener_handshake_sources.",
            "src/compat/runtime_scheduler.",
            "src/compat/runtime_scheduler_service.",
        )
    ):
        # These bounded steps, readiness/timer sources, and scheduler adapters
        # advance only Caller/Listener establishment, while Rendezvous and
        # post-establishment FileCC recovery use independent paths.
        change_set.interop = True
        change_set.handshake_interop = True
        change_set.encrypted_interop = True
        change_set.aead_interop = True
        change_set.aead_fec_interop = True
        change_set.aead_group_interop = True
        change_set.aead_timing_interop = True
        change_set.file_interop = True
        change_set.file_base_interop = True
        change_set.file_encrypted_base_interop = True
        change_set.fec_interop = True
        return
    if any(
        token in normalized for token in ("fec", "packet_filter")
    ):
        change_set.interop = True
        change_set.fec_interop = True
        change_set.aead_fec_interop = True
        return
    if any(token in normalized for token in BROAD_INTEROP_TOKENS):
        change_set.enable_all_interop()
        return

    matched_domain = False
    if any(
        token in normalized
        for token in ("crypto", "encrypt", "key_material")
    ):
        change_set.interop = True
        change_set.encrypted_interop = True
        change_set.aead_interop = True
        change_set.aead_fec_interop = True
        change_set.aead_group_interop = True
        change_set.aead_timing_interop = True
        change_set.rendezvous_interop = True
        change_set.enable_all_encrypted_file_interop()
        matched_domain = True
    if "rendezvous" in normalized:
        change_set.interop = True
        change_set.rendezvous_interop = True
        matched_domain = True
    if any(
        token in normalized
        for token in ("file", "congestion", "send_buffer", "receive_buffer")
    ):
        change_set.enable_all_file_interop()
        matched_domain = True
    if not matched_domain:
        change_set.enable_all_interop()


def enable_file_interop_profiles(
    change_set: ChangeSet,
    path: str,
) -> None:
    normalized = path.lower()
    change_set.interop = True
    change_set.file_interop = True
    if "encrypted_file" in normalized:
        change_set.enable_all_encrypted_file_interop()
        return
    if "peer_error" in normalized:
        change_set.file_peer_error_interop = True
        return
    if "file_interop" in normalized:
        # The common FileCC harness owns the baseline, rollover, dynamic-rate,
        # and Rendezvous resilience command paths. A change to that shared
        # runner must exercise each of those entry points, but does not need
        # the independent encryption or PEERERROR harnesses.
        change_set.file_base_interop = True
        change_set.file_rollover_interop = True
        change_set.file_resilience_interop = True
        change_set.file_rendezvous_interop = True
        return
    change_set.enable_all_file_interop()


def enable_interop_profiles(change_set: ChangeSet, path: str) -> None:
    normalized = path.lower()
    if "scalability_scorecard" in normalized:
        # The scorecard uses existing public-API peers. Its own deterministic
        # unit tests validate orchestration and aggregation; comparative runs
        # belong on an explicitly controlled benchmark host, not shared CI.
        return
    if any(
        token in normalized
        for token in ("live_timing", "timing_peer", "timing_sender")
    ):
        change_set.timing = True
        change_set.interop = True
        change_set.aead_group_interop = True
        change_set.aead_timing_interop = True
        return
    if any(
        token in normalized
        for token in ("interop_common", "api_peer", "handshake_trace")
    ):
        change_set.enable_all_interop()
        return
    if any(token in normalized for token in ("ipv6_mtu", "statistics")):
        change_set.interop = True
        change_set.handshake_interop = True
        return
    if "aead_fec" in normalized:
        change_set.interop = True
        change_set.aead_fec_interop = True
        return
    if "aead_group" in normalized:
        change_set.interop = True
        change_set.aead_group_interop = True
        return
    if "aead_timing" in normalized:
        change_set.interop = True
        change_set.aead_timing_interop = True
        return
    if "aead" in normalized:
        change_set.interop = True
        change_set.aead_interop = True
        return
    if any(
        token in normalized
        for token in ("binding_interop", "binding_peer")
    ):
        change_set.interop = True
        change_set.handshake_interop = True
        return
    if "tlpktdrop" in normalized:
        change_set.interop = True
        change_set.handshake_interop = True
        return
    if "receive_contract" in normalized:
        change_set.interop = True
        change_set.handshake_interop = True
        return
    if "group" in normalized:
        change_set.interop = True
        change_set.handshake_interop = True
        change_set.aead_group_interop = True
        return
    if "rendezvous" in normalized:
        change_set.interop = True
        change_set.rendezvous_interop = True
        return
    if "file" in normalized or "peer_error" in normalized:
        enable_file_interop_profiles(change_set, path)
        return
    if "fec" in normalized:
        change_set.interop = True
        change_set.fec_interop = True
        change_set.aead_fec_interop = True
        return
    if any(token in normalized for token in ("encrypted", "encryption")):
        change_set.interop = True
        change_set.encrypted_interop = True
        if "encrypted_interop" in normalized:
            change_set.aead_interop = True
        return
    if any(token in normalized for token in ("handshake", "reference_peer")):
        change_set.interop = True
        change_set.handshake_interop = True
        return
    change_set.enable_all_interop()


def enable_python_interop(change_set: ChangeSet, path: str) -> None:
    change_set.code = True
    change_set.python = True
    enable_interop_profiles(change_set, path)


def classify(
    paths: list[str],
    *,
    force_full: bool = False,
) -> ChangeSet:
    change_set = ChangeSet()
    normalized_paths = sorted(
        {
            path.strip().replace("\\", "/").removeprefix("./")
            for path in paths
            if path.strip()
        }
    )
    if force_full:
        change_set.enable_all()
        return change_set

    for path in normalized_paths:
        if is_documentation(path):
            change_set.documentation = True
            continue
        change_set.docs_only = False
        if path in API_CONTRACT_FILES:
            # The installed public API inventory is both documentation and a
            # machine-readable contract. Validate its JSON layout and its
            # semantic invariants without broadening runtime test selection.
            change_set.documentation = True
            change_set.code = True
            change_set.python = True
            continue
        if path in AEAD_CONTRACT_FILES:
            # The extension manifest is normative input for the Python
            # contract tests and every AES-GCM runtime profile. It does not
            # affect the default build, unrelated FileCC profiles, or the
            # legacy interoperability matrix.
            change_set.documentation = True
            change_set.code = True
            change_set.python = True
            change_set.interop = True
            change_set.aead_interop = True
            change_set.aead_fec_interop = True
            change_set.aead_group_interop = True
            change_set.aead_timing_interop = True
            change_set.aead_platform = True
            continue
        if path in FORMAT_TOOLING_FILES:
            change_set.code = True
            change_set.format = True
            if path.endswith(".py"):
                change_set.python = True
            continue
        if path in PYTHON_TOOLING_FILES:
            change_set.code = True
            change_set.python = True
            continue
        if path in DOCUMENTATION_TOOLING_FILES:
            change_set.code = True
            change_set.python = True
            change_set.documentation = True
            continue
        if path in PACKAGE_FILES:
            change_set.code = True
            change_set.package = True
            change_set.portable = True
            change_set.shared = True
            change_set.aead_platform = True
            change_set.ffmpeg = True
            change_set.documentation = True
            change_set.python = True
            change_set.format |= PurePosixPath(path).suffix.lower() in CPP_SUFFIXES
            continue
        if is_full_validation_path(path):
            change_set.enable_all()
            continue
        if path.startswith("tests/ffmpeg/"):
            change_set.code = True
            change_set.shared = True
            change_set.ffmpeg = True
            continue
        suffix = PurePosixPath(path).suffix.lower()
        if path.startswith("interop/"):
            if suffix in CPP_SUFFIXES:
                enable_cpp(change_set, path)
                enable_interop_profiles(change_set, path)
            else:
                enable_python_interop(change_set, path)
            continue
        if path.startswith("benchmarks/"):
            if suffix == ".py":
                change_set.code = True
                change_set.python = True
            elif suffix in CPP_SUFFIXES:
                enable_cpp(change_set, path)
                change_set.benchmark = True
                if "scalability_peer" in path.lower():
                    change_set.thread_sanitizer = True
            else:
                change_set.enable_all()
            continue
        if path.startswith("examples/"):
            change_set.code = True
            change_set.examples = True
            change_set.portable = True
            if suffix in CPP_SUFFIXES:
                change_set.cpp = True
                change_set.format = True
            elif suffix == ".py":
                change_set.python = True
            else:
                change_set.enable_all()
            continue
        if suffix in CPP_SUFFIXES or path.startswith(
            ("src/", "include/", "tests/", "tools/", "fuzz/")
        ):
            enable_cpp(change_set, path)
            continue
        # Unknown non-documentation paths are deliberately broad. Missing a
        # relevant test is more expensive than an occasional full PR run.
        change_set.enable_all()

    return change_set


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "paths",
        nargs="*",
        help="changed paths, or a single '-' to read newline-delimited stdin",
    )
    parser.add_argument("--full", action="store_true")
    arguments = parser.parse_args()
    paths = (
        [line.rstrip("\n") for line in sys.stdin]
        if arguments.paths == ["-"]
        else arguments.paths
    )
    result = classify(
        paths,
        force_full=arguments.full,
    )
    print("\n".join(result.output_lines()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
