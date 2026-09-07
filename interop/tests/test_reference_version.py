import tempfile
import unittest
import sys
from pathlib import Path


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import reference_version  # noqa: E402


class ReferenceVersionTests(unittest.TestCase):
    def test_dotted_and_integer_forms_match(self):
        self.assertEqual(
            reference_version.parse_srt_version("1.5.7"),
            reference_version.parse_srt_version("0x010507"),
        )
        self.assertEqual(
            reference_version.format_srt_version(0x010507), "1.5.7"
        )

    def test_primary_version_comes_from_public_header(self):
        with tempfile.TemporaryDirectory() as raw_directory:
            header = Path(raw_directory) / "version.h"
            header.write_text(
                "#define SRT_VERSION_MAJOR 2\n"
                "#define SRT_VERSION_MINOR 3\n"
                "#define SRT_VERSION_PATCH 4\n",
                encoding="utf-8",
            )
            self.assertEqual(
                reference_version.compatible_srt_version(header), 0x020304
            )

    def test_invalid_versions_fail_closed(self):
        for value in ("", "1.2", "1.2.999", "-1", "0x1000000"):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    reference_version.parse_srt_version(value)


if __name__ == "__main__":
    unittest.main()
