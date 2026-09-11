import hashlib
import json
import pathlib
import re
import struct
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST_PATH = ROOT / "compat" / "robotweax-0.2-aead.json"
FIXTURES_PATH = ROOT / "compat" / "robotweax-0.2-aead-fixtures.json"
PUBLIC_HEADER_PATH = ROOT / "include" / "srt" / "srt.h"
AEAD_FUZZ_PATH = ROOT / "fuzz" / "fuzz_aead_packet_open.cpp"
KEY_MATERIAL_FUZZ_PATH = ROOT / "fuzz" / "fuzz_key_material_decode.cpp"
CMAKE_PATH = ROOT / "CMakeLists.txt"
CI_PATH = ROOT / ".github" / "workflows" / "ci.yml"
PC_TEMPLATE_PATH = ROOT / "cmake" / "robotweax-srt.pc.in"
PUBLIC_SYMBOLS_PATH = ROOT / "cmake" / "public-symbols.txt"
HISTORICAL_ABI_PATH = ROOT / "cmake" / "abi" / "robotweax-srt-0.1.0.txt"
FROZEN_0_2_0_ABI_PATH = (
    ROOT / "cmake" / "abi" / "robotweax-srt-0.2.0.txt"
)
CURRENT_PROJECT_VERSION = "0.2.4"
CURRENT_ABI_PATH = (
    ROOT / "cmake" / "abi" / f"robotweax-srt-{CURRENT_PROJECT_VERSION}.txt"
)


def load_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def gcm_iv(salt, sequence):
    iv = bytearray(salt[:12])
    packet_index = sequence.to_bytes(4, "big")
    for index, value in enumerate(packet_index):
        iv[8 + index] ^= value
    return bytes(iv)


def gcm_aad(header, sequence):
    word1 = (
        (header["boundary"] << 30)
        | (int(header["in_order"]) << 29)
        | (header["encryption_key"] << 27)
        | (header["message_number"] & 0x03FF_FFFF)
    )
    return struct.pack(
        ">IIII",
        sequence & 0x7FFF_FFFF,
        word1,
        header["timestamp"],
        header["destination_socket_id"],
    )


class AeadContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = load_json(MANIFEST_PATH)
        cls.fixtures = load_json(FIXTURES_PATH)

    def test_adversarial_runtime_is_pinned_and_default_selection_remains_disabled(
        self,
    ):
        self.assertEqual(self.manifest["schema_version"], 2)
        self.assertEqual(self.manifest["status"], "released")
        self.assertTrue(self.manifest["wire_output_enabled"])
        self.assertEqual(
            self.manifest["implementation"],
            {
                "release_version": "0.2.0",
                "protected_packet_model": True,
                "production_data_runtime": True,
                "caller_listener_ipv4_live_message": True,
                "caller_listener_ipv6_live_message": True,
                "enabled_in_default_api_profile": False,
                "extension_build_selection": True,
                "pinned_reference_interoperability": True,
                "pinned_reference_live_message_interoperability": True,
                "pinned_reference_file_stream_interoperability": False,
                "rendezvous": True,
                "ipv6": True,
                "file_stream": True,
                "fec": True,
                "connection_groups": True,
                "timing": True,
                "adversarial_fuzz": True,
                "platform_abi": True,
            },
        )
        self.assertEqual(self.manifest["default_api_profile"], "srt-1.5.5")
        self.assertFalse(
            self.manifest["public_api"]["enabled_in_default_header"]
        )
        header = PUBLIC_HEADER_PATH.read_text(encoding="utf-8")
        self.assertRegex(
            header,
            re.compile(
                r"#ifdef ENABLE_AEAD_API_PREVIEW\s+"
                r"SRTO_CRYPTOMODE = 62,\s+"
                r"SRTO_E_SIZE = 63\s+"
                r"#else\s+SRTO_E_SIZE = 62\s+#endif"
            ),
        )

    def test_release_evidence_is_frozen_for_version_0_2(self):
        evidence = self.manifest["release_evidence"]
        self.assertEqual(
            evidence,
            {
                "milestone": "AEAD-15",
                "project_version": "0.2.0",
                "shared_library_abi": "0.2",
                "release_tag": "v0.2.0",
                "release_commit": (
                    "e28f5f317f1d497ba4d569a25c95cae94a389b9c"
                ),
                "release_date": "2026-08-27",
                "pull_request_ci": "passed",
                "post_merge_ci": "passed",
                "ctr_regression": "full_ci_matrix",
                "selected_gcm_profiles": [
                    "aead",
                    "aead-ipv6",
                    "aead-file",
                    "aead-file-ipv6",
                    "aead-fec",
                    "aead-group",
                    "aead-timing",
                ],
                "security_gates": [
                    "Address/undefined sanitizers",
                    "Fuzz smoke",
                ],
                "platform_abi_gate": "AES-GCM platform/ABI",
                "documentation": {
                    "release_notes": "docs/release-notes-0.2.0.md",
                    "migration_guide": "docs/migration-0.2.md",
                    "compatibility_report": "docs/compatibility.md",
                    "changelog": "CHANGELOG.md",
                },
                "source_artifacts": [
                    "github-source-archive-tar-gz",
                    "github-source-archive-zip",
                ],
            },
        )
        for path in evidence["documentation"].values():
            with self.subTest(path=path):
                self.assertTrue((ROOT / path).is_file())

    def test_platform_abi_matrix_is_active_for_release_version(self):
        platform = self.manifest["platform_abi"]
        self.assertEqual(
            platform,
            {
                "current_project_version": "0.2.0",
                "target_project_version": "0.2.0",
                "project_version_bump_milestone": "AEAD-15",
                "target_shared_library_abi": "0.2",
                "opaque_c_abi_revision": 1,
                "public_symbol_manifest": "cmake/public-symbols.txt",
                "historical_abi_baseline": (
                    "cmake/abi/robotweax-srt-0.1.0.txt"
                ),
                "current_abi_baseline": (
                    "cmake/abi/robotweax-srt-0.2.0.txt"
                ),
                "target_abi_baseline": (
                    "cmake/abi/robotweax-srt-0.2.0.txt"
                ),
                "public_symbol_count": 80,
                "public_symbol_sha256": (
                    "078187df2008370574a8acb7dd64c17d0abadc3ba9f8d5f45a88a15d949dda2a"
                ),
                "platforms": ["linux", "macos", "windows"],
                "build_type": "Release",
                "linkages": ["static", "shared"],
                "preview_build_guard": "ENABLE_AEAD_API_PREVIEW",
                "default_build_guard_enabled": False,
                "package_consumers": [
                    "cmake_c",
                    "cmake_cpp",
                    "pkg_config_linux",
                ],
                "shared_library_checks": [
                    "exact_public_exports",
                    "load_unload_lifecycle",
                    "installed_package_consumer",
                ],
                "ci_gates": [
                    "Linux Release",
                    "macos-latest Release",
                    "windows-latest Release",
                    "Shared library (ubuntu-latest)",
                    "Shared library (macos-latest)",
                    "Shared library (windows-latest)",
                    "AES-GCM platform/ABI (ubuntu-latest)",
                    "AES-GCM platform/ABI (macos-latest)",
                    "AES-GCM platform/ABI (windows-latest)",
                    "Address/undefined sanitizers",
                    "Fuzz smoke",
                ],
            },
        )

        symbols = PUBLIC_SYMBOLS_PATH.read_bytes()
        self.assertEqual(symbols, HISTORICAL_ABI_PATH.read_bytes())
        self.assertEqual(symbols, FROZEN_0_2_0_ABI_PATH.read_bytes())
        self.assertEqual(symbols, CURRENT_ABI_PATH.read_bytes())
        self.assertEqual(
            len(symbols.decode("utf-8").splitlines()),
            platform["public_symbol_count"],
        )
        self.assertEqual(
            hashlib.sha256(symbols).hexdigest(),
            platform["public_symbol_sha256"],
        )

        cmake = CMAKE_PATH.read_text(encoding="utf-8")
        self.assertIn(
            f"project(robotweax_srt VERSION {CURRENT_PROJECT_VERSION} LANGUAGES",
            cmake,
        )
        self.assertIn("robotweax-srt-${PROJECT_VERSION}.txt", cmake)
        self.assertNotIn("robotweax_srt_0_2_abi_baseline_tests", cmake)
        self.assertIn("ROBOTWEAX_SRT_AEAD_API_PREVIEW", cmake)
        self.assertIn("ROBOTWEAX_SRT_PACKAGE_VERSION", cmake)
        self.assertIn("install(DIRECTORY docs/", cmake)
        self.assertIn('FILES_MATCHING PATTERN "*.md"', cmake)
        self.assertTrue((ROOT / "docs" / "release-notes-0.2.0.md").is_file())
        self.assertTrue((ROOT / "docs" / "release-notes-0.2.1.md").is_file())
        self.assertTrue((ROOT / "docs" / "release-notes-0.2.2.md").is_file())
        self.assertTrue(
            (ROOT / "docs" / f"release-notes-{CURRENT_PROJECT_VERSION}.md").is_file()
        )
        self.assertTrue((ROOT / "docs" / "migration-0.2.md").is_file())

        pkg_config = PC_TEMPLATE_PATH.read_text(encoding="utf-8")
        self.assertIn("@ROBOTWEAX_SRT_PC_CFLAGS@", pkg_config)

        workflow = CI_PATH.read_text(encoding="utf-8")
        self.assertIn("aead_platform:", workflow)
        self.assertIn("os: [ubuntu-latest, macos-latest, windows-latest]", workflow)
        self.assertGreaterEqual(
            workflow.count("-DENABLE_AEAD_API_PREVIEW=ON"), 5
        )
        self.assertIn('aead-platform=$AEAD_PLATFORM', workflow)

    def test_file_stream_evidence_matrix_is_intentionally_narrow(self):
        interop = self.manifest["interoperability"]
        header = PUBLIC_HEADER_PATH.read_text(encoding="utf-8")
        self.assertEqual(
            interop["reference_revision"],
            self.manifest["upstream"]["haivision_srt"]["revision"],
        )
        self.assertEqual(
            interop["ci_profiles"],
            [
                "aead",
                "aead-ipv6",
                "aead-file",
                "aead-file-ipv6",
                "aead-fec",
                "aead-group",
                "aead-timing",
            ],
        )
        self.assertEqual(
            interop["build_guard"], "ENABLE_AEAD_API_PREVIEW"
        )
        self.assertEqual(
            interop["matrix"],
            {
                "key_lengths_bytes": [16],
                "address_families": ["ipv4", "ipv6"],
                "connection_modes": ["caller_listener", "rendezvous"],
                "live_message": {
                    "peers": ["robotweax", "haivision"],
                    "directions": [
                        "robotweax_to_haivision",
                        "haivision_to_robotweax",
                    ],
                },
                "file_stream": {
                    "peers": ["robotweax", "robotweax"],
                    "profiles": [
                        "stream",
                        "file_helper",
                        "loss_recovery",
                    ],
                },
                "fec": {
                    "geometries": ["row", "column", "matrix"],
                    "protected_bytes": "ciphertext_and_16_byte_tag",
                    "robotweax_receiver_directions": [
                        "haivision_to_robotweax"
                    ],
                    "reference_receiver_positive_boundary": (
                        "first_message_row_loss"
                    ),
                    "minimum_data_key_transitions": 2,
                },
                "connection_groups": {
                    "peers": ["robotweax", "robotweax"],
                    "policies": ["broadcast", "backup"],
                    "profiles": [
                        "baseline",
                        "late_join",
                        "ack_suppression_replay",
                        "path_outage",
                    ],
                    "minimum_member_data_key_transitions": 1,
                },
                "timing": {
                    "peers": ["robotweax", "robotweax"],
                    "payload_profiles": ["binary-1200", "ts-1316"],
                    "address_families": ["ipv4", "ipv6"],
                    "single_socket_connection_modes": [
                        "caller_listener",
                        "rendezvous",
                    ],
                    "single_socket_fault_profile": (
                        "loss_delay_reorder"
                    ),
                    "backup_group_profiles": [
                        "baseline",
                        "ack_suppression_replay",
                        "path_outage",
                    ],
                    "minimum_data_key_transitions": 3,
                    "quality_limits": {
                        "early_srt_release_count": 0,
                        "early_udp_egress_count": 0,
                        "payload_integrity_uncompared": 0,
                        "payload_integrity_failures": 0,
                        "tsbpd_deadline_regression_count": 0,
                        "srt_release_regression_count": 0,
                        "maximum_egress_p99_9_microseconds": 20_000,
                        "maximum_burst_depth": 8,
                        "maximum_pcr_span_rate_error_ppm": 5_000,
                        "maximum_replay_tsbpd_phase_range_microseconds": (
                            2_000
                        ),
                    },
                },
                "adversarial": {
                    "fuzz_smoke_runs_per_target": 10_000,
                    "key_material_acceptance_modes": [
                        "automatic",
                        "aes_ctr",
                        "aes_gcm",
                    ],
                    "authenticated_packet_mutations": [
                        "sequence",
                        "message_number",
                        "timestamp",
                        "destination_socket_id",
                        "key_selector",
                        "ciphertext",
                        "authentication_tag",
                        "truncated_authentication_tag",
                        "undersized_output",
                    ],
                    "authenticated_packet_valid_variants": [
                        "original",
                        "retransmitted_header",
                        "oversized_caller_output_buffer",
                    ],
                    "fail_closed_invariants": [
                        "no_plaintext_on_authentication_failure",
                        "complete_caller_output_erasure_on_failure",
                        "no_adjacent_output_mutation",
                        "no_key_state_mutation_from_malformed_key_material",
                        "bounded_key_fec_and_group_replay_history",
                        "secret_redaction_in_remote_evidence",
                    ],
                },
                "key_rotation": {
                    "caller_listener": True,
                    "minimum_caller_listener_data_key_transitions": 3,
                    "rendezvous": True,
                    "minimum_rendezvous_data_key_transitions": 2,
                    "file_stream_caller_listener": True,
                    "file_stream_rendezvous": True,
                    "minimum_file_stream_data_key_transitions": 2,
                    "connection_group_members": True,
                },
            },
        )
        self.assertNotIn(
            "rendezvous", interop["excluded_until_later_milestones"]
        )
        self.assertNotIn(
            "ipv6", interop["excluded_until_later_milestones"]
        )
        self.assertNotIn(
            "file_stream", interop["excluded_until_later_milestones"]
        )
        self.assertIn(
            "pinned_haivision_file_stream_gcm_interoperability",
            interop["excluded_until_later_milestones"],
        )
        self.assertEqual(
            interop["reference_capability_boundaries"],
            [
                {
                    "transport": "file_stream",
                    "reference_requires_tsbpd_for_gcm": True,
                    "result": "setup_rejected",
                    "diagnostics": [
                        "Enable TSBPD to use AES GCM.",
                        "setsockflag 62 failed",
                    ],
                },
                {
                    "transport": "live_message_fec",
                    "reference_rebuilt_message_number": 1,
                    "result": (
                        "later_source_authentication_not_supported_by_"
                        "reference_receiver"
                    ),
                    "positive_boundary": (
                        "robotweax_to_reference_first_message_row_loss"
                    ),
                    "robotweax_receiver_profiles": [
                        "row",
                        "column",
                        "matrix",
                    ],
                },
                {
                    "transport": "connection_groups",
                    "reference_public_group_config_supports_crypto_mode": (
                        False
                    ),
                    "result": "robotweax_self_interoperability_only",
                },
            ],
        )
        self.assertIn(
            "pinned_haivision_live_message_caller_listener_key_rotation",
            interop["excluded_until_later_milestones"],
        )
        self.assertNotIn(
            "live_message_caller_listener_key_rotation",
            interop["excluded_until_later_milestones"],
        )
        self.assertNotIn(
            "connection_groups",
            interop["excluded_until_later_milestones"],
        )
        self.assertRegex(
            header,
            re.compile(
                r"#ifdef ENABLE_AEAD_API_PREVIEW\s+"
                r"SRT_REJ_CRYPTO,\s+#endif"
            ),
        )

    def test_upstream_sources_use_immutable_revisions(self):
        for source in self.manifest["upstream"].values():
            revision = source["revision"]
            self.assertRegex(revision, re.compile(r"^[0-9a-f]{40}$"))
            self.assertTrue(source["sources"])
            for url in source["sources"]:
                self.assertIn(revision, url)

    def test_api_and_wire_numbers_are_exact(self):
        public_api = self.manifest["public_api"]
        self.assertEqual(public_api["socket_option"]["value"], 62)
        self.assertEqual(
            public_api["socket_option"]["modes"],
            {"automatic": 0, "aes_ctr": 1, "aes_gcm": 2},
        )
        self.assertEqual(public_api["key_management_state"]["value"], 5)
        self.assertEqual(public_api["rejection_reason"]["value"], 17)
        self.assertEqual(
            public_api["socket_option"]["gcm_transport_bundles"],
            [
                {
                    "transmission_type": "live",
                    "congestion_control": "live",
                    "tsbpd": True,
                    "message_api": True,
                },
                {
                    "transmission_type": "file",
                    "congestion_control": "file",
                    "tsbpd": False,
                    "message_api": False,
                },
            ],
        )
        wire = self.manifest["wire"]
        self.assertEqual(
            wire["key_material"]["ctr"],
            {"cipher": 2, "authentication": 0},
        )
        self.assertEqual(
            wire["key_material"]["gcm"],
            {"cipher": 4, "authentication": 1},
        )
        self.assertEqual(wire["iv"]["size_bytes"], 12)
        self.assertEqual(wire["aad"]["size_bytes"], 16)
        self.assertEqual(wire["authentication_tag"]["size_bytes"], 16)

    def test_negotiation_tables_are_complete_and_fail_closed(self):
        expected_pairs = {(left, right) for left in range(3) for right in range(3)}
        for name in ("caller_listener", "rendezvous_initiator_responder"):
            rows = self.manifest["negotiation"][name]
            self.assertEqual({(row[0], row[1]) for row in rows}, expected_pairs)
            self.assertTrue(all(row[2] in {"ctr", "gcm", "reject"} for row in rows))
        caller_listener = {
            (row[0], row[1]): row[2]
            for row in self.manifest["negotiation"]["caller_listener"]
        }
        rendezvous = {
            (row[0], row[1]): row[2]
            for row in self.manifest["negotiation"][
                "rendezvous_initiator_responder"
            ]
        }
        self.assertEqual(caller_listener[(2, 0)], "gcm")
        self.assertEqual(rendezvous[(2, 0)], "reject")
        self.assertEqual(caller_listener[(0, 2)], "reject")

    def test_adversarial_fuzz_gate_crosses_production_boundaries(self):
        aead_fuzzer = AEAD_FUZZ_PATH.read_text(encoding="utf-8")
        for boundary in (
            "sender.seal",
            "encode_packet",
            "decode_packet",
            "decode_protected_payload",
            "receiver.open",
            "all_bytes_equal",
        ):
            self.assertIn(boundary, aead_fuzzer)

        key_material_fuzzer = KEY_MATERIAL_FUZZ_PATH.read_text(
            encoding="utf-8"
        )
        self.assertIn("decode_key_material", key_material_fuzzer)
        self.assertIn("accept_key_material", key_material_fuzzer)
        for mode in ("automatic", "aes_ctr", "aes_gcm"):
            self.assertIn(f"CryptoMode::{mode}", key_material_fuzzer)

        cmake = CMAKE_PATH.read_text(encoding="utf-8")
        workflow = CI_PATH.read_text(encoding="utf-8")
        self.assertIn("add_executable(fuzz_aead_packet_open", cmake)
        self.assertIn("--target fuzz_packet_decode", workflow)
        self.assertIn("fuzz_aead_packet_open", workflow)
        self.assertIn("fuzz_aead_packet_open -runs=10000", workflow)

    def test_srt_specific_fixture_derivations_are_exact(self):
        common = self.fixtures["common"]
        salt = bytes.fromhex(common["salt"])
        plaintext = bytes.fromhex(common["plaintext"])
        self.assertEqual(len(salt), 16)
        self.assertEqual(len(plaintext), 32)
        self.assertTrue(common["data_header"]["retransmitted"])
        expected_key_sizes = [16, 24, 32]
        key_iv_pairs = set()
        for vector, expected_key_size in zip(
            self.fixtures["vectors"], expected_key_sizes
        ):
            key = bytes.fromhex(vector["key"])
            iv = bytes.fromhex(vector["iv"])
            aad = bytes.fromhex(vector["aad"])
            ciphertext = bytes.fromhex(vector["ciphertext"])
            tag = bytes.fromhex(vector["tag"])
            protected = bytes.fromhex(vector["protected_payload"])
            self.assertEqual(len(key), expected_key_size)
            self.assertEqual(iv, gcm_iv(salt, vector["sequence"]))
            self.assertEqual(
                aad,
                gcm_aad(common["data_header"], vector["sequence"]),
            )
            self.assertEqual(len(aad), 16)
            self.assertEqual(len(ciphertext), len(plaintext))
            self.assertEqual(len(tag), 16)
            self.assertEqual(protected, ciphertext + tag)
            key_iv_pairs.add((key, iv))
        self.assertEqual(len(key_iv_pairs), len(self.fixtures["vectors"]))


if __name__ == "__main__":
    unittest.main()
