#!/usr/bin/env python3
"""Add opt-in diagnostics to the exact pinned OBS desktop sources."""

import argparse
import hashlib
from pathlib import Path

HASHES = {
    "libobs/util/bmem.c": "910ad872bf7e4350317b5b30adb79a3fb8484f97d7ce29af48419ef4a4f2abdf",
    "frontend/OBSApp.cpp": "bc298358c9fd2b8e7e8ca7952108adfeafe6243df4718b854003e33e96372963",
    "frontend/widgets/OBSBasic_Hotkeys.cpp": "8ceea89edb53d1a073cf9b554b579f13f8ad772dff648e718549d331d97673bf",
}


def replacements(path):
    if path.endswith("bmem.c"):
        header = Path(__file__).with_name("trace_allocations.h").resolve().as_posix()
        return [
            ("static long num_allocs = 0;", f'static long num_allocs = 0;\n#include "{header}"'),
            ("os_atomic_inc_long(&num_allocs);\n\treturn ptr;", "os_atomic_inc_long(&num_allocs);\n\trw_allocation_update(NULL, ptr, size);\n\treturn ptr;"),
            ("\tptr = a_realloc(ptr, size);", "\trw_allocation_update(ptr, NULL, 0);\n\tptr = a_realloc(ptr, size);"),
            ("\n\treturn ptr;\n}\n\nvoid bfree", "\n\trw_allocation_update(NULL, ptr, size);\n\treturn ptr;\n}\n\nvoid bfree"),
            ("\t\ta_free(ptr);", "\t\trw_allocation_update(ptr, NULL, 0);\n\t\ta_free(ptr);"),
            ("\treturn num_allocs;", "\trw_allocation_dump();\n\treturn num_allocs;"),
        ]
    if path.endswith("OBSApp.cpp"):
        return [("\t\t\tobs_hotkey_inject_event(hotkey, pressed);\n\t\t\treturn true;\n\t\t};\n\n\t\tswitch", "\t\t\tif (getenv(\"ROBOTWEAX_OBS_TRACE_HOTKEYS\"))\n\t\t\t\tblog(LOG_INFO, \"OBS_KEY_EVENT native=%u key=%d modifiers=%u pressed=%d focus=%d\", event->nativeVirtualKey(), (int)hotkey.key, hotkey.modifiers, pressed, enabledInFocus);\n\t\t\tobs_hotkey_inject_event(hotkey, pressed);\n\t\t\treturn true;\n\t\t};\n\n\t\tswitch")]
    return [
        ("\t\tconst char *info = config_get_string(activeConfiguration, \"Hotkeys\", name);", "\t\tconst char *info = config_get_string(activeConfiguration, \"Hotkeys\", name);\n\t\tif (getenv(\"ROBOTWEAX_OBS_TRACE_HOTKEYS\") && strstr(name, \"Streaming\"))\n\t\t\tblog(LOG_INFO, \"OBS_HOTKEY_BINDING %s %s\", name, info ? info : \"<missing>\");"),
        ("\tQMetaObject::invokeMethod(&basic, \"ProcessHotkey\",", "\tif (getenv(\"ROBOTWEAX_OBS_TRACE_HOTKEYS\"))\n\t\tblog(LOG_INFO, \"OBS_HOTKEY_ROUTED id=%zu pressed=%d\", (size_t)id, pressed);\n\tQMetaObject::invokeMethod(&basic, \"ProcessHotkey\","),
    ]


def prepare(source):
    pending = []
    for path, expected in HASHES.items():
        target = source / path
        current = target.read_text().replace("\r\n", "\n")
        changes = replacements(path)
        restored = current
        for old, new in reversed(changes):
            restored = restored.replace(new, old)
        if hashlib.sha256(restored.encode()).hexdigest() != expected:
            raise RuntimeError(f"unrecognized OBS diagnostic source: {path}")
        fixed = restored
        for old, new in changes:
            if fixed.count(old) != 1:
                raise RuntimeError(f"ambiguous OBS diagnostic insertion: {path}")
            fixed = fixed.replace(old, new)
        if current not in (restored, fixed):
            raise RuntimeError(f"partially applied OBS diagnostics: {path}")
        pending.append((target, fixed))
    for target, fixed in pending:
        target.write_text(fixed)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    prepare(parser.parse_args().source)
