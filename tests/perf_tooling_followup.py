#!/usr/bin/env python3
"""Offline regression tests for capacity failure evidence and profiler collection."""

import importlib.util
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest

BENCH = pathlib.Path(__file__).resolve().parents[1] / "bench"
# perf_capacity needs only OUTPUTS at import time; actual perf_run remains unmodified.
if "perf_run" not in sys.modules:
    import types
    sys.modules["perf_run"] = types.SimpleNamespace(OUTPUTS=("rtmp", "rtsp", "flv", "hls", "whep", "gb_udp", "gb_tcp_active", "gb_tcp_passive"))
sys.path.insert(0, str(BENCH))
from perf_capacity import run_round


class EvidencePrivacy(unittest.TestCase):
    def test_command_redaction(self):
        # Extract just the helper so the offline test does not require C++ binaries.
        source = (BENCH / "perf_run.py").read_text()
        function = "def redacted_command" + source.split("def redacted_command", 1)[1].split("def workload_counts", 1)[0]
        namespace = {"re": re}
        exec(function, namespace)
        token = "a" * 64
        command = ["media_server", "--control-token", "private", "--option=123",
                   "rtmp://198.19.0.10/live/" + token, "--control-token=other-secret"]
        result = namespace["redacted_command"](command)
        self.assertNotIn("private", " ".join(result))
        self.assertNotIn("other-secret", " ".join(result))
        self.assertNotIn(token, " ".join(result))
        self.assertIn("<token>", " ".join(result))


