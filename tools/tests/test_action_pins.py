"""Exercise immutable references through the CLI and actual YAML parser."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
POLICY = ROOT / 'tools/check_action_pins.rb'
SHA = '0123456789abcdef' * 2 + '01234567'


class ActionPinTests(unittest.TestCase):
    def check(self, source, accepted):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'fixture.yml'
            path.write_text(source)
            result = subprocess.run(['ruby', str(POLICY), str(path)],
                                    capture_output=True, text=True)
        self.assertEqual(result.returncode, 0 if accepted else 1,
                         result.stdout + result.stderr)

    def test_pins_and_subactions(self):
        self.check(f'steps:\n  - uses: actions/cache/restore@{SHA} # v6\n', True)

    def test_mutable_tags_branches_and_short_shas(self):
        for ref in ('v7', 'main', SHA[:12], SHA + '0', 'g' * 40):
            with self.subTest(ref=ref):
                self.check(f'steps:\n  - uses: actions/checkout@{ref}\n', False)

    def test_quotes_flow_mappings_and_escaped_keys(self):
        for key in ('uses', "'uses'", '"u\\u0073es"'):
            with self.subTest(key=key):
                self.check(f'steps: [{{{key}: "actions/checkout@v7"}}]\n', False)
                self.check(f'steps: [{{{key}: "actions/checkout@{SHA}"}}]\n', True)

    def test_reusable_workflows(self):
        self.check(f'jobs: {{reuse: {{uses: owner/repo/.github/workflows/ci.yml@{SHA}}}}}\n', True)
        self.check('jobs: {reuse: {uses: owner/repo/.github/workflows/ci.yml@main}}\n', False)

    def test_local_paths(self):
        for ref in ('./.github/workflows/ci.yml', './.github/actions/build'):
            self.check(f'uses: {ref}\n', True)
        for ref in ('./../outside', './actions/../../outside', './', './actions//build'):
            self.check(f'uses: {ref}\n', False)

    def test_container_digest(self):
        self.check('uses: docker://alpine:3@sha256:' + 'a' * 64 + '\n', True)
        self.check('uses: docker://alpine:3\n', False)
        self.check('uses: docker://alpine@sha256:' + 'a' * 63 + '\n', False)

    def test_dynamic_references(self):
        self.check('uses: actions/checkout@${{ inputs.ref }}\n', False)

    def test_block_scalars(self):
        self.check('uses: >-\n  actions/checkout@v7\n', False)
        self.check(f'uses: >-\n  actions/checkout@{SHA}\n', True)
        self.check(f'uses: |\n  actions/checkout@{SHA}\n', False)

    def test_scripts_and_comments_are_not_references(self):
        self.check('steps:\n  - run: |\n      uses: actions/checkout@v7\n'
                   '      echo "uses: actions/checkout@main"\n'
                   '# uses: actions/checkout@v7\n', True)

    def test_aliases_and_merge_keys_fail_closed(self):
        self.check(f'pin: &pin actions/checkout@{SHA}\nuses: *pin\n', False)
        self.check(f'base: &base {{uses: actions/checkout@{SHA}}}\nstep: {{<<: *base}}\n', False)
        self.check('step: {<<: {uses: actions/checkout@v7}}\n', False)

    def test_duplicate_keys_fail_closed(self):
        self.check(f'uses: actions/checkout@v7\nuses: actions/checkout@{SHA}\n', False)
        self.check(f'uses: actions/checkout@{SHA}\nuses: actions/checkout@v7\n', False)

    def test_non_scalar_and_tagged_references(self):
        for value in ('[actions/checkout@v7]', '{ref: main}', '', f'!custom actions/checkout@{SHA}'):
            self.check(f'uses: {value}\n', False)

    def test_invalid_yaml_or_multiple_documents_fail(self):
        for source in ('steps: [\n', '---\nuses: ./local\n---\nuses: ./local\n', '[]\n', ''):
            self.check(source, False)

    def test_symlink_is_not_followed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / 'target.yml'
            target.write_text(f'uses: actions/checkout@{SHA}\n')
            link = root / 'link.yml'
            link.symlink_to(target)
            result = subprocess.run(['ruby', str(POLICY), str(link)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn('symlinked manifests', result.stderr)

    def test_inventory_includes_nested_and_outside_github_actions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subprocess.run(['git', 'init', '-q', directory], check=True)
            names = ('.github/actions/nested/action.yml', '.github/workflows/ci.yaml', 'local/action.yaml')
            for name in names + ('notes.yaml',):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('uses: actions/checkout@v7\n')
                subprocess.run(['git', 'add', name], cwd=root, check=True)
            for name in sorted(names):
                result = subprocess.run(['ruby', str(POLICY)], cwd=root, capture_output=True, text=True)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(name, result.stderr)
                (root / name).write_text(f'uses: actions/checkout@{SHA}\n')
            result = subprocess.run(['ruby', str(POLICY)], cwd=root, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('3 manifests, 3 uses references', result.stdout)

    def test_empty_inventory_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run(['git', 'init', '-q', directory], check=True)
            result = subprocess.run(['ruby', str(POLICY)], cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 1, result.stderr)

    def test_policy_precedes_classification_without_skip_condition(self):
        changes = (ROOT / '.github/workflows/ci.yml').read_text().split('\n  changes:\n', 1)[1]
        step = changes.split('      - name: Enforce immutable action references\n', 1)[1].split('      - name:', 1)[0]
        self.assertNotIn('if:', step)
        self.assertIn('ruby tools/check_action_pins.rb', step)
        self.assertIn("-s tools/tests -p 'test_action_pins.py'", step)
        self.assertLess(changes.index('ruby tools/check_action_pins.rb'), changes.index('interop/ci_changes.py'))


if __name__ == '__main__':
    unittest.main()
