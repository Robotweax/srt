#!/usr/bin/env python3
"""Verify dependency-version/configuration rejection without changing the dependency."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--mbedtls-root',type=Path,required=True)
    args=parser.parse_args()
    for variant in ['old','major4','no-gcm','aes128-only','null-entropy','aes-encrypt-alt']:
        with tempfile.TemporaryDirectory(prefix='srt-mbedtls-config-') as temporary:
            root=Path(temporary)
            shutil.copytree(args.mbedtls_root/'include',root/'include')
            (root/'lib').mkdir()
            library=next((args.mbedtls_root/'lib').glob('*mbedcrypto*'))
            shutil.copyfile(library,root/'lib'/library.name)
            if variant in ['old','major4']:
                header=root/'include/mbedtls/build_info.h'
                text=header.read_text()
                text=re.sub(r'(#define MBEDTLS_VERSION_STRING\s+)"[^"]+"',
                    r'\g<1>"'+('3.6.6' if variant=='old' else '4.1.1')+'"',text)
                header.write_text(text)
            else:
                header=root/'include/mbedtls/mbedtls_config.h'
                text=header.read_text()
                if variant=='no-gcm':
                    assert '#define MBEDTLS_GCM_C' in text
                    text=text.replace('#define MBEDTLS_GCM_C','//#define MBEDTLS_GCM_C')
                elif variant=='aes128-only':
                    text+='\n#define MBEDTLS_AES_ONLY_128_BIT_KEY_LENGTH\n'
                elif variant=='null-entropy':
                    text+='\n#define MBEDTLS_TEST_NULL_ENTROPY\n'
                else:
                    text+='\n#define MBEDTLS_AES_ENCRYPT_ALT\n'
                header.write_text(text)
            result=subprocess.run(['cmake','-S',str(args.source.resolve()),'-B',str(root/'build'),
                '-DROBOTWEAX_SRT_CRYPTO_BACKEND=mbedtls',f'-DMbedTLS_ROOT={root}',
                '-DROBOTWEAX_SRT_BUILD_TESTS=OFF','-DROBOTWEAX_SRT_BUILD_TOOLS=OFF',
                '-DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF','-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON'],
                stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
            assert result.returncode!=0, f'{variant} was silently accepted'
            assert ('Could NOT find MbedTLS' in result.stdout if variant in ['old','major4']
                else 'Unsupported Mbed TLS configuration' in result.stdout),result.stdout
            print(f'PASS reject-{variant}')

if __name__=='__main__':
    main()