class CapacityRunner(unittest.TestCase):
    def test_contradictory_boundary_is_not_capacity_evidence(self):
        for state, boundary, code in (("FAIL", "HEALTHY", 1), ("PASS", "SERVER_UNSTABLE", 0),
                                      ("BOGUS", "HEALTHY", 0), ("FAIL", "SERVER_UNSTABLE", 0)):
            with self.subTest(state=state, boundary=boundary, code=code), tempfile.TemporaryDirectory() as folder:
                directory = pathlib.Path(folder) / "round"
                body = json.dumps({"status": state, "capacity_boundary": boundary})
                snippet = (f"from pathlib import Path; p=Path({str(directory)!r}); p.mkdir(); "
                           f"(p/'result.json').write_text({body!r}); raise SystemExit({code})")
                result = run_round([sys.executable, "-c", snippet], directory, 2)
                self.assertEqual((result["status"], result["boundary"]), ("INCONCLUSIVE", "RUNNER_FAILED"))

    def test_cancel_records_inconclusive_and_stops_owned_group(self):
        from unittest.mock import Mock, patch
        import perf_capacity
        with tempfile.TemporaryDirectory() as folder:
            process = Mock(pid=123456)
            process.wait.side_effect = [KeyboardInterrupt, -15]
            with patch.object(perf_capacity.subprocess, "Popen", return_value=process), patch.object(perf_capacity.os, "killpg") as kill:
                result = run_round(["mock-runner"], pathlib.Path(folder) / "round", 1)
            self.assertEqual((result["status"], result["boundary"]), ("INCONCLUSIVE", "CANCELLED"))
            kill.assert_called_once_with(process.pid, perf_capacity.signal.SIGTERM)

    def test_cancel_retains_completed_round_checkpoint(self):
        from unittest.mock import patch
        import perf_capacity
        with tempfile.TemporaryDirectory() as folder:
            output = pathlib.Path(folder) / "capacity"
            calls = []
            def execute(command, directory, timeout):
                calls.append(directory)
                if len(calls) == 1:
                    return {"status": "PASS", "boundary": "HEALTHY", "exit_code": 0}
                checkpoint = json.loads((output / "result.json").read_text())
                self.assertEqual(checkpoint["rounds"][0]["viewers"], 1)
                raise KeyboardInterrupt
            argv = ["perf_capacity", "--output", str(output), "--counts", "1,2"]
            with patch.object(sys, "argv", argv), patch.object(perf_capacity, "run_round", side_effect=execute), patch.object(perf_capacity.signal, "signal"):
                self.assertEqual(perf_capacity.main(), 1)
            summary = json.loads((output / "result.json").read_text())
            self.assertEqual((summary["status"], summary["reason"]), ("INCONCLUSIVE", "CANCELLED"))
            self.assertEqual(len(summary["rounds"]), 1)

    def test_invalid_counts_do_not_create_output(self):
        with tempfile.TemporaryDirectory() as folder:
            output = pathlib.Path(folder) / "capacity"
            result = subprocess.run([sys.executable, str(BENCH / "perf_capacity.py"), "--output", str(output), "--counts", "1,bad"],
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 2)
            self.assertIn("counts must be", result.stderr)
            self.assertNotIn("Traceback", result.stderr)
            self.assertFalse(output.exists())

    def test_missing_result_is_inconclusive(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = pathlib.Path(folder) / "round"
            result = run_round([sys.executable, "-c", "pass"], directory, 2)
            self.assertEqual((result["status"], result["boundary"]), ("INCONCLUSIVE", "RUNNER_FAILED"))
            self.assertIn("did not write result", result["error"])
            self.assertTrue(pathlib.Path(result["log"]).exists())

    def test_malformed_result_is_inconclusive(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = pathlib.Path(folder) / "round"
            snippet = f"from pathlib import Path; p=Path({str(directory)!r}); p.mkdir(); (p/'result.json').write_text('not json')"
            result = run_round([sys.executable, "-c", snippet], directory, 2)
            self.assertEqual(result["boundary"], "RUNNER_FAILED")
            self.assertIn("invalid runner result", result["error"])

    def test_success_must_match_exit_code(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = pathlib.Path(folder) / "round"
            snippet = (f"from pathlib import Path; p=Path({str(directory)!r}); p.mkdir(); "
                       "(p/'result.json').write_text('{\"status\":\"PASS\",\"capacity_boundary\":\"HEALTHY\"}'); raise SystemExit(1)")
            result = run_round([sys.executable, "-c", snippet], directory, 2)
            self.assertEqual(result["boundary"], "RUNNER_FAILED")

    def test_server_boundary_is_preserved(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = pathlib.Path(folder) / "round"
            snippet = (f"from pathlib import Path; p=Path({str(directory)!r}); p.mkdir(); "
                       "(p/'result.json').write_text('{\"status\":\"FAIL\",\"capacity_boundary\":\"SERVER_UNSTABLE\"}'); raise SystemExit(1)")
            result = run_round([sys.executable, "-c", snippet], directory, 2)
            self.assertEqual(result["boundary"], "SERVER_UNSTABLE")

    def test_unresponsive_child_is_bounded(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = pathlib.Path(folder) / "round"
            started = time.monotonic()
            result = run_round([sys.executable, "-c", "import time; time.sleep(20)"], directory, 1)
            self.assertLess(time.monotonic() - started, 6)
            self.assertEqual(result["boundary"], "RUNNER_FAILED")
            self.assertIn("exceeded", result["error"])


class NegativeScenarioValidation(unittest.TestCase):
    def test_source_stop_injection_is_abrupt_and_records_completion(self):
        from types import SimpleNamespace
        from unittest.mock import Mock
        source = (BENCH / "perf_run.py").read_text()
        block = '                if args.fault == "source_stop"' + source.split('                if args.fault == "source_stop"', 1)[1].split("                next_sample", 1)[0]
        events = []
        publisher = Mock(pid=123)
        publisher.poll.return_value = None
        publisher.kill.side_effect = lambda: events.append("kill")
        def wait(timeout):
            events.append(("wait", timeout))
            publisher.poll.return_value = -9
            return -9
        publisher.wait.side_effect = wait
        def no_graceful_stop(_process):
            self.fail("fault injection must not spend the measurement window in graceful cleanup")
        namespace = {"args": SimpleNamespace(fault="source_stop", duration=18), "result": {}, "publishers": [publisher],
                     "clients": [1, 2], "start_ns": 100_000_000_000, "stopped_source": False,
                     "time": SimpleNamespace(monotonic_ns=lambda: 109_000_000_000), "stop_process": no_graceful_stop}
        exec(textwrap.dedent(block), namespace)
        self.assertEqual(events, ["kill", ("wait", 5)])
        fault = namespace["result"]["fault_evidence"]
        self.assertLessEqual(fault["requested_monotonic_ns"], fault["completed_monotonic_ns"])
        self.assertEqual(fault["publishers"], [{"pid": publisher.pid, "exit_code": -9}])

    def test_malformed_or_unknown_result_cannot_pass(self):
        from perf_validate import verdict
        self.assertEqual(verdict("invalid_token", [], False)[0], "FAIL")
        base = {"status": "UNKNOWN", "cleanup": {"all_owned_processes_reaped": True},
                "groups": [{"failure_category": "AUTHORIZATION", "first_errors": ["HTTP 403"], "measurement": {"ready": 0}}]}
        self.assertEqual(verdict("invalid_token", base, False)[0], "INCONCLUSIVE")

    def test_auth_rejection_requires_real_error_evidence(self):
        from perf_validate import verdict
        base = {"status": "FAIL", "capacity_boundary": "UNCONFIRMED",
                "cleanup": {"all_owned_processes_reaped": True, "remaining_owned_receivers": []},
                "groups": [{"failure_category": "AUTHORIZATION", "first_errors": [], "measurement": {"ready": 0}}]}
        self.assertEqual(verdict("invalid_token", base, False)[0], "INCONCLUSIVE")
        base["groups"][0]["first_errors"] = ["HTTP status=403"]
        self.assertEqual(verdict("invalid_token", base, False)[0], "PASS")
        base["groups"].append({"measurement": {"ready": 1, "progressing": 1}})
        self.assertEqual(verdict("invalid_token", base, False)[0], "INCONCLUSIVE")

    def test_media_stop_must_be_observable(self):
        from perf_validate import verdict
        base = {"status": "FAIL", "capacity_boundary": "UNCONFIRMED",
                "cleanup": {"all_owned_processes_reaped": True, "remaining_owned_receivers": []},
                "requested_window": {"start_monotonic_ns": 10, "end_monotonic_ns": 30},
                "fault_evidence": {"kind": "source_stop", "requested_monotonic_ns": 20, "completed_monotonic_ns": 21, "media_ready_groups": 1,
                                   "publishers": [{"pid": 1, "exit_code": 0}]},
                "groups": [{"requested": 1, "failure_category": "SOURCE_ENDED", "measurement": {"failed": 1, "ready": 1, "measurement_start_monotonic_ns": 11}}]}
        self.assertEqual(verdict("source_stop", base, False)[0], "PASS")
        base["groups"][0]["measurement"]["failed"] = 0
        self.assertEqual(verdict("source_stop", base, False)[0], "INCONCLUSIVE")
        base["groups"][0]["measurement"]["failed"] = 1
        base["fault_evidence"]["completed_monotonic_ns"] = 31
        self.assertEqual(verdict("source_stop", base, False)[0], "INCONCLUSIVE")
        del base["fault_evidence"]
        self.assertEqual(verdict("source_stop", base, False)[0], "INCONCLUSIVE")

    def test_startup_error_cannot_prove_source_stop(self):
        from perf_validate import verdict
        base = {"status": "FAIL", "capacity_boundary": "UNCONFIRMED",
                "cleanup": {"all_owned_processes_reaped": True},
                "groups": [{"failure_category": "SOURCE_ENDED", "first_errors": ["error=connect failed"]}]}
        self.assertEqual(verdict("source_stop", base, False)[0], "INCONCLUSIVE")

    def test_cancellation_must_reap_every_child(self):
        from perf_validate import verdict
        base = {"status": "FAIL", "capacity_boundary": "UNCONFIRMED",
                "cleanup": {"all_owned_processes_reaped": True},
                "errors": [{"type": "KeyboardInterrupt"}]}
        self.assertEqual(verdict("cancel", base, True)[0], "PASS")
        base["cleanup"]["all_owned_processes_reaped"] = False
        self.assertEqual(verdict("cancel", base, True)[0], "FAIL")


class MeasurementEvidence(unittest.TestCase):
    def test_requested_window_cannot_replace_actual_window(self):
        source = (BENCH / "perf_run.py").read_text()
        block = "            for group in result[\"groups\"]:" + source.split("            for group in result[\"groups\"]:", 1)[1].split('            result["status"] = "PASS"', 1)[0]
        from types import SimpleNamespace
        for start, end, healthy in ((100_000_000_000, 110_000_000_000, True),
                                    (100_001_000_000, 110_000_000_000, True),
                                    (101_000_000_000, 110_000_000_000, False), (0, 0, False)):
            group = {"requested": 1, "exit_code": 0, "measurement": {"ready": 1, "progressing": 1,
                     "measurement_start_monotonic_ns": start, "measurement_end_monotonic_ns": end}}
            namespace = {"args": SimpleNamespace(duration=10), "result": {"groups": [group]},
                         "start_ns": 100_000_000_000, "deadline_ns": 110_000_000_000}
            exec(textwrap.dedent(block), namespace)
            self.assertEqual(namespace["success"], healthy)
            self.assertGreaterEqual(group["window"]["coverage"], 0)


class ProfileDriver(unittest.TestCase):
    def invoke(self, *args):
        return subprocess.run([sys.executable, str(BENCH / "perf_profile.py"), *map(str, args)],
                              capture_output=True, text=True, timeout=15)

    def test_missing_profiler_is_blocked(self):
        with tempfile.TemporaryDirectory() as folder:
            output = pathlib.Path(folder) / "run"
            finished = self.invoke("--mode", "offcpu", "--pid", os.getpid(), "--duration", 1,
                                   "--offcpu-bin", "/does-not-exist", "--output", output)
            self.assertEqual(finished.returncode, 1)
            result = json.loads((output / "result.json").read_text())
            self.assertEqual(result["status"], "BLOCKED")

    def test_reused_pid_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            output = pathlib.Path(folder) / "run"
            finished = self.invoke("--mode", "cpu-stat", "--pid", os.getpid(), "--duration", 1,
                                   "--expected-starttick", -1, "--output", output)
            self.assertEqual(finished.returncode, 1)
            result = json.loads((output / "result.json").read_text())
            self.assertIn("starttick differs", result["reason"])

    def test_heap_report_of_known_file(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            heap = root / "dummy.heap"
            heap.write_text("diagnostic fixture")
            jeprof = root / "jeprof"
            jeprof.write_text("#!/bin/sh\necho 'Total: 123 B'\necho 'sample allocation: 123 bytes'\n")
            jeprof.chmod(0o755)
            finished = self.invoke("--mode", "heap-report", "--output", root / "result", "--binary", sys.executable,
                                   "--heap-file", heap, "--jeprof-bin", jeprof)
            self.assertEqual(finished.returncode, 0, finished.stderr)
            result = json.loads((root / "result/result.json").read_text())
            self.assertEqual(result["status"], "PASS")
            self.assertTrue(result["heap_sha256"])

    def test_fake_cpu_record_retains_raw_and_report(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            fake = root / "perf"
            fake.write_text(textwrap.dedent("""\
                #!/usr/bin/env python3
                import pathlib, sys
                if sys.argv[1] == 'record':
                    pathlib.Path(sys.argv[sys.argv.index('-o') + 1]).write_bytes(b'profiler-raw-data')
                if sys.argv[1] == 'report':
                    print("# Samples: 1 of event 'cycles:P'")
                    print('stack evidence')
                """))
            fake.chmod(0o755)
            finished = self.invoke("--mode", "cpu-record", "--output", root / "result", "--pid", os.getpid(),
                                   "--duration", 1, "--perf-bin", fake)
            self.assertEqual(finished.returncode, 0, finished.stderr)
            result = json.loads((root / "result/result.json").read_text())
            self.assertEqual(result["status"], "PASS")
            self.assertEqual(len(result["steps"]), 2)


if __name__ == "__main__":
    unittest.main()
