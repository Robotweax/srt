"""Static guardrails for the Windows-only SDK build workflow."""
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]


class WindowsSdkTests(unittest.TestCase):
    def test_crypto_assembly_is_required_for_every_target(self):
        script = (ROOT / 'packaging/windows/build-ci.ps1').read_text()
        self.assertNotIn("'no-asm'", script)
        self.assertIn('VC-WIN64-CLANGASM-ARM', script)
        self.assertIn('nasm.exe', script)
        self.assertIn('clang-cl.exe', script)
        self.assertIn('Required assembler missing', script)
        self.assertIn('$configdata::disabled{asm}', script)
        self.assertIn('$configdata::target{asm_arch}', script)

    def test_msbuild_probe_imports_shipped_props(self):
        project = ET.parse(ROOT / 'packaging/windows/consumer/consumer.vcxproj')
        namespace = {'m': 'http://schemas.microsoft.com/developer/msbuild/2003'}
        imports = [entry.attrib['Project'] for entry in project.findall('m:Import', namespace)]
        self.assertIn('$(ROBOTWEAX_SRT)\\srt.props', imports)
        toolset = project.find('m:PropertyGroup[@Label="Configuration"]/m:PlatformToolset', namespace)
        self.assertIsNotNone(toolset)
        self.assertEqual(toolset.text, '$(DefaultPlatformToolset)')
        ET.parse(ROOT / 'packaging/windows/srt.props')

    def test_existing_install_is_guarded_independent_of_destination(self):
        script = (ROOT / 'packaging/windows/sdk.iss').read_text()
        self.assertIn('RegKeyExists(HKLM,', script)
        self.assertIn('Robotweax.SRT.SDK_is1', script)

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
