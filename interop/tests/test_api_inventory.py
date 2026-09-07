import json
import re
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]


class ApiInventoryTests(unittest.TestCase):
    def test_installed_public_headers_have_machine_readable_license(self):
        for relative_path in (
            "include/robotweax_srt.h",
            "include/srt.h",
            "include/srt/logging_api.h",
            "include/srt/srt.h",
            "include/srt/version.h",
        ):
            with self.subTest(header=relative_path):
                first_line = (
                    REPOSITORY_ROOT / relative_path
                ).read_text(encoding="utf-8").splitlines()[0]
                self.assertEqual(
                    first_line, "/* SPDX-License-Identifier: MIT */"
                )

    def test_installed_public_sources_have_machine_readable_license(self):
        for relative_path in (
            "examples/installed-consumer/main.cpp",
        ):
            with self.subTest(source=relative_path):
                first_line = (
                    REPOSITORY_ROOT / relative_path
                ).read_text(encoding="utf-8").splitlines()[0]
                self.assertEqual(
                    first_line, "/* SPDX-License-Identifier: MIT */"
                )

    def test_every_exported_c_symbol_has_an_adjacent_api_contract(self):
        declaration = re.compile(
            r"\b(?:ROBOTWEAX_SRT_API|SRT_API)\b"
            r"\s+(?:extern\s+)?[^;{}]*?"
            r"\b((?:robotweax_srt|srt)_[A-Za-z0-9_]+)"
            r"\s*(?=\(|\[|;)",
            re.DOTALL,
        )
        documented: set[str] = set()
        for relative_path in (
            "include/robotweax_srt.h",
            "include/srt/srt.h",
        ):
            content = (REPOSITORY_ROOT / relative_path).read_text(
                encoding="utf-8"
            )
            for match in declaration.finditer(content):
                prefix = content[: match.start()].rstrip()
                self.assertTrue(
                    prefix.endswith("*/"),
                    f"{match.group(1)} lacks an adjacent API contract",
                )
                comment_end = len(prefix)
                comment_start = prefix.rfind("/**")
                self.assertGreaterEqual(
                    comment_start,
                    0,
                    f"{match.group(1)} lacks a Doxygen API contract",
                )
                self.assertEqual(
                    prefix.find("*/", comment_start), comment_end - 2
                )
                documented.add(match.group(1))

        expected = {
            line
            for line in (
                REPOSITORY_ROOT / "cmake/public-symbols.txt"
            ).read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")
        }
        self.assertEqual(documented, expected)

    def test_default_v1_5_7_profile_has_no_pending_surface(self):
        inventory = json.loads(
            (REPOSITORY_ROOT / "compat/srt-1.5.7-api.json").read_text(
                encoding="utf-8"
            )
        )

        surfaces = (
            "functions",
            "exported_data",
            "public_types",
            "socket_options",
            "constant_groups",
            "abi_checks",
        )
        pending = [
            item["name"]
            for surface in surfaces
            for item in inventory[surface]
            if item["status"] == "pending"
        ]
        self.assertEqual(pending, [])

    def test_future_v1_6_options_are_explicit_conditional_exclusions(self):
        inventory = json.loads(
            (REPOSITORY_ROOT / "compat/srt-1.5.7-api.json").read_text(
                encoding="utf-8"
            )
        )
        options = {item["name"]: item for item in inventory["socket_options"]}

        expected = {
            "SRTO_CRYPTOMODE": "ENABLE_AEAD_API_PREVIEW",
            "SRTO_MAXREXMITBW": "ENABLE_MAXREXMITBW",
        }
        for name, condition in expected.items():
            self.assertEqual(options[name]["status"], "excluded")
            self.assertEqual(options[name]["conditional"], condition)

        public_header = (REPOSITORY_ROOT / "include/srt/srt.h").read_text(
            encoding="utf-8"
        )
        for name, condition in expected.items():
            if name not in public_header:
                continue
            self.assertRegex(
                public_header,
                re.compile(
                    rf"#ifdef {re.escape(condition)}[\s\S]*?"
                    rf"\b{re.escape(name)}\b[\s\S]*?#endif"
                ),
                f"{name} must remain inside its explicit preview guard",
            )


if __name__ == "__main__":
    unittest.main()
