from __future__ import annotations
import copy, json, os, shutil, subprocess, sys, tempfile, unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "benchmarks"))
import continuation_diagnostics as cd
import throughput_diagnostics as diag


class CounterTests(unittest.TestCase):
    def record(self, d, pid=10, shard=0, **values):
        c = dict.fromkeys(cd.NAMES, 0)
        c.update(worker_starts=1, **values)
        head = {"schema": 2, "pid": pid, "tid": pid + 100 + shard, "serial": shard}
        row = {
            **head,
            "complete": True,
            "overflow": False,
            "worker_shard": shard,
            "counters": c,
        }
        path = d / f"{pid}-{shard}.jsonl"
        path.write_text(json.dumps(head) + "\n" + json.dumps(row) + "\n")
        return path, row

    def fixture(self, d, variant="baseline"):
        d.mkdir(exist_ok=True)
        counter = d / "continuation-counters"
        counter.mkdir()
        for side in ["caller", "listener"]:
            (d / f"many-socket-{side}.stderr").write_text("")
        for pid in [10, 20]:
            for shard in [0, 1]:
                self.record(counter, pid, shard)
        path, row = self.record(
            counter, tx_originals=4, poll_packets=4, connection_polls=1, batch_2_4=1
        )
        self.record(counter, pid=20, rx_attempts=4, rx_datagrams=4, rx_data=4)
        return (
            {
                "peer_process_resources": {
                    "sender": {"pid": 10},
                    "receiver": {"pid": 20},
                },
                "wire_statistics": {
                    "sender_packets_unique": 4,
                    "receiver_packets_unique": 4,
                    "sender_packets_total": 4,
                    "retransmitted_packets": 0,
                },
            },
            path,
            row,
        )

    def test_exact_pinned_product_and_candidate_helper_anchors(self):
        import hashlib

        # A separate harness may sit on either frozen product tree. The lab
        # exports and verifies both complete trees before any measurement.
        runtime_hash = hashlib.sha256((cd.ROOT / cd.RUNTIME).read_bytes()).hexdigest()
        revision = next(
            pin
            for pin in cd.REVISIONS.values()
            if cd.PIN_HASHES[pin][cd.RUNTIME] == runtime_hash
        )
        for path, hooks in cd.hooks(revision).items():
            raw = (cd.ROOT / path).read_bytes()
            self.assertEqual(
                hashlib.sha256(raw).hexdigest(),
                cd.PIN_HASHES[revision][path],
            )
            self.assertIn(
                "diagnostic_continuation_counters", cd.instrument(raw.decode(), hooks)
            )
        text = (
            Path(__file__).parent / "fixtures/continuation/original-helper.hpp"
        ).read_text()
        self.assertEqual(
            hashlib.sha256(text.encode()).hexdigest(),
            cd.PIN_HASHES[cd.REVISIONS["candidate"]][cd.HELPER],
        )
        for hooks in [
            cd.HELPER_HOOKS,
            cd.COMMON + cd.BASELINE,
            cd.COMMON + cd.CANDIDATE,
        ]:
            before = "\n".join(a for a, b in hooks)
            patched = cd.instrument(before, hooks)
            with self.assertRaises(ValueError):
                cd.instrument(patched, hooks)
        with self.assertRaises(ValueError):
            cd.hooks("unknown")
        with self.assertRaises(ValueError):
            cd.instrument("same same", [("same", "new")])

    def test_endpoints_require_both_registered_shards(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            result, path, row = self.fixture(root)
            for variant in cd.REVISIONS:
                self.assertTrue(
                    cd.analyze_case(
                        root, result, "robotweax-self", cd.REVISIONS[variant]
                    )["valid"]
                )
            self.assertEqual(
                cd.analyze_case(
                    root, result, "robotweax-self", cd.REVISIONS["baseline"]
                )["endpoints"]["sender"]["per_original_packet"]["poll_packets"],
                1,
            )
            (root / "continuation-counters/20-1.jsonl").unlink()
            self.assertFalse(
                cd.analyze_case(
                    root, result, "robotweax-self", cd.REVISIONS["baseline"]
                )["valid"]
            )

    def test_partial_unknown_overflow_and_unreconciled_data_rejected(self):
        mutations = [
            lambda r: r.update(complete=False),
            lambda r: r.update(overflow=True),
            lambda r: r.update(pid=True),
            lambda r: r["counters"].pop("rx_eagain"),
            lambda r: r["counters"].update(rx_errors=-1),
            lambda r: r["counters"].update(rx_errors=True),
            lambda r: r["counters"].update(rx_errors=2**64),
            lambda r: r["counters"].update(rx_attempts=1),
            lambda r: r["counters"].update(channel_calls=1),
            lambda r: r["counters"].update(channel_full_polls=1, channel_calls=1),
            lambda r: r["counters"].update(connection_polls=1),
            lambda r: r["counters"].update(arm_calls=1),
            lambda r: r["counters"].update(take_calls=1),
            lambda r: r["counters"].update(age_ge_5us=1),
        ]
        for mutate in mutations:
            with self.subTest(mutate=mutate), tempfile.TemporaryDirectory() as d:
                path, row = self.record(Path(d))
                header = path.read_text().splitlines()[0]
                mutate(row)
                path.write_text(header + "\n" + json.dumps(row) + "\n")
                with self.assertRaises(ValueError):
                    cd.read_counters(Path(d))
        with tempfile.TemporaryDirectory() as d:
            path, row = self.record(Path(d))
            full = path.read_text()
            path.write_text(full.splitlines()[0] + "\n")
            with self.assertRaises(ValueError):
                cd.read_counters(Path(d))
            path.write_text(full)
            (Path(d) / "duplicate.jsonl").write_text(full)
            with self.assertRaises(ValueError):
                cd.read_counters(Path(d))

    def test_candidate_lifecycle_and_baseline_separation(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            result, path, row = self.fixture(root)
            row["counters"].update(
                channel_calls=3,
                channel_full_polls=2,
                channel_skips=1,
                channel_no_hint=1,
                channel_after_skip=1,
                channel_immediate_pacer=1,
                channel_delayed=1,
                route_isolated=2,
                arm_calls=1,
                arm_success=1,
                take_calls=1,
                take_success=1,
                age_lt_1us=1,
            )
            header = path.read_text().splitlines()[0]
            path.write_text(header + "\n" + json.dumps(row) + "\n")
            self.assertTrue(
                cd.analyze_case(
                    root, result, "robotweax-self", cd.REVISIONS["candidate"]
                )["valid"]
            )
            self.assertFalse(
                cd.analyze_case(
                    root, result, "robotweax-self", cd.REVISIONS["baseline"]
                )["valid"]
            )
            row["counters"]["route_isolated"] = 1
            path.write_text(header + "\n" + json.dumps(row) + "\n")
            self.assertFalse(
                cd.analyze_case(
                    root, result, "robotweax-self", cd.REVISIONS["candidate"]
                )["valid"]
            )

    def test_complete_export_guard_detects_unpatched_edits_and_extra_files(self):
        import hashlib

        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "src").mkdir()
            p = root / "src/other.cpp"
            p.write_bytes(b"original")
            (root / "src/diagnostic_continuation_counters.hpp").write_bytes(
                cd.HEADER.read_bytes()
            )
            oid = hashlib.sha1(b"blob 8\0original").hexdigest()
            tree = ("100644 blob " + oid + "\tsrc/other.cpp\0").encode()
            with patch.object(cd.subprocess, "check_output", return_value=tree):
                self.assertEqual(cd.verify_export(root, cd.REVISIONS["baseline"]), 2)
                p.write_bytes(b"changed")
                with self.assertRaises(ValueError):
                    cd.verify_export(root, cd.REVISIONS["baseline"])
                p.write_bytes(b"original")
                (root / "extra").write_text("extra")
                with self.assertRaises(ValueError):
                    cd.verify_export(root, cd.REVISIONS["baseline"])

    def test_capture_is_separate_and_clears_inherited_environment(self):
        self.assertEqual(
            diag.perf_command("continuation", Path("unused"), ["driver"], "unused"),
            ["driver"],
        )
        self.assertEqual(
            len(
                diag.case_plan(
                    ["robotweax-self", "robotweax-to-haivision"],
                    1,
                    2,
                    True,
                    "continuation",
                )
            ),
            2,
        )
        with tempfile.TemporaryDirectory() as d, patch.dict(
            os.environ,
            {
                "ROBOTWEAX_CONTINUATION_COUNTER_DIR": "wrong",
                "ROBOTWEAX_POLL_COUNTER_DIR": "wrong",
            },
        ):
            path = Path(d) / "request.json"
            path.write_text(
                json.dumps(
                    {
                        "capture": "none",
                        "profile": "robotweax-self",
                        "programs": {},
                        "options": {},
                        "index": 0,
                        "kind": "measurement",
                        "peer_arguments": [],
                    }
                )
            )
            with patch.object(diag, "udp_snapshot", return_value={}):
                diag.run_case(path)
            self.assertNotIn("ROBOTWEAX_CONTINUATION_COUNTER_DIR", os.environ)
            self.assertNotIn("ROBOTWEAX_POLL_COUNTER_DIR", os.environ)
        for enabled, capture in [
            (True, "none"),
            (True, "transport"),
            (False, "continuation"),
        ]:
            with tempfile.TemporaryDirectory() as d:
                p = Path(d) / "manifest.json"
                p.write_text(
                    json.dumps(
                        {
                            "complete": True,
                            "continuation_diagnostics": {"enabled": enabled},
                        }
                    )
                )
                with self.assertRaises(SystemExit):
                    diag.main(
                        [
                            "--build-manifest",
                            str(p),
                            "--output-directory",
                            str(Path(d) / "out"),
                            "--capture",
                            capture,
                        ]
                    )


@unittest.skipUnless(
    sys.platform in ["linux", "darwin"] and shutil.which("c++"),
    "POSIX native collector",
)
class NativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        original = (
            Path(__file__).parent / "fixtures/continuation/original-helper.hpp"
        ).read_text()
        (cls.root / "original.hpp").write_text(original)
        patched = cd.instrument(original, cd.HELPER_HOOKS).replace(
            "../diagnostic_continuation_counters.hpp", "continuation_counters.hpp"
        )
        (cls.root / "patched.hpp").write_text(patched)
        body = """
#include HEADER
#include <iostream>
#include <cstring>
using P = robotweax::srt::compat::PacedPollContinuation;
int main(int argc,char**argv) {
#ifdef DIAGNOSTIC
 robotweax::srt::diagnostics::continuation_counters().worker(0);
#endif
 P p; const P::Time t {std::chrono::seconds {1}};
 std::cout<<sizeof(P)<<":";
 p.arm(t+std::chrono::microseconds{10},t,1); std::cout<<p.take(t,1)<<p.take(t,1);
 p.arm(t+std::chrono::microseconds{10},t,1); std::cout<<p.take(t+std::chrono::microseconds{5},1);
 p.arm(t+std::chrono::microseconds{2},t,1); std::cout<<p.take(t+std::chrono::microseconds{2},1);
 p.arm(t+std::chrono::microseconds{10},t,1); std::cout<<p.take(t,2);
 p.arm(t+std::chrono::microseconds{10},t,1); std::cout<<p.take(t-std::chrono::microseconds{1},1);
 p.arm(t,t,1); std::cout<<p.pending();
 p.arm(P::Time::max(),P::Time::max()-std::chrono::microseconds{5},1);std::cout<<p.pending();
 if(argc>1 && std::strcmp(argv[1],"abrupt")==0) {_exit(0);}
#ifdef DIAGNOSTIC
 if(argc>1 && std::strcmp(argv[1],"overflow")==0) {robotweax::srt::diagnostics::continuation_counters().add(robotweax::srt::diagnostics::Counter::arm_calls,UINT64_MAX);}
#endif
}
"""
        for kind in ["original", "patched"]:
            source = cls.root / (kind + ".cpp")
            source.write_text(
                '#include <unistd.h>\n#define HEADER "'
                + kind
                + '.hpp"\n'
                + ("#define DIAGNOSTIC\n" if kind == "patched" else "")
                + body
            )
            subprocess.run(
                [
                    "c++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I" + str(cd.HEADER.parent),
                    str(source),
                    "-o",
                    str(cls.root / kind),
                ],
                check=True,
                capture_output=True,
            )

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_behavior_size_counter_reasons_and_partial_output(self):
        expected = subprocess.check_output([str(self.root / "original")], text=True)
        for mode in ["normal", "abrupt", "overflow"]:
            with tempfile.TemporaryDirectory() as d:
                result = subprocess.run(
                    [str(self.root / "patched"), mode],
                    env=os.environ | {"ROBOTWEAX_CONTINUATION_COUNTER_DIR": d},
                    text=True,
                    capture_output=True,
                    check=True,
                )
                if mode != "normal":
                    with self.assertRaises(ValueError):
                        cd.read_counters(Path(d))
                else:
                    self.assertEqual(result.stdout, expected)
                    rows = cd.read_counters(Path(d))
                    self.assertEqual(len(rows), 1)
                    c = rows[0]["counters"]
                    for key in [
                        "take_success",
                        "take_no_pending",
                        "take_expired_cap",
                        "take_expired_pacer",
                        "take_epoch_changed",
                        "take_rollback",
                        "arm_due",
                        "arm_overflow",
                    ]:
                        self.assertEqual(c[key], 1, key)
                    self.assertEqual(c["age_ge_5us"], 1)
                    self.assertEqual(c["age_rollback"], 1)
