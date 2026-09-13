#!/usr/bin/env python3
"""Fixed 32-case A/B mechanism diagnosis; never nominates a product candidate."""
from __future__ import annotations
import argparse, contextlib, json, platform, signal, socket, statistics
from pathlib import Path
import run_pacer_continuation_ab as plain
import continuation_diagnostics as cd

common, build, diag, sc = plain.common, plain.build, plain.diag, plain.sc
REVISIONS = cd.REVISIONS
PROFILES = plain.PROFILES
REPORT = "continuation-diagnostics-report.json"


def plan():
    return [
        {"variant": v, "capture": c}
        for v, c in [
            ("baseline", "none"),
            ("baseline", "continuation"),
            ("candidate", "none"),
            ("candidate", "continuation"),
            ("candidate", "continuation"),
            ("candidate", "none"),
            ("baseline", "continuation"),
            ("baseline", "none"),
        ]
    ]


def build_key(spec):
    return spec["variant"] + ("-plain" if spec["capture"] == "none" else "-counters")


def arguments(spec):
    return {
        **plain.budget.arguments(plain.STAGE),
        "capture": spec["capture"],
        "repetitions": 2 if spec["capture"] == "none" else 1,
    }


def analyze_block(raw, manifest, exit_code, spec, directory):
    stage = {**plain.STAGE, "repetitions": 2 if spec["capture"] == "none" else 1}
    result = plain.analyze_block(
        raw, manifest, exit_code, stage=stage, capture=spec["capture"]
    )
    if spec["capture"] == "continuation":
        for row, entry in zip(result["cases"], raw.get("runs", [])):
            case_dir = directory / f"{row['index']:02}-{row['profile']}-{row['kind']}"
            counters = cd.analyze_case(
                case_dir,
                entry.get("result", {}),
                row["profile"],
                REVISIONS[spec["variant"]],
            )
            row["counter_diagnostics"] = counters
            if counters.get("valid") is not True or entry.get("profiling") != counters:
                message = f"case {row['index']}: counter evidence incomplete or differs from raw worker files"
                row["errors"].append(message)
                result["errors"].append(message)
        result["measurement_contract_pass"] = not result["errors"]
    return result


def analyze_experiment(report):
    blocks = report["blocks"]
    expected = plan()
    complete = (
        report.get("complete") is True
        and report.get("interrupted") is False
        and [{k: b[k] for k in ("variant", "capture")} for b in blocks] == expected
    )
    contract = complete and all(
        b["analysis"]["measurement_contract_pass"] for b in blocks
    )
    result = {
        "measurement_contract_pass": contract,
        "diagnosis_valid": False,
        "plain_comparison": None,
        "diagnostic_profiles": [],
        "followup_candidates": [],
        "candidate_nomination_performed": False,
        "long_transfer_qualification_performed": False,
        "errors": [],
        "decision": "mechanism diagnosis only; no product nomination or automatic follow-up",
    }
    if not contract:
        result["errors"].append(
            "incomplete/failed 32-case evidence; preserve partial records"
        )
        return result
    pb = [b for b in blocks if b["capture"] == "none"]
    a = plain.analyze_experiment({"blocks": pb, "complete": True, "interrupted": False})
    result["plain_comparison"] = {
        k: a[k]
        for k in [
            "measurement_contract_pass",
            "comparison_valid",
            "haivision_controls",
            "control_spread",
            "profiles",
            "errors",
        ]
    }
    result["errors"].extend(a["errors"])
    for profile in PROFILES:
        roles = ["sender", "receiver"] if profile == "robotweax-self" else ["sender"]
        for role in roles:
            variants = {}
            for variant in REVISIONS:
                rows = [
                    r["counter_diagnostics"]["endpoints"][role]
                    for b in blocks
                    if b["capture"] == "continuation" and b["variant"] == variant
                    for r in b["analysis"]["cases"]
                    if r["profile"] == profile
                ]
                if len(rows) != 2:
                    raise ValueError("missing diagnostic sample")
                variants[variant] = {
                    "samples": rows,
                    "median_per_original_packet": {
                        k: statistics.median(r["per_original_packet"][k] for r in rows)
                        for k in cd.NAMES
                    },
                }
            ratios = {
                k: variants["candidate"]["median_per_original_packet"][k]
                / variants["baseline"]["median_per_original_packet"][k]
                if variants["baseline"]["median_per_original_packet"][k]
                else None
                for k in cd.NAMES
            }
            result["diagnostic_profiles"].append(
                {
                    "profile": profile,
                    "role": role,
                    "variants": variants,
                    "candidate_over_baseline_counter_ratios": ratios,
                }
            )
    result["diagnosis_valid"] = not result["errors"]
    return result


def validate_manifests(manifests, harness, environment, *, check_exports=True):
    revisions = {
        v + "-" + mode: pin
        for v, pin in REVISIONS.items()
        for mode in ("plain", "counters")
    }
    if set(manifests) != set(revisions):
        raise ValueError("four fresh A/B plain/counter build manifests required")
    plain_views = {}
    for name, manifest in manifests.items():
        copy = dict(manifest)
        if name.endswith("-counters"):
            cd.validate_overlay(manifest, revisions[name])
            if check_exports:
                cd.verify_export(
                    Path(manifest["continuation_diagnostics"]["source_export"]),
                    revisions[name],
                )
            copy.pop("continuation_diagnostics")
        plain_views[name] = copy
    common.validate_manifests(plain_views, harness, environment, revisions=revisions)


