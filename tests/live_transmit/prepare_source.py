#!/usr/bin/env python3
"""Prepare a pinned external Haivision application for the installed Robotweax SDK."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent
REVISION = "899348d8318eb9a3c5a5b6ec43c4a1114288773a"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def verified_inputs(source: Path, hashes: dict[str, str]) -> dict[str, bytes]:
    source = source.resolve()
    result = {}
    for name, expected in hashes.items():
        path = source / name
        if path.is_symlink() or any(p.is_symlink() for p in path.parents if p != source.parent):
            raise RuntimeError(f"source symlink rejected: {name}")
        data = path.read_bytes()
        if digest(data) != expected:
            raise RuntimeError(f"unrecognized source bytes: {name}")
        result[name] = data
    return result


def replace(text: str, old: str, new: str, count: int = 1) -> str:
    if text.count(old) != count:
        raise RuntimeError(f"unexpected pinned source marker: {old!r}")
    return text.replace(old, new)


def adapt(name: str, text: str) -> str:
    # These transformations affect only the external application copy. No
    # Haivision transport source or private C++ library API is compiled.
    for old, new in (
        ('"../srtcore/srt.h"', '<srt/srt.h>'),
        ('"../srtcore/logging_api.h"', '<srt/logging_api.h>'),
        ('<udt.h>', '<srt/srt.h>'),
        ('<logging.h>', '<srt/logging_api.h>'),
        ('"sync.h"', '"app_sync.hpp"'),
        ('srt_logging::LogFA', 'int'),
        ('set<LogFA>', 'set<int>'),
        ('SRT_ATR_UNUSED', '[[maybe_unused]]'),
    ):
        text = text.replace(old, new)
    if name == 'apps/apputil.cpp':
        text = replace(text, 'void PrintLibVersion()\n{',
                       'void PrintLibVersion()\n{\n    cerr << "Transport provider: Robotweax SRT; arqfirst=yes (tail probe off)" << endl;')
        text = replace(text, '<< SRT_VERSION ', '<< SRT_VERSION_STRING ')
    elif name == 'apps/logsupport.cpp':
        # The public SDK severity range begins at LOG_CRIT on every platform.
        text = replace(text, 'LOG_ALERT', 'LOG_CRIT')
        text = replace(text, 'LOG_EMERG', 'LOG_CRIT', count=2)
    elif name == 'apps/logsupport.hpp':
        text = replace(text, '#include <string>', '#include <string>\n#include <set>\n#include <iostream>')
    elif name == 'apps/socketoptions.hpp':
        text = replace(text, '#include <vector>', '#include <vector>\n#include "robotweax_app.hpp"')
        marker = '    { "packetfilter", 0, SRTO_PACKETFILTER, SocketOption::PRE, SocketOption::STRING, nullptr },'
        text = replace(text, marker, marker + '\n    { "arqfirst", 0, SRTO_ROBOTWEAX_FECPOLICY, SocketOption::PRE, SocketOption::ENUM, &robotweax_app::arqfirst_values },')
    elif name == 'apps/socketoptions.cpp':
        marker = '    return all_clear ? mode : SocketOption::FAILURE;'
        text = replace(text, '<< value;', '<< robotweax_app::display_option(o.name, value);')
        text = replace(text, marker,
                       '    if (all_clear && options.count("arqfirst"))\n        Verb() << robotweax_app::policy_description(socket);\n' + marker)
    elif name == 'apps/srt-live-transmit.cpp':
        marker = '        cerr << "Supported schemes:\\n";'
        text = replace(text, '#include <srt.h>', '#include <srt.h>\n#include \"robotweax_app.hpp\"')
        text = replace(text, '<< cfg.source', '<< robotweax_app::display_uri(cfg.source)')
        text = replace(text, '<< cfg.target', '<< robotweax_app::display_uri(cfg.target)')
        text = replace(text, marker,
                       '        cerr << "Robotweax SRT output option: arqfirst=yes; Live Caller/Listener, Matrix 8x8 default, tail probe off.\\n";\n' + marker)
        marker = '    char NAME[] = "SRTLIB";'
        text = replace(text, marker,
                       '    struct ResetLogHandler {\n        ~ResetLogHandler() { srt_setloghandler(nullptr, nullptr); }\n    } reset_log_handler;\n' + marker)
        text = replace(text, 'srt::setlogstream(logfile_stream);',
                       'srt_setloghandler(&logfile_stream, [](void* opaque, int, const char*, int, const char*, const char* message) {\n                *static_cast<std::ofstream*>(opaque) << message << std::flush;\n            });')
    elif name == 'apps/transmitmedia.cpp':
        text = replace(text, '#include <stdexcept>', '#include <stdexcept>\n#include <system_error>\n#include "robotweax_app.hpp"')
        marker = 'void SrtCommon::Init(string host, int port, map<string,string> par, bool dir_output)\n{'
        text = replace(text, marker, marker + '\n    robotweax_app::configure_uri(par, dir_output);')
        marker = '    InitParameters(host, par);\n\n    Verb() << \"Opening SRT \"'
        text = replace(text, marker, '    InitParameters(host, par);\n    robotweax_app::validate_mode(par, m_mode);\n\n    Verb() << \"Opening SRT \"')
        text = replace(text, '<< i->second', '<< robotweax_app::display_option(i->first, i->second)')
        text = replace(text, 'srt::setstreamid(m_sock, w_name);',
                       'if (srt_setsockflag(m_sock, SRTO_STREAMID, w_name.data(), static_cast<int>(w_name.size())) == SRT_ERROR)\n                Error("set Stream ID");')
        text = replace(text, 'w_name = srt::getstreamid(m_sock);',
                       'char streamid[513] = {};\n        int streamid_size = sizeof(streamid);\n        if (srt_getsockflag(m_sock, SRTO_STREAMID, streamid, &streamid_size) == SRT_ERROR)\n            Error("get Stream ID");\n        w_name.assign(streamid, static_cast<size_t>(streamid_size));')
        text = replace(text, 'SysStrError(err, buf, 512u)', 'std::system_category().message(err)')
    return text


def prepare(source: Path, output: Path) -> None:
    source, output = source.resolve(), output.resolve()
    if source == output or source in output.parents or output in source.parents:
        raise RuntimeError("source and generated output must be separate trees")
    revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != REVISION:
        raise RuntimeError(f"expected Haivision {REVISION}, found {revision}")
    manifest = json.loads((ROOT / 'source-manifest.json').read_text())
    inputs = verified_inputs(source, manifest['files'])
    prepared = {name: (adapt(name, data.decode()).encode() if name.startswith('apps/') else data)
                for name, data in inputs.items()}
    for overlay in (ROOT / 'overlays').glob('*.hpp'):
        prepared['apps/' + overlay.name] = overlay.read_bytes()
    identity = {'upstream_revision': REVISION,
                'files': {name: digest(data) for name, data in prepared.items()}}
    if output.exists():
        existing = {str(p.relative_to(output)).replace('\\', '/'): p for p in output.rglob('*') if p.is_file()}
        if set(existing) == set(prepared) | {'prepared-manifest.json'} and all(
            not existing[name].is_symlink() and existing[name].read_bytes() == data
            for name, data in prepared.items()
        ) and json.loads((output / 'prepared-manifest.json').read_text()) == identity:
            return
        raise RuntimeError("generated output already exists with different content; use a fresh directory")
    # All inputs and transformations have been checked before writing any output.
    output.mkdir(parents=True)
    try:
        for name, data in prepared.items():
            target = output / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        (output / 'prepared-manifest.json').write_text(json.dumps(identity, indent=2) + '\n')
    except BaseException:
        shutil.rmtree(output)
        raise


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        prepare(args.source, args.output)
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"application preparation failed: {error}\n")
    print(f"Prepared external application: {args.output}")


if __name__ == '__main__':
    main()
