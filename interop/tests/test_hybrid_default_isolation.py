"""Default builds must not call into the experimental hybrid control layer.

The hybrid control sources are compiled in every build, so nothing but source
structure keeps a default build from running them. Every call from the rest
of the library into the ``hybrid`` namespace must sit inside a block that is
compiled only with ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL. Type names such
as ``hybrid::DataPath`` are not calls and stay allowed.
"""

from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
GUARD = "ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL"
HYBRID_SOURCES = ROOT / "src/compat/hybrid"
# Functions in this code base are snake_case and types are CamelCase. A call
# is a free function or a static member reached through the namespace.
CALL = re.compile(
    r"\bhybrid::(?:[A-Za-z_][A-Za-z_0-9]*::)*[a-z_][a-z_0-9]*\s*\(")
IFDEF = re.compile(r"#\s*ifdef\s+" + GUARD + r"\b")
IFNDEF = re.compile(r"#\s*ifndef\s+" + GUARD + r"\b")
IF_DEFINED = re.compile(r"#\s*if\s+defined\s*\(?\s*" + GUARD + r"\s*\)?\s*$")
IF_NOT_DEFINED = re.compile(
    r"#\s*if\s+!\s*defined\s*\(?\s*" + GUARD + r"\s*\)?\s*$")


def unguarded_calls(text: str) -> list[tuple[int, str]]:
    """Return (line number, line) for hybrid calls a default build compiles."""
    found: list[tuple[int, str]] = []
    # One entry per open conditional: "on" is compiled only with the option,
    # "off" only without it, "other" does not depend on it.
    stack: list[str] = []
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if stripped.startswith("#"):
            directive = re.sub(r"\s*//.*$", "", stripped)
            if IFDEF.match(directive) or IF_DEFINED.match(directive):
                stack.append("on")
            elif IFNDEF.match(directive) or IF_NOT_DEFINED.match(directive):
                stack.append("off")
            elif re.match(r"#\s*if", directive):
                stack.append("other")
            elif re.match(r"#\s*else\b", directive):
                if not stack:
                    raise ValueError(f"line {number}: #else without #if")
                stack[-1] = {"on": "off", "off": "on"}.get(stack[-1], "other")
            elif re.match(r"#\s*elif", directive):
                if not stack:
                    raise ValueError(f"line {number}: #elif without #if")
                stack[-1] = "other"
            elif re.match(r"#\s*endif\b", directive):
                if not stack:
                    raise ValueError(f"line {number}: #endif without #if")
                stack.pop()
            continue
        code = re.sub(r"//.*$", "", line)
        if "on" not in stack and CALL.search(code):
            found.append((number, stripped))
    if stack:
        raise ValueError("unterminated preprocessor conditional")
    return found


class HybridDefaultIsolationTests(unittest.TestCase):
    def test_scanner_tracks_the_experimental_guard(self) -> None:
        guarded = (
            "void f() {\n"
            f"#ifdef {GUARD}\n"
            "    if (hybrid::native_candidate(p)) return;\n"
            "#endif\n"
            "}\n"
        )
        self.assertEqual(unguarded_calls(guarded), [])
        self.assertEqual(
            unguarded_calls(guarded.replace(f"#ifdef {GUARD}", "#if 1")),
            [(3, "if (hybrid::native_candidate(p)) return;")],
        )
        half_guarded = (
            "if (hybrid::native_candidate(p)) {\n"
            f"#ifdef {GUARD}\n"
            "    service->enqueue(p);\n"
            "#endif\n"
            "    return;\n"
            "}\n"
        )
        self.assertEqual(
            [number for number, _ in unguarded_calls(half_guarded)], [1])
        inverted = (
            f"#ifndef {GUARD}\n"
            "a = hybrid::decode(x);\n"
            "#else\n"
            "b = hybrid::decode(x);\n"
            "#endif\n"
        )
        self.assertEqual(
            [number for number, _ in unguarded_calls(inverted)], [2])
        nested = (
            f"#if defined({GUARD})\n"
            "#ifdef OTHER\n"
            "a = hybrid::decode(x);\n"
            "#else\n"
            "b = hybrid::decode(x);\n"
            "#endif\n"
            "#endif\n"
        )
        self.assertEqual(unguarded_calls(nested), [])

    def test_scanner_ignores_types_and_comments(self) -> None:
        text = (
            "std::optional<hybrid::DataPath> path;\n"
            "std::shared_ptr<hybrid::NativeService> service;\n"
            "// hybrid::native_candidate(packet) is experimental\n"
            "void f(std::optional<hybrid::DataPath> path) noexcept;\n"
        )
        self.assertEqual(unguarded_calls(text), [])

    def test_scanner_reports_static_member_calls(self) -> None:
        text = "auto at = hybrid::NativeService::monotonic_now();\n"
        self.assertEqual([number for number, _ in unguarded_calls(text)], [1])

    def test_library_calls_hybrid_control_only_behind_the_option(self) -> None:
        sources = sorted(
            path for path in (ROOT / "src").rglob("*")
            if path.suffix in {".cpp", ".hpp"}
            and HYBRID_SOURCES not in path.parents
        )
        self.assertGreater(len(sources), 50)
        self.assertTrue(HYBRID_SOURCES.is_dir())
        offenders = [
            f"{path.relative_to(ROOT)}:{number}: {line}"
            for path in sources
            for number, line in unguarded_calls(path.read_text())
        ]
        self.assertEqual(offenders, [])


if __name__ == "__main__":
    unittest.main()