def run_blocks(paths: dict, manifests: dict, out: Path, report: dict) -> int:
    out.mkdir(parents=True, exist_ok=False)
    report.update(
        schema_version=1,
        complete=False,
        interrupted=False,
        plan=plan(),
        stage=plain.STAGE,
        blocks=[],
        analysis=None,
        long_transfer_qualification_performed=False,
    )
    watched = [
        getattr(signal, n)
        for n in ("SIGINT", "SIGTERM", "SIGHUP")
        if hasattr(signal, n)
    ]
    saved = {sig: signal.getsignal(sig) for sig in watched}
    active = None

    def interrupted(signum, _frame):
        for sig in watched:
            signal.signal(sig, signal.SIG_IGN)
        report["interruption_signal"] = signum
        raise KeyboardInterrupt

    for sig in watched:
        signal.signal(sig, interrupted)
    try:
        for index, spec in enumerate(plan()):
            variant = spec["variant"]
            build_name = build_key(spec)
            name = f"{index:02}-{build_name}"
            argv = [
                "--build-manifest",
                str(paths[build_name]),
                "--output-directory",
                str(out / name),
            ]
            for key, value in arguments(spec).items():
                argv += ["--" + key.replace("_", "-"), str(value)]
            for profile in PROFILES:
                argv += ["--profile", profile]
            active = {"name": name, **spec, "exit_code": None, "driver_arguments": argv}
            report["blocks"].append(active)
            sc.write_report(out / REPORT, report)
            with (out / f"{name}.log").open("x") as log:
                try:
                    with contextlib.redirect_stdout(log), contextlib.redirect_stderr(
                        log
                    ):
                        active["exit_code"] = diag.main(argv)
                except SystemExit as error:
                    active["exit_code"] = error.code if type(error.code) is int else 1
                except Exception as error:
                    active["exit_code"], active["error"] = 1, repr(error)
                    if report.get("interruption_signal") is not None:
                        report["interruption_cleanup_error"] = repr(error)
                        raise KeyboardInterrupt from error
            # Keep an observed signal even if nested cleanup replaces or
            # suppresses KeyboardInterrupt. Never enter the next block.
            if report.get("interruption_signal") is not None:
                raise KeyboardInterrupt
            try:
                raw = json.loads((out / name / "report.json").read_text())
                active["analysis"] = analyze_block(
                    raw, manifests[build_name], active["exit_code"], spec, out / name
                )
            except (
                OSError,
                ValueError,
                TypeError,
                KeyError,
                OverflowError,
                AttributeError,
            ) as error:
                active["analysis"] = {
                    "measurement_contract_pass": False,
                    "cases": [],
                    "errors": [f"unusable report: {error}"],
                }
            sc.write_report(out / REPORT, report)
            print(
                json.dumps(
                    {
                        "block": name,
                        "exit_code": active["exit_code"],
                        "measurement_contract_pass": active["analysis"][
                            "measurement_contract_pass"
                        ],
                    }
                ),
                flush=True,
            )
        report["complete"] = True
        report["analysis"] = analyze_experiment(report)
        if not report["analysis"]["measurement_contract_pass"]:
            report["runner_exit_code"] = 1
        else:
            report["runner_exit_code"] = (
                0 if report["analysis"]["diagnosis_valid"] else 2
            )
        return report["runner_exit_code"]
    except KeyboardInterrupt:
        report["interrupted"] = True
        code = 128 + report.get("interruption_signal", signal.SIGINT)
        report["interruption_exit_code"] = report["runner_exit_code"] = code
        if active is not None and "analysis" not in active:
            if active["exit_code"] is not None:
                active["driver_exit_code_before_interruption"] = active["exit_code"]
            active["exit_code"] = code
        report["analysis"] = analyze_experiment(report)
        return code
    finally:
        try:
            sc.write_report(out / REPORT, report)
        finally:
            for sig, handler in saved.items():
                signal.signal(sig, handler)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    for variant in REVISIONS:
        for mode in ["plain", "counters"]:
            p.add_argument("--" + variant + "-" + mode, type=Path, required=True)
    p.add_argument("--output-directory", type=Path, required=True)
    args = p.parse_args(argv)
    if (
        platform.system() != "Linux"
        or platform.machine() not in ["arm64", "aarch64"]
        or socket.gethostname() != "lima-srt-network-lab-runtime"
    ):
        p.error("use the original dedicated Linux ARM64 lab VM")
    environment = diag.host_metadata()
    if environment["cpu_count"] != 4:
        p.error("retain four vCPU")
    paths = {
        v + "-" + m: getattr(args, v + "_" + m).resolve()
        for v in REVISIONS
        for m in ["plain", "counters"]
    }
    manifests = {k: json.loads(path.read_text()) for k, path in paths.items()}
    harness = build.source_identity(Path(__file__).resolve().parents[1])
    validate_manifests(manifests, harness, environment)
    signatures = common.validate_build_files(paths, manifests)
    if any(
        int(environment["sysctls"].get(f"net/core/{key}_max") or 0) < 8 * 1024**2
        for key in ["rmem", "wmem"]
    ):
        p.error("outer operator must configure and restore UDP maxima")
    report = {
        "harness_checkout": harness,
        "environment": environment,
        "manifests": manifests,
        "toolchain_signatures": signatures,
        "runner": sc.program_identity(Path(__file__)),
        "harness_files": [
            sc.program_identity(Path(m.__file__))
            for m in [plain, plain.budget, common, build, diag, sc, cd]
        ],
        "candidate_nomination_performed": False,
    }
    return run_blocks(paths, manifests, args.output_directory.resolve(), report)


if __name__ == "__main__":
    raise SystemExit(main())
