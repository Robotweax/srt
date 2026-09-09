"""Static guardrails for the Windows-only SDK build workflow."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WindowsSdkTests(unittest.TestCase):
    def test_public_c_consumer_uses_c11(self):
        script = (ROOT / 'packaging/windows/build-ci.ps1').read_text()
        self.assertIn("Run cl @('/nologo','/std:c11'", script)

    def test_msvc_crypto_configurations_are_explicit(self):
        script = (ROOT / 'packaging/windows/build-sdk.ps1').read_text()
        for config in ('DEBUG', 'RELEASE'):
            self.assertIn(f'-DLIB_EAY_{config}:FILEPATH=$Crypto', script)
        self.assertIn('Unexpected OpenSSL selection', script)

    def test_child_shell_uses_same_powershell_runtime(self):
        workflow = (ROOT / '.github/workflows/windows-sdk.yml').read_text()
        self.assertIn("Join-Path $PSHOME 'pwsh.exe'", workflow)
        self.assertNotIn('&& powershell ', workflow)
