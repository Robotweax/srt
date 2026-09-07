from __future__ import annotations

import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = REPOSITORY_ROOT / "tools" / "clang_format.py"
SPEC = importlib.util.spec_from_file_location("clang_format_tool", MODULE_PATH)
assert SPEC is not None
assert SPEC.loader is not None
clang_format_tool = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = clang_format_tool
SPEC.loader.exec_module(clang_format_tool)


class ClangFormatToolTests(unittest.TestCase):
    def test_cpp_path_selection_excludes_generated_trees(self) -> None:
        self.assertTrue(clang_format_tool.is_cpp_path("src/session.cpp"))
        self.assertTrue(
            clang_format_tool.is_cpp_path(
                "include/robotweax/srt/session.hpp"
            )
        )
        self.assertFalse(clang_format_tool.is_cpp_path("docs/design.md"))
        self.assertFalse(
            clang_format_tool.is_cpp_path("build-agent/generated/version.cpp")
        )
        self.assertFalse(
            clang_format_tool.is_cpp_path("third_party/library/source.cpp")
        )
        self.assertTrue(
            clang_format_tool.is_cpp_path("src/build_configuration.cpp")
        )

    def test_changed_ranges_ignore_deletions_and_merge_adjacency(self) -> None:
        diff = """\
@@ -4,0 +5,2 @@
@@ -11,3 +13,0 @@
@@ -14 +14,2 @@
@@ -20 +21 @@
"""

        self.assertEqual(
            clang_format_tool.parse_changed_ranges(diff),
            [(5, 6), (14, 15), (21, 21)],
        )

    def test_invalid_line_range_fails_closed(self) -> None:
        with self.assertRaises(clang_format_tool.FormatError):
            clang_format_tool.merge_ranges([(0, 1)])

    def test_version_parser_requires_complete_pinned_version(self) -> None:
        match = clang_format_tool.VERSION_PATTERN.search(
            "Homebrew clang-format version 22.1.8"
        )

        self.assertIsNotNone(match)
        assert match is not None
        self.assertEqual(match.group("version"), "22.1.8")

    def test_dumped_configuration_accepts_aligned_false_value(self) -> None:
        self.assertIsNotNone(
            clang_format_tool.FORMAT_ENABLED_PATTERN.search(
                "Language: Cpp\nDisableFormat:  false\n"
            )
        )
        self.assertIsNone(
            clang_format_tool.FORMAT_ENABLED_PATTERN.search(
                "Language: Cpp\nDisableFormat: true\n"
            )
        )

    def test_missing_configuration_fails_before_invoking_formatter(self) -> None:
        with (
            mock.patch.object(Path, "is_file", return_value=False),
            mock.patch.object(clang_format_tool, "run_command") as run,
        ):
            with self.assertRaisesRegex(
                clang_format_tool.FormatError,
                "repository .clang-format is missing",
            ):
                clang_format_tool.validate_configuration("clang-format")

        run.assert_not_called()

    def test_changed_ranges_are_forwarded_to_read_only_formatter(self) -> None:
        completed = subprocess.CompletedProcess([], 0)
        with mock.patch.object(
            clang_format_tool,
            "run_command",
            return_value=completed,
        ) as run:
            clang_format_tool.format_sources(
                "/usr/bin/clang-format",
                [("/repo/source.cpp", [(5, 6), (14, 14)])],
                write=False,
            )

        run.assert_called_once_with(
            [
                "/usr/bin/clang-format",
                "--style=file",
                "--fallback-style=none",
                "--lines=5:6",
                "--lines=14:14",
                "--dry-run",
                "--Werror",
                "/repo/source.cpp",
            ]
        )


if __name__ == "__main__":
    unittest.main()
