from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import ci_changes  # noqa: E402


class CiChangeClassifierTests(unittest.TestCase):
    def test_python_ci_tooling_does_not_start_protocol_builds(self) -> None:
        for path in (
            "tools/python",
            "interop/tests/test_python_launcher.py",
            "interop/tests/test_ci_gate.py",
            "interop/tests/test_ci_artifact_license.py",
            "interop/tests/test_ci_build_scope.py",
        ):
            with self.subTest(path=path):
                result = ci_changes.classify([path])
                self.assertTrue(result.code)
                self.assertTrue(result.python)
                self.assertFalse(result.cpp)
                self.assertFalse(result.portable)
                self.assertFalse(result.sanitizers)
                self.assertFalse(result.interop)
                self.assertFalse(result.full)
                self.assertEqual(result.reference_profiles(), ())

    def test_documentation_checker_selects_its_actual_execution(self) -> None:
        result = ci_changes.classify(
            ["tests/package_consumer/check_documentation.py"]
        )
        self.assertTrue(result.documentation)
        self.assertTrue(result.python)
        self.assertFalse(result.cpp)
        self.assertFalse(result.interop)
        self.assertFalse(result.full)

    def test_tooling_exceptions_do_not_weaken_other_changed_paths(self) -> None:
        for production_path in (
            "src/connection.cpp",
            ".github/workflows/ci.yml",
            "interop/ci_changes.py",
            "interop/tests/test_ci_changes.py",
            "tests/package_consumer/c_consumer.c",
            "interop/tests/test_unknown_future_harness.py",
        ):
            with self.subTest(path=production_path):
                expected = ci_changes.classify([production_path])
                for paths in (
                    ["tools/python", production_path],
                    [production_path, "tools/python"],
                ):
                    actual = ci_changes.classify(paths)
                    for name, enabled in vars(expected).items():
                        if enabled:
                            self.assertTrue(getattr(actual, name), name)
                    self.assertEqual(actual.reference_profiles(),
                                     expected.reference_profiles())

    def test_dco_tooling_selects_only_python_contract_tests(self) -> None:
        for path in ("tools/check_dco.py", "interop/tests/test_dco.py"):
            with self.subTest(path=path):
                result = ci_changes.classify([path])

                self.assertTrue(result.code)
                self.assertTrue(result.python)
                self.assertFalse(result.cpp)
                self.assertFalse(result.full)
                self.assertFalse(result.interop)

    def test_documentation_changes_run_no_code_jobs(self) -> None:
        result = ci_changes.classify(
            [
                "README.md",
                "docs/rendezvous.md",
            ]
        )

        self.assertTrue(result.docs_only)
        self.assertTrue(result.documentation)
        self.assertFalse(result.code)
        self.assertFalse(result.format)
        self.assertFalse(result.interop)

    def test_api_manifest_selects_documentation_and_python_contracts(
        self,
    ) -> None:
        result = ci_changes.classify(["compat/srt-1.5.7-api.json"])

        self.assertFalse(result.docs_only)
        self.assertTrue(result.documentation)
        self.assertTrue(result.code)
        self.assertTrue(result.python)
        self.assertFalse(result.cpp)
        self.assertFalse(result.full)
        self.assertFalse(result.interop)

    def test_aead_fixtures_select_aead_contract_gates(self) -> None:
        result = ci_changes.classify(
            ["compat/robotweax-0.2-aead-fixtures.json"]
        )

        self.assertTrue(result.documentation)
        self.assertTrue(result.python)
        self.assertTrue(result.aead_interop)
        self.assertFalse(result.full)

    def test_cpp_change_selects_format_guard(self) -> None:
        result = ci_changes.classify(["src/session.cpp"])

        self.assertTrue(result.cpp)
        self.assertTrue(result.format)
        self.assertTrue(result.ffmpeg)
        self.assertTrue(result.examples)

    def test_public_example_changes_select_release_smoke_tests(self) -> None:
        source = ci_changes.classify(["examples/srt_message_demo.cpp"])

        self.assertTrue(source.code)
        self.assertTrue(source.cpp)
        self.assertFalse(source.core_tests)
        self.assertFalse(source.aead_platform)
        self.assertFalse(source.package)
        self.assertTrue(source.examples)
        self.assertTrue(source.format)
        self.assertTrue(source.portable)
        self.assertFalse(source.sanitizers)
        self.assertFalse(source.interop)
        self.assertFalse(source.full)

        harness = ci_changes.classify(["examples/test_message_demo.py"])

        self.assertTrue(harness.code)
        self.assertTrue(harness.python)
        self.assertTrue(harness.examples)
        self.assertTrue(harness.portable)
        self.assertFalse(harness.cpp)
        self.assertFalse(harness.interop)
        self.assertFalse(harness.full)

    def test_runtime_scheduler_selects_caller_establishment_profiles(
        self,
    ) -> None:
        result = ci_changes.classify(
            [
                "src/compat/runtime_scheduler.cpp",
                "src/compat/runtime_scheduler.hpp",
                "tests/test_runtime_scheduler.cpp",
            ]
        )

        self.assertTrue(result.code)
        self.assertTrue(result.cpp)
        self.assertTrue(result.portable)
        self.assertTrue(result.shared)
        self.assertTrue(result.sanitizers)
        self.assertTrue(result.thread_sanitizer)
        self.assertTrue(result.interop)
        self.assertEqual(
            result.reference_profiles(),
            (
                "handshake",
                "encrypted",
                "aead",
                "aead-ipv6",
                "aead-file",
                "aead-file-ipv6",
                "aead-fec",
                "aead-group",
                "aead-timing",
                "file-base",
                "file-encrypted-base",
                "fec",
            ),
        )

    def test_runtime_scheduler_test_only_change_skips_reference_interop(
        self,
    ) -> None:
        result = ci_changes.classify(["tests/test_runtime_scheduler.cpp"])

        self.assertTrue(result.thread_sanitizer)
        self.assertFalse(result.interop)
        self.assertEqual(result.reference_profiles(), ())

    def test_listener_source_selects_only_establishment_profiles(self) -> None:
        for path in (
            "src/compat/listener_handshake_sources.cpp",
            "src/compat/listener_connection_setup.cpp",
            "src/compat/listener_connection_setup_sources.cpp",
        ):
            with self.subTest(path=path):
                result = ci_changes.classify([path])

                self.assertTrue(result.thread_sanitizer)
                self.assertEqual(
                    result.fuzz,
                    path.endswith("listener_handshake_sources.cpp"),
                )
                self.assertEqual(
                    result.reference_profiles(),
                    (
                        "handshake",
                        "encrypted",
                        "aead",
                        "aead-ipv6",
                        "aead-file",
                        "aead-file-ipv6",
                        "aead-fec",
                        "aead-group",
                        "aead-timing",
                        "file-base",
                        "file-encrypted-base",
                        "fec",
                    ),
                )

    def test_caller_listener_handshake_runtime_selects_establishment_profiles(
        self,
    ) -> None:
        result = ci_changes.classify(
            [
                "src/compat/caller_handshake_events.cpp",
                "src/compat/caller_handshake_events.hpp",
                "src/compat/caller_handshake_sources.cpp",
                "src/compat/caller_handshake_sources.hpp",
                "src/compat/caller_handshake_exchange.cpp",
                "src/compat/caller_handshake_exchange.hpp",
                "src/compat/caller_handshake_steps.cpp",
                "src/compat/caller_handshake_steps.hpp",
                "src/compat/listener_connection_setup.cpp",
                "src/compat/listener_connection_setup.hpp",
                "src/compat/listener_connection_setup_sources.cpp",
                "src/compat/listener_connection_setup_sources.hpp",
                "src/compat/listener_handshake_sources.cpp",
                "src/compat/listener_handshake_sources.hpp",
                "src/compat/runtime_scheduler_service.cpp",
                "src/compat/runtime_scheduler_service.hpp",
                "tests/test_caller_handshake_events.cpp",
                "tests/test_caller_handshake_exchange.cpp",
                "tests/test_caller_handshake_sources.cpp",
                "tests/test_caller_handshake_steps.cpp",
                "tests/test_listener_connection_setup.cpp",
                "tests/test_listener_connection_setup_sources.cpp",
                "tests/test_listener_handshake_sources.cpp",
            ]
        )

        self.assertTrue(result.code)
        self.assertTrue(result.cpp)
        self.assertTrue(result.portable)
        self.assertTrue(result.shared)
        self.assertTrue(result.sanitizers)
        self.assertTrue(result.fuzz)
        self.assertTrue(result.interop)
        self.assertEqual(
            result.reference_profiles(),
            (
                "handshake",
                "encrypted",
                "aead",
                "aead-ipv6",
                "aead-file",
                "aead-file-ipv6",
                "aead-fec",
                "aead-group",
                "aead-timing",
                "file-base",
                "file-encrypted-base",
                "fec",
            ),
        )

    def test_format_tooling_change_is_targeted(self) -> None:
        for path in (
            ".clang-format",
            ".clang-format-ignore",
            "tools/clang_format.py",
            "interop/tests/test_clang_format_tool.py",
        ):
            with self.subTest(path=path):
                result = ci_changes.classify([path])
                self.assertTrue(result.code)
                self.assertTrue(result.format)
                self.assertFalse(result.cpp)
                self.assertFalse(result.interop)
                self.assertFalse(result.full)
                self.assertEqual(result.python, path.endswith(".py"))

    def test_api_manifest_does_not_broaden_source_validation(self) -> None:
        result = ci_changes.classify(
            [
                "compat/srt-1.5.7-api.json",
                "interop/run_ipv6_mtu_interop.py",
            ]
        )

        self.assertTrue(result.python)
        self.assertTrue(result.handshake_interop)
        self.assertFalse(result.full)
        self.assertFalse(result.debug)
        self.assertFalse(result.encrypted_interop)
        self.assertFalse(result.file_interop)
        self.assertFalse(result.fec_interop)
        self.assertFalse(result.rendezvous_interop)

    def test_aead_manifest_selects_only_aead_contract_gates(self) -> None:
        result = ci_changes.classify(
            ["compat/robotweax-0.2-aead.json"]
        )

        self.assertTrue(result.code)
        self.assertTrue(result.python)
        self.assertTrue(result.interop)
        self.assertTrue(result.aead_platform)
        self.assertFalse(result.cpp)
        self.assertFalse(result.full)
        self.assertEqual(
            result.reference_profiles(),
            (
                "aead",
                "aead-ipv6",
                "aead-file",
                "aead-file-ipv6",
                "aead-fec",
                "aead-group",
                "aead-timing",
            ),
        )

    def test_rendezvous_harness_change_is_targeted(self) -> None:
        result = ci_changes.classify(
            [
                "interop/run_rendezvous_interop.py",
                "interop/tests/test_rendezvous_interop.py",
            ]
        )

        self.assertTrue(result.code)
        self.assertTrue(result.python)
        self.assertTrue(result.interop)
        self.assertTrue(result.rendezvous_interop)
        self.assertFalse(result.cpp)
        self.assertFalse(result.file_interop)
        self.assertFalse(result.encrypted_interop)
        self.assertEqual(result.reference_profiles(), ("rendezvous",))

    def test_file_harness_change_is_targeted(self) -> None:
        cases = {
            "interop/run_file_interop.py": (
                "file-base",
                "file-rollover",
                "file-resilience",
                "file-rendezvous",
            ),
            "interop/tests/test_file_interop.py": (
                "file-base",
                "file-rollover",
                "file-resilience",
                "file-rendezvous",
            ),
            "interop/run_encrypted_file_interop.py": (
                "file-encrypted-base",
                "file-encrypted-rendezvous",
                "file-encrypted-faults",
                "file-encrypted-rollover",
            ),
            "interop/tests/test_encrypted_file_interop.py": (
                "file-encrypted-base",
                "file-encrypted-rendezvous",
                "file-encrypted-faults",
                "file-encrypted-rollover",
            ),
            "interop/run_file_peer_error_interop.py": (
                "file-peer-error",
            ),
            "interop/run_peer_error_interop.py": (
                "file-peer-error",
            ),
        }
        for path, expected_profiles in cases.items():
            with self.subTest(path=path):
                result = ci_changes.classify([path])

                self.assertTrue(result.file_interop)
                self.assertFalse(result.rendezvous_interop)
                self.assertFalse(result.handshake_interop)
                self.assertEqual(
                    result.reference_profiles(),
                    expected_profiles,
                )

    def test_fec_harness_change_is_targeted(self) -> None:
        result = ci_changes.classify(
            [
                "interop/run_fec_interop.py",
                "interop/tests/test_fec_interop.py",
            ]
        )

        self.assertTrue(result.code)
        self.assertTrue(result.python)
        self.assertTrue(result.interop)
        self.assertTrue(result.fec_interop)
        self.assertFalse(result.full)
        self.assertFalse(result.file_interop)
        self.assertFalse(result.rendezvous_interop)
        self.assertFalse(result.handshake_interop)
        self.assertEqual(result.reference_profiles(), ("aead-fec", "fec"))

    def test_aead_fec_harness_change_is_targeted(self) -> None:
        result = ci_changes.classify(
            [
                "interop/run_aead_fec_interop.py",
                "interop/tests/test_aead_fec_interop.py",
            ]
        )

        self.assertTrue(result.code)
        self.assertTrue(result.python)
        self.assertTrue(result.interop)
        self.assertTrue(result.aead_fec_interop)
        self.assertFalse(result.fec_interop)
        self.assertFalse(result.full)
        self.assertEqual(result.reference_profiles(), ("aead-fec",))

    def test_fec_core_change_selects_only_fec_interop(self) -> None:
        result = ci_changes.classify(["src/fec.cpp"])

        self.assertTrue(result.cpp)
        self.assertTrue(result.fuzz)
        self.assertTrue(result.fec_interop)
        self.assertFalse(result.file_interop)
        self.assertFalse(result.encrypted_interop)
        self.assertFalse(result.rendezvous_interop)
        self.assertEqual(result.reference_profiles(), ("aead-fec", "fec"))

    def test_ipv6_mtu_harness_change_is_targeted(self) -> None:
        result = ci_changes.classify(
            ["interop/run_ipv6_mtu_interop.py"]
        )

        self.assertTrue(result.interop)
        self.assertTrue(result.handshake_interop)
        self.assertFalse(result.encrypted_interop)
        self.assertFalse(result.file_interop)
        self.assertFalse(result.rendezvous_interop)

    def test_binding_harness_change_is_targeted(self) -> None:
        result = ci_changes.classify(
            [
                "interop/binding_peer.cpp",
                "interop/run_binding_interop.py",
                "interop/tests/test_binding_interop.py",
            ]
        )

        self.assertTrue(result.cpp)
        self.assertTrue(result.python)
        self.assertTrue(result.interop)
        self.assertTrue(result.handshake_interop)
        self.assertFalse(result.encrypted_interop)
        self.assertFalse(result.file_interop)
        self.assertFalse(result.fec_interop)
        self.assertFalse(result.rendezvous_interop)

    def test_group_harness_change_is_targeted(self) -> None:
        result = ci_changes.classify(
            ["interop/run_group_interop.py"]
        )

        self.assertTrue(result.interop)
        self.assertTrue(result.handshake_interop)
        self.assertFalse(result.encrypted_interop)
        self.assertFalse(result.file_interop)
        self.assertFalse(result.fec_interop)
        self.assertFalse(result.rendezvous_interop)

    def test_tlpktdrop_harness_change_is_targeted(self) -> None:
        result = ci_changes.classify(
            [
                "interop/run_tlpktdrop_interop.py",
                "interop/tests/test_tlpktdrop_interop.py",
            ]
        )

        self.assertTrue(result.interop)
        self.assertTrue(result.handshake_interop)
        self.assertFalse(result.encrypted_interop)
        self.assertFalse(result.file_interop)
        self.assertFalse(result.fec_interop)
        self.assertFalse(result.rendezvous_interop)

    def test_receive_contract_harness_change_is_targeted(self) -> None:
        result = ci_changes.classify(
            [
                "interop/run_receive_contract_interop.py",
                "interop/tests/test_receive_contract_interop.py",
            ]
        )

        self.assertTrue(result.code)
        self.assertTrue(result.python)
        self.assertTrue(result.interop)
        self.assertTrue(result.handshake_interop)
        self.assertFalse(result.encrypted_interop)
        self.assertFalse(result.file_interop)
        self.assertFalse(result.fec_interop)
        self.assertFalse(result.rendezvous_interop)

    def test_live_timing_changes_select_the_dedicated_gate(self) -> None:
        for path in (
            "interop/run_live_timing_interop.py",
            "interop/tests/test_live_timing_interop.py",
            "interop/timing_peer.cpp",
        ):
            with self.subTest(path=path):
                result = ci_changes.classify([path])
                self.assertTrue(result.code)
                self.assertTrue(result.timing)
                self.assertTrue(result.interop)
                self.assertEqual(
                    result.reference_profiles(),
                    ("aead-group", "aead-timing"),
                )
                self.assertFalse(result.handshake_interop)
                self.assertFalse(result.encrypted_interop)
                self.assertFalse(result.file_interop)
                self.assertFalse(result.fec_interop)
                self.assertFalse(result.rendezvous_interop)

    def test_cpp_interop_peers_select_cpp_and_reference_gates(self) -> None:
        cases = {
            "interop/api_peer.cpp": {
                "handshake_interop",
                "encrypted_interop",
                "file_interop",
                "fec_interop",
                "rendezvous_interop",
            },
            "interop/reference_peer.cpp": {"handshake_interop"},
            "interop/group_peer.cpp": {"handshake_interop"},
        }
        profiles = {
            "handshake_interop",
            "encrypted_interop",
            "file_interop",
            "fec_interop",
            "rendezvous_interop",
        }

        for path, expected_profiles in cases.items():
            with self.subTest(path=path):
                result = ci_changes.classify([path])
                self.assertTrue(result.code)
                self.assertTrue(result.cpp)
                self.assertTrue(result.portable)
                self.assertTrue(result.sanitizers)
                self.assertFalse(result.python)
                self.assertTrue(result.interop)
                for profile in profiles:
                    self.assertEqual(
                        getattr(result, profile),
                        profile in expected_profiles,
                    )
                self.assertEqual(
                    result.aead_group_interop,
                    path
                    in {
                        "interop/api_peer.cpp",
                        "interop/group_peer.cpp",
                    },
                )
                self.assertEqual(
                    result.aead_timing_interop,
                    path == "interop/api_peer.cpp",
                )

    def test_unknown_cpp_interop_peer_selects_every_reference_gate(
        self,
    ) -> None:
        result = ci_changes.classify(["interop/future_peer.cpp"])

        self.assertTrue(result.cpp)
        self.assertFalse(result.python)
        self.assertTrue(result.interop)
        self.assertTrue(result.handshake_interop)
        self.assertTrue(result.encrypted_interop)
        self.assertTrue(result.file_interop)
        self.assertTrue(result.fec_interop)
        self.assertTrue(result.rendezvous_interop)

    def test_encrypted_harness_selects_the_encrypted_profile(self) -> None:
        result = ci_changes.classify(
            ["interop/run_encrypted_interop.py"]
        )

        self.assertTrue(result.encrypted_interop)
        self.assertTrue(result.aead_interop)

    def test_aead_harness_selects_only_the_aead_profile(self) -> None:
        result = ci_changes.classify(
            [
                "interop/run_aead_interop.py",
                "interop/tests/test_aead_interop.py",
            ]
        )

        self.assertTrue(result.code)
        self.assertTrue(result.python)
        self.assertTrue(result.interop)
        self.assertTrue(result.aead_interop)
        self.assertFalse(result.handshake_interop)
        self.assertFalse(result.encrypted_interop)
        self.assertFalse(result.file_interop)
        self.assertFalse(result.fec_interop)
        self.assertFalse(result.rendezvous_interop)
        self.assertEqual(
            result.reference_profiles(),
            ("aead", "aead-ipv6", "aead-file", "aead-file-ipv6"),
        )

    def test_aead_timing_harness_selects_only_its_profile(self) -> None:
        result = ci_changes.classify(
            [
                "interop/run_aead_timing_interop.py",
                "interop/tests/test_aead_timing_interop.py",
            ]
        )

        self.assertTrue(result.code)
        self.assertTrue(result.python)
        self.assertTrue(result.interop)
        self.assertTrue(result.aead_timing_interop)
        self.assertFalse(result.aead_group_interop)
        self.assertFalse(result.full)
        self.assertEqual(result.reference_profiles(), ("aead-timing",))

    def test_group_timing_sender_selects_public_and_aead_timing(self) -> None:
        result = ci_changes.classify(
            ["interop/group_timing_sender.cpp"]
        )

        self.assertTrue(result.cpp)
        self.assertTrue(result.timing)
        self.assertTrue(result.interop)
        self.assertTrue(result.aead_group_interop)
        self.assertTrue(result.aead_timing_interop)
        self.assertEqual(
            result.reference_profiles(),
            ("aead-group", "aead-timing"),
        )

    def test_scalability_scorecard_runs_only_python_validation(self) -> None:
        result = ci_changes.classify(
            [
                "benchmarks/scalability_scorecard.py",
                "interop/tests/test_scalability_scorecard.py",
                "docs/performance.md",
            ]
        )

        self.assertTrue(result.code)
        self.assertTrue(result.python)
        self.assertFalse(result.full)
        self.assertFalse(result.cpp)
        self.assertFalse(result.portable)
        self.assertFalse(result.interop)
        self.assertFalse(result.handshake_interop)
        self.assertFalse(result.encrypted_interop)
        self.assertFalse(result.file_interop)
        self.assertFalse(result.fec_interop)
        self.assertFalse(result.rendezvous_interop)

    def test_scalability_peer_selects_only_reference_scalability(
        self,
    ) -> None:
        result = ci_changes.classify(
            ["benchmarks/scalability_peer.cpp", "docs/performance.md"]
        )

        self.assertTrue(result.code)
        self.assertTrue(result.cpp)
        self.assertTrue(result.format)
        self.assertTrue(result.portable)
        self.assertTrue(result.sanitizers)
        self.assertTrue(result.thread_sanitizer)
        self.assertTrue(result.benchmark)
        self.assertFalse(result.python)
        self.assertFalse(result.full)
        self.assertFalse(result.interop)
        self.assertFalse(result.handshake_interop)
        self.assertFalse(result.encrypted_interop)
        self.assertFalse(result.file_interop)
        self.assertFalse(result.fec_interop)
        self.assertFalse(result.rendezvous_interop)
        self.assertEqual(result.reference_profiles(), ("scalability",))

    def test_central_connection_change_is_conservative(self) -> None:
        result = ci_changes.classify(
            ["src/compat/connection.cpp"]
        )

        self.assertTrue(result.cpp)
        self.assertTrue(result.portable)
        self.assertTrue(result.shared)
        self.assertTrue(result.sanitizers)
        self.assertTrue(result.thread_sanitizer)
        self.assertFalse(result.fuzz)
        self.assertTrue(result.interop)
        self.assertTrue(result.handshake_interop)
        self.assertTrue(result.encrypted_interop)
        self.assertTrue(result.file_interop)
        self.assertTrue(result.rendezvous_interop)
        self.assertTrue(result.file_base_interop)
        self.assertTrue(result.file_encrypted_base_interop)
        self.assertTrue(result.file_encrypted_rendezvous_interop)
        self.assertTrue(result.file_encrypted_faults_interop)
        self.assertTrue(result.file_encrypted_rollover_interop)
        self.assertTrue(result.file_peer_error_interop)
        self.assertTrue(result.file_rollover_interop)
        self.assertTrue(result.file_resilience_interop)
        self.assertTrue(result.file_rendezvous_interop)

    def test_specific_rendezvous_cpp_change_keeps_targeted_interop(
        self,
    ) -> None:
        result = ci_changes.classify(["src/rendezvous.cpp"])

        self.assertTrue(result.cpp)
        self.assertTrue(result.rendezvous_interop)
        self.assertFalse(result.handshake_interop)
        self.assertFalse(result.file_interop)

    def test_shared_crypto_change_selects_supported_security_profiles(
        self,
    ) -> None:
        result = ci_changes.classify(["src/crypto.cpp"])

        self.assertTrue(result.aead_platform)
        self.assertTrue(result.encrypted_interop)
        self.assertTrue(result.rendezvous_interop)
        self.assertTrue(result.file_interop)
        self.assertTrue(result.file_encrypted_base_interop)
        self.assertTrue(result.file_encrypted_rendezvous_interop)
        self.assertTrue(result.file_encrypted_faults_interop)
        self.assertTrue(result.file_encrypted_rollover_interop)
        self.assertFalse(result.file_base_interop)
        self.assertFalse(result.file_peer_error_interop)
        self.assertFalse(result.handshake_interop)

    def test_file_runtime_change_selects_every_file_profile(self) -> None:
        result = ci_changes.classify(["src/compat/file_io.cpp"])

        self.assertEqual(
            result.reference_profiles(),
            (
                "file-base",
                "file-encrypted-base",
                "file-encrypted-rendezvous",
                "file-encrypted-faults",
                "file-encrypted-rollover",
                "file-peer-error",
                "file-rollover",
                "file-resilience",
                "file-rendezvous",
            ),
        )

    def test_public_unknown_header_uses_all_interop_profiles(self) -> None:
        result = ci_changes.classify(
            ["include/robotweax/srt/new_feature.hpp"]
        )

        self.assertTrue(result.shared)
        self.assertTrue(result.handshake_interop)
        self.assertTrue(result.encrypted_interop)
        self.assertTrue(result.file_interop)
        self.assertTrue(result.rendezvous_interop)

    def test_workflow_and_classifier_changes_require_full_validation(
        self,
    ) -> None:
        for path in (
            ".github/workflows/ci.yml",
            "CMakeLists.txt",
            "cmake/new-build-logic.cmake",
            "tests/package_consumer/new-unknown-probe.cpp",
            "interop/ci_changes.py",
            "interop/tests/test_ci_changes.py",
        ):
            with self.subTest(path=path):
                result = ci_changes.classify([path])
                self.assertTrue(result.full)
                self.assertTrue(result.format)
                self.assertTrue(result.debug)
                self.assertTrue(result.shared)
                self.assertTrue(result.aead_platform)
                self.assertTrue(result.interop)

    def test_known_package_inputs_select_all_package_variants_not_protocols(self) -> None:
        for path in ci_changes.PACKAGE_FILES:
            with self.subTest(path=path):
                result = ci_changes.classify([path])
                for flag in ("package", "portable", "shared", "aead_platform",
                             "ffmpeg", "documentation", "python"):
                    self.assertTrue(getattr(result, flag), flag)
                for flag in ("core_tests", "cpp", "examples", "interop", "full",
                             "debug", "sanitizers", "fuzz", "thread_sanitizer"):
                    self.assertFalse(getattr(result, flag), flag)
                self.assertEqual(result.reference_profiles(), ())

    def test_mixed_selections_keep_production_and_timing_coverage(self) -> None:
        package = "cmake/RobotweaxSRTConfig.cmake.in"
        example = "examples/srt_message_demo.cpp"
        combined = ci_changes.classify([package, example])
        self.assertTrue(combined.package)
        self.assertTrue(combined.examples)
        self.assertFalse(combined.core_tests)
        for path in ("src/compat/runtime_scheduler.cpp", "src/crypto.cpp",
                     "include/srt/srt.h", "tests/test_connection_group.cpp"):
            with self.subTest(path=path):
                expected = ci_changes.classify([path])
                actual = ci_changes.classify([example, package, path])
                self.assertTrue(actual.core_tests)
                for flag, enabled in vars(expected).items():
                    if enabled:
                        self.assertTrue(getattr(actual, flag), flag)
                self.assertEqual(actual.reference_profiles(), expected.reference_profiles())
        timing = ci_changes.classify([example, "interop/run_live_timing_interop.py"])
        self.assertTrue(timing.timing)
        self.assertFalse(timing.core_tests)

    def test_production_retains_previously_implicit_aead_abi_coverage(self) -> None:
        for path in ("src/statistics.cpp", "src/fec.cpp", "src/compat/error_state.cpp"):
            with self.subTest(path=path):
                result = ci_changes.classify([path])
                self.assertTrue(result.core_tests)
                self.assertTrue(result.examples)
                self.assertTrue(result.aead_platform)

    def test_explicit_full_run_needs_no_paths(self) -> None:
        result = ci_changes.classify([], force_full=True)

        self.assertFalse(result.docs_only)
        self.assertTrue(result.full)
        self.assertTrue(result.cpp)
        self.assertTrue(result.ffmpeg)
        self.assertTrue(result.aead_platform)
        self.assertTrue(result.interop)
        self.assertEqual(
            result.reference_profiles(),
            (
                "handshake",
                "encrypted",
                "aead",
                "aead-ipv6",
                "aead-file",
                "aead-file-ipv6",
                "aead-fec",
                "aead-group",
                "aead-timing",
                "file-base",
                "file-encrypted-base",
                "file-encrypted-rendezvous",
                "file-encrypted-faults",
                "file-encrypted-rollover",
                "file-peer-error",
                "file-rollover",
                "file-resilience",
                "file-rendezvous",
                "fec",
                "rendezvous",
                "scalability",
            ),
        )

    def test_ffmpeg_harness_selects_only_the_shared_ffmpeg_gate(self) -> None:
        result = ci_changes.classify(["tests/ffmpeg/run_smoke.sh"])

        self.assertFalse(result.docs_only)
        self.assertTrue(result.code)
        self.assertTrue(result.shared)
        self.assertTrue(result.ffmpeg)
        self.assertFalse(result.full)
        self.assertFalse(result.interop)

    def test_reference_matrix_is_safe_when_no_profile_is_selected(
        self,
    ) -> None:
        result = ci_changes.classify(["docs/roadmap.md"])
        output = dict(
            line.split("=", 1) for line in result.output_lines()
        )

        self.assertEqual(
            json.loads(output["reference_matrix"]),
            {"include": [{"profile": "none"}]},
        )

    def test_reference_matrix_contains_only_selected_profiles(self) -> None:
        result = ci_changes.classify(
            [
                "interop/run_encrypted_file_interop.py",
                "interop/run_fec_interop.py",
            ]
        )
        output = dict(
            line.split("=", 1) for line in result.output_lines()
        )

        self.assertEqual(
            result.reference_profiles(),
            (
                "aead-fec",
                "file-encrypted-base",
                "file-encrypted-rendezvous",
                "file-encrypted-faults",
                "file-encrypted-rollover",
                "fec",
            ),
        )
        self.assertEqual(
            json.loads(output["reference_matrix"]),
            {
                "include": [
                    {"profile": "aead-fec"},
                    {"profile": "file-encrypted-base"},
                    {"profile": "file-encrypted-rendezvous"},
                    {"profile": "file-encrypted-faults"},
                    {"profile": "file-encrypted-rollover"},
                    {"profile": "fec"},
                ]
            },
        )

    def test_unassigned_reference_change_fails_closed(self) -> None:
        result = ci_changes.ChangeSet(docs_only=False, interop=True)

        self.assertEqual(
            result.reference_profiles(),
            (
                "handshake",
                "encrypted",
                "aead",
                "aead-ipv6",
                "aead-file",
                "aead-file-ipv6",
                "aead-fec",
                "aead-group",
                "aead-timing",
                "file-base",
                "file-encrypted-base",
                "file-encrypted-rendezvous",
                "file-encrypted-faults",
                "file-encrypted-rollover",
                "file-peer-error",
                "file-rollover",
                "file-resilience",
                "file-rendezvous",
                "fec",
                "rendezvous",
            ),
        )

    def test_reference_workflow_uses_prepared_dynamic_shards(self) -> None:
        workflow = (
            INTEROP_DIRECTORY.parent / ".github/workflows/ci.yml"
        ).read_text(encoding="utf-8")

        self.assertIn("  reference_interop_prepare:\n", workflow)
        self.assertIn("  reference_interop_shard:\n", workflow)
        self.assertIn("  reference_interop:\n", workflow)
        self.assertIn(
            "matrix: ${{ fromJSON(needs.changes.outputs.reference_matrix) }}",
            workflow,
        )
        self.assertEqual(
            workflow.count("Run many-socket scalability functional profiles"),
            1,
        )
        self.assertNotIn("matrix.profile == 'file'", workflow)
        for profile in (
            "aead-fec",
            "aead-group",
            "aead-timing",
            "file-base",
            "file-encrypted-base",
            "file-encrypted-rendezvous",
            "file-encrypted-faults",
            "file-encrypted-rollover",
            "file-peer-error",
            "file-rollover",
            "file-resilience",
            "file-rendezvous",
        ):
            with self.subTest(profile=profile):
                self.assertIn(
                    f"matrix.profile == '{profile}'",
                    workflow,
                )
        for profile in ("base", "rendezvous", "faults", "rollover"):
            with self.subTest(encrypted_file_profile=profile):
                self.assertIn(f"--profile {profile}", workflow)

    def test_dco_job_checks_pr_range_and_is_required(self) -> None:
        workflow = (
            INTEROP_DIRECTORY.parent / ".github/workflows/ci.yml"
        ).read_text(encoding="utf-8")
        dco_job = workflow.split("  dco:\n", 1)[1].split(
            "\n  documentation:\n", 1
        )[0]
        required_gate = workflow.split("  ci_gate:\n", 1)[1]

        self.assertIn("fetch-depth: 0", dco_job)
        self.assertIn("github.event.pull_request.base.sha", dco_job)
        self.assertIn("github.event.pull_request.head.sha", dco_job)
        self.assertIn("github.actor == 'dependabot[bot]'", dco_job)
        self.assertIn(
            "github.event.pull_request.user.login == 'dependabot[bot]'",
            dco_job,
        )
        self.assertIn(
            "startsWith(github.event.pull_request.head.ref, 'dependabot/')",
            dco_job,
        )
        self.assertIn("arguments+=(--allow-dependabot)", dco_job)
        self.assertIn("python3 tools/check_dco.py", dco_job)
        self.assertIn("      - dco\n", required_gate)
        self.assertIn("DCO: ${{ needs.dco.result }}", required_gate)
        self.assertIn('"dco=$DCO"', required_gate)

    def test_reference_artifact_contains_only_robotweax_tools(self) -> None:
        workflow = (
            INTEROP_DIRECTORY.parent / ".github/workflows/ci.yml"
        ).read_text(encoding="utf-8")

        self.assertEqual(workflow.count("uses: actions/upload-artifact@"), 3)
        timing_evidence = workflow.split(
            "      - name: Preserve AES-GCM timing failure diagnostics\n", 1
        )[1].split("      - name:", 1)[0]
        self.assertIn("failure() && matrix.profile == 'aead-timing'", timing_evidence)
        self.assertIn("steps.aead_timing.outcome == 'failure'", timing_evidence)
        self.assertIn("retention-days: 3", timing_evidence)
        evidence_paths = timing_evidence.split("          path: |\n", 1)[1].split(
            "          retention-days:", 1
        )[0]
        self.assertEqual(
            [line.strip() for line in evidence_paths.splitlines()],
            ["${{ runner.temp }}/aead-timing-failures/run-*/*." + suffix
             for suffix in ("json", "log")],
        )
        ffmpeg_job = workflow.split("  ffmpeg_integration:\n", 1)[1].split(
            "  aead_platform:\n", 1
        )[0]
        self.assertEqual(ffmpeg_job.count("uses: actions/upload-artifact@"), 1)
        self.assertIn("name: ffmpeg-smoke-failure", ffmpeg_job)
        self.assertIn("steps.ffmpeg_smoke.outcome == 'failure'", ffmpeg_job)
        evidence_paths = ffmpeg_job.split("          path: |\n", 1)[1].split(
            "          if-no-files-found:", 1
        )[0]
        self.assertEqual(
            [line.strip() for line in evidence_paths.splitlines()],
            [
                "${{ runner.temp }}/ffmpeg-smoke/robotweax-ffmpeg.*/*." + suffix
                for suffix in ("log", "out", "ts", "m2v")
            ],
        )
        self.assertIn("name: robotweax-interop-tools", workflow)
        self.assertNotIn("name: reference-interop-tools", workflow)

        package_step = workflow.split(
            "- name: Package Robotweax interoperability tools", 1
        )[1].split(
            "- name: Upload Robotweax interoperability tools", 1
        )[0]
        self.assertIn("Refusing non-Robotweax artifact entry", package_step)
        for forbidden_entry in (
            "reference-build/",
            "reference-peer",
            "reference-api-peer",
            "reference-security-api-peer",
            "reference-aead-api-peer",
            "reference-group-peer",
            "reference-scalability-peer",
            "libsrt.so",
            "libsrt.dylib",
        ):
            with self.subTest(forbidden_entry=forbidden_entry):
                self.assertNotIn(forbidden_entry, package_step)

        self.assertIn("Restore pinned Haivision build locally", workflow)
        self.assertIn("Fetch primary Haivision SRT v1.5.7 locally", workflow)
        self.assertIn(
            "Fetch Haivision SRT v1.5.5 backward profile locally", workflow
        )
        self.assertIn(
            "Run Haivision v1.5.5 encrypted backward baseline", workflow
        )
        self.assertIn("Build exact-message reference helper locally", workflow)
        self.assertIn("Build reference API peer locally", workflow)

        prepare_job = workflow.split(
            "  reference_interop_prepare:\n", 1
        )[1].split("  reference_interop_shard:\n", 1)[0]
        shard_job = workflow.split("  reference_interop_shard:\n", 1)[1]
        self.assertEqual(prepare_job.count("lookup-only: true"), 4)
        self.assertNotIn("lookup-only: true", shard_job)


if __name__ == "__main__":
    unittest.main()
