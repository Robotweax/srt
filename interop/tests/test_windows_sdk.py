"""Static guardrails for the Windows-only SDK build workflow."""
from pathlib import Path
import re
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

    def test_signing_workflow_uses_only_immutable_actions(self):
        workflow = (ROOT / '.github/workflows/windows-sdk.yml').read_text()
        references = re.findall(r'uses: ([^\s]+)', workflow)
        self.assertTrue(references)
        for reference in references:
            self.assertRegex(reference, r'^[^@]+@[0-9a-f]{40}$')

    def test_unsigned_provenance_is_verified_before_oidc(self):
        workflow = (ROOT / '.github/workflows/windows-sdk.yml').read_text()
        signing = workflow.split('\n  sign:\n', 1)[1].split('\n  release-draft:\n', 1)[0]
        self.assertIn('needs.rebuild.outputs.installer-provenance', signing)
        self.assertLess(signing.index('Assert-SdkInstallerProvenance'), signing.index('uses: azure/login@'))


    def test_signer_consumes_only_independent_rebuild_artifact_id(self):
        workflow = (ROOT / '.github/workflows/windows-sdk.yml').read_text()
        rebuild = workflow.split('\n  rebuild:\n', 1)[1].split('\n  sign:\n', 1)[0]
        signing = workflow.split('\n  sign:\n', 1)[1].split('\n  release-draft:\n', 1)[0]
        self.assertIn('environment: windows-release-signing', rebuild)
        self.assertIn('contents: read', rebuild)
        self.assertNotIn('id-token:', rebuild)
        self.assertNotIn('download-artifact', rebuild)
        self.assertNotIn('actions/cache', rebuild)
        self.assertIn('ref: ${{ github.sha }}', rebuild)
        self.assertIn('rebuild-installers.ps1', rebuild)
        self.assertIn('overwrite: false', rebuild)
        self.assertIn('artifact-ids: ${{ needs.rebuild.outputs.artifact-id }}', signing)
        self.assertNotIn('pattern:', signing)
        self.assertNotIn('needs.coexistence', signing)
        qualification = workflow.split('\n  rebuild-qualification:\n', 1)[1].split('\n  rebuild:\n', 1)[0]
        self.assertIn('rebuild-installers.ps1', qualification)
        self.assertNotIn('environment:', qualification)
        self.assertNotIn('id-token:', qualification)
