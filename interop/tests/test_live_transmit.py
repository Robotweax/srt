from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
DIRECTORY = ROOT / 'tests/live_transmit'
spec = importlib.util.spec_from_file_location('live_transmit_prepare', DIRECTORY / 'prepare_source.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class LiveTransmitSourceTests(unittest.TestCase):
    def test_pin_and_source_allowlist_exclude_upstream_transport(self):
        manifest = json.loads((DIRECTORY / 'source-manifest.json').read_text())
        self.assertEqual(manifest['revision'], prepare.REVISION)
        self.assertEqual(manifest['revision'], '899348d8318eb9a3c5a5b6ec43c4a1114288773a')
        compiled = [p for p in manifest['files'] if p.endswith('.cpp')]
        self.assertEqual(len(compiled), 9)
        self.assertTrue(all(p.startswith('apps/') for p in compiled))
        self.assertNotIn('srtcore/srt.h', manifest['files'])
        self.assertNotIn('srtcore/sync.h', manifest['files'])

    def test_input_hash_mismatch_is_rejected_without_modification(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'sample.cpp').write_bytes(b'authored synthetic fixture\n')
            correct = hashlib.sha256((root / 'sample.cpp').read_bytes()).hexdigest()
            self.assertEqual(prepare.verified_inputs(root, {'sample.cpp': correct}),
                             {'sample.cpp': b'authored synthetic fixture\n'})
            with self.assertRaisesRegex(RuntimeError, 'unrecognized source bytes'):
                prepare.verified_inputs(root, {'sample.cpp': '0' * 64})
            self.assertEqual((root / 'sample.cpp').read_bytes(), b'authored synthetic fixture\n')

    def test_marker_mismatch_fails_closed(self):
        with self.assertRaisesRegex(RuntimeError, 'unexpected pinned source marker'):
            prepare.replace('unrelated fixture', 'expected marker', 'replacement')
        with self.assertRaises(RuntimeError):
            prepare.replace('marker marker', 'marker', 'replacement')

    def test_source_output_overlap_rejected_before_git_or_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for output in (root, root / 'generated', root.parent):
                with self.subTest(output=output), patch.object(prepare.subprocess, 'check_output') as git:
                    with self.assertRaisesRegex(RuntimeError, 'separate trees'):
                        prepare.prepare(root, output)
                    git.assert_not_called()

    def test_standalone_cmake_uses_imported_sdk_and_no_upstream_build(self):
        cmake = (DIRECTORY / 'CMakeLists.txt').read_text()
        self.assertIn('find_package(RobotweaxSRT CONFIG REQUIRED)', cmake)
        self.assertIn('PRIVATE RobotweaxSRT::srt', cmake)
        self.assertNotIn('add_subdirectory(', cmake)
        self.assertNotIn('FetchContent', cmake)
        self.assertNotIn('install(TARGETS', cmake)
        self.assertIn('ENABLE_BONDING=0', cmake)

    def test_ci_retains_only_summary_json_not_app_or_sources(self):
        workflow = (ROOT / '.github/workflows/ci.yml').read_text()
        job = workflow.split('  live_transmit_integration:\n', 1)[1].split('  ffmpeg_integration:\n', 1)[0]
        self.assertIn('os: [ubuntu-latest, macos-latest, windows-latest]', job)
        upload = job.split('          path: |\n', 1)[1].split('          if-no-files-found:', 1)[0]
        self.assertEqual([line.strip() for line in upload.splitlines()], [
            '${{ runner.temp }}/live-transmit/evidence/build-identity.json',
            '${{ runner.temp }}/live-transmit/evidence/runtime/results.json'])
        self.assertNotIn('app-build', upload)
        self.assertNotIn('haivision-source', upload)

    def test_targeted_recipe_changes_select_app_without_core_rebuild_gates(self):
        import sys
        sys.path.insert(0, str(ROOT / 'interop'))
        import ci_changes
        for path in ('tests/live_transmit/CMakeLists.txt', 'tests/live_transmit/prepare_source.py',
                     'tests/live_transmit/overlays/robotweax_app.hpp'):
            with self.subTest(path=path):
                result = ci_changes.classify([path])
                self.assertTrue(result.live_transmit)
                self.assertTrue(result.python)
                self.assertFalse(result.core_tests)
                self.assertFalse(result.portable)
        self.assertTrue(ci_changes.classify(['src/compat/socket_options.cpp']).live_transmit)
        self.assertTrue(ci_changes.classify(['cmake/RobotweaxSRTConfig.cmake.in']).live_transmit)


if __name__ == '__main__':
    unittest.main()
