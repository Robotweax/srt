from __future__ import annotations

import os
import re
import subprocess
import textwrap
import unittest
from pathlib import Path


WORKFLOW = Path(__file__).resolve().parents[2] / ".github/workflows/ci.yml"
JOB_ID = r"[A-Za-z_][A-Za-z0-9_-]*"


def job_blocks(workflow: str) -> dict[str, str]:
    # This checks the repository's explicit block-style workflow contract,
    # not arbitrary YAML. A layout change must update this guard explicitly.
    jobs = workflow.split("\njobs:\n", 1)[1]
    return dict(re.findall(
        rf"^  ({JOB_ID}):\n(.*?)(?=^  {JOB_ID}:\n|\Z)",
        jobs,
        re.MULTILINE | re.DOTALL,
    ))


def dependencies(job: str) -> set[str]:
    needs = re.search(rf"^    needs:\n((?:      - {JOB_ID}\n)+)",
                      job, re.MULTILINE)
    if needs is None:
        raise ValueError("gate must declare explicit job dependencies")
    return set(re.findall(rf"- ({JOB_ID})", needs[1]))


def result_bindings(job: str) -> dict[str, str]:
    return dict(re.findall(
        r"^          ([A-Z_]+): \$\{\{ needs\."
        rf"({JOB_ID})\.result \}}\}}$",
        job, re.MULTILINE,
    ))


def gate_script(job: str) -> str:
    return textwrap.dedent(job.split("        run: |\n", 1)[1])


def skip_expectations(job: str) -> dict[str, str]:
    return dict(re.findall(r"^          (EXPECT_[a-z_]+): (.+)$",
                           job, re.MULTILINE))


def validate_gate_coverage(workflow: str) -> None:
    jobs = job_blocks(workflow)
    gate = jobs["ci_gate"]
    # Preparation and matrix results are deliberately covered by their
    # strict reference aggregate, rather than being accepted as skipped.
    delegated = {"reference_interop_prepare", "reference_interop_shard"}
    expected = set(jobs) - {"ci_gate"} - delegated
    if dependencies(gate) != expected:
        raise ValueError("CI jobs missing from gate dependencies")
    bindings = result_bindings(gate)
    if set(bindings.values()) != expected or len(bindings) != len(expected):
        raise ValueError("CI job results missing from gate environment")
    script = gate_script(gate)
    entries = re.findall(r'"[a-z-]+=\$([A-Z_]+)"', script)
    if set(entries) != set(bindings) or len(entries) != len(bindings):
        raise ValueError("CI job results missing from gate decision")
    if "    if: always()\n" not in gate:
        raise ValueError("gate must run after failed or cancelled jobs")
    reference = jobs["reference_interop"]
    if dependencies(reference) != delegated | {"changes"}:
        raise ValueError("reference gate must cover preparation and shards")
    if result_bindings(reference) != {
        "PREPARE": "reference_interop_prepare",
        "SHARDS": "reference_interop_shard",
    }:
        raise ValueError("reference gate result bindings changed")
    if "always()" not in reference:
        raise ValueError("reference gate must inspect failed dependencies")


class RequiredCiGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.workflow = WORKFLOW.read_text(encoding="utf-8")

    def test_every_job_is_covered(self) -> None:
        validate_gate_coverage(self.workflow)

    def test_expected_selections_match_actual_job_conditions(self) -> None:
        jobs = job_blocks(self.workflow)
        expectations = skip_expectations(jobs["ci_gate"])
        self.assertEqual(set(expectations),
                         {"EXPECT_" + name for name in dependencies(jobs["ci_gate"])})

        def normalized(expression: str) -> str:
            expression = expression.replace("always()", "").replace("${{", "")
            expression = expression.replace("}}", "").replace(">-", "")
            expression = expression.replace("(", "").replace(")", "")
            return "".join(expression.split()).removeprefix("&&")

        for variable, expression in expectations.items():
            name = variable.removeprefix("EXPECT_")
            condition = re.search(r"^    if: (.*(?:\n      [^\n]+)*)",
                                  jobs[name], re.MULTILINE)
            with self.subTest(job=name):
                expected = "'true'" if condition is None else condition[1]
                self.assertEqual(normalized(expression), normalized(expected))

    def test_selected_jobs_cannot_be_skipped(self) -> None:
        gate = job_blocks(self.workflow)["ci_gate"]
        bindings = result_bindings(gate)
        expectations = skip_expectations(gate)
        for variable, job_name in bindings.items():
            for selected in ("true", "unexpected", ""):
                with self.subTest(job=job_name, selected=selected):
                    result = subprocess.run(
                        ["bash", "-c", gate_script(gate)],
                        env={**os.environ, **dict.fromkeys(bindings, "success"),
                             **dict.fromkeys(expectations, "false"),
                             variable: "skipped", "EXPECT_" + job_name: selected},
                        capture_output=True, text=True, timeout=5,
                    )
                    self.assertNotEqual(result.returncode, 0, result.stdout)

    def test_missing_dependency_binding_or_decision_is_rejected(self) -> None:
        for removed in (
            "      - ffmpeg_integration\n",
            "          FFMPEG_INTEGRATION: "
            "${{ needs.ffmpeg_integration.result }}\n",
            '            "ffmpeg-integration=$FFMPEG_INTEGRATION" \\\n',
        ):
            with self.subTest(removed=removed):
                self.assertIn(removed, self.workflow)
                with self.assertRaises(ValueError):
                    validate_gate_coverage(self.workflow.replace(removed, ""))

    def test_new_uncovered_job_is_rejected(self) -> None:
        for name in ("new_check", "new-check2", "NewCheck3"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                validate_gate_coverage(
                    self.workflow
                    + f"\n  {name}:\n    runs-on: ubuntu-latest\n"
                )

    def test_actual_gate_shell_rejects_each_nonpassing_result(self) -> None:
        job = job_blocks(self.workflow)["ci_gate"]
        variables = result_bindings(job)
        for variable in variables:
            for result in ("success", "skipped", "failure", "cancelled",
                           "timed_out", "", "unexpected"):
                with self.subTest(variable=variable, result=result):
                    environment = dict(os.environ)
                    environment.update(dict.fromkeys(variables, "success"))
                    environment.update(dict.fromkeys(skip_expectations(job), "false"))
                    environment[variable] = result
                    process = subprocess.run(
                        ["bash", "-c", gate_script(job)], env=environment,
                        capture_output=True, text=True, timeout=5,
                    )
                    self.assertEqual(
                        process.returncode,
                        0 if result in {"success", "skipped"} else 1,
                        process.stdout + process.stderr,
                    )

    def test_reference_gate_requires_both_results_to_succeed(self) -> None:
        job = job_blocks(self.workflow)["reference_interop"]
        for prepare in ("success", "skipped", "failure", "cancelled", ""):
            for shards in ("success", "skipped", "failure", "cancelled", ""):
                with self.subTest(prepare=prepare, shards=shards):
                    process = subprocess.run(
                        ["bash", "-c", gate_script(job)],
                        env={**os.environ, "PREPARE": prepare, "SHARDS": shards},
                        capture_output=True, text=True, timeout=5,
                    )
                    self.assertEqual(
                        process.returncode,
                        0 if prepare == shards == "success" else 1,
                        process.stdout + process.stderr,
                    )


if __name__ == "__main__":
    unittest.main()
