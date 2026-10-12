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
    def test_auth_rejection_requires_real_error_evidence(self):
        from perf_validate import verdict
        base = {"status": "FAIL", "capacity_boundary": "UNCONFIRMED",
                "cleanup": {"all_owned_processes_reaped": True, "remaining_owned_receivers": []},
                "groups": [{"failure_category": "AUTHORIZATION", "first_errors": []}]}
        self.assertEqual(verdict("invalid_token", base, False)[0], "INCONCLUSIVE")
        base["groups"][0]["first_errors"] = ["HTTP status=403"]
        self.assertEqual(verdict("invalid_token", base, False)[0], "PASS")

    def test_media_stop_must_be_observable(self):
        from perf_validate import verdict
        base = {"status": "FAIL", "capacity_boundary": "UNCONFIRMED",
                "cleanup": {"all_owned_processes_reaped": True, "remaining_owned_receivers": []},
                "groups": [{"failure_category": "SOURCE_ENDED", "measurement": {"failed": 1}}]}
        self.assertEqual(verdict("source_stop", base, False)[0], "PASS")
        base["groups"][0]["measurement"]["failed"] = 0
        self.assertEqual(verdict("source_stop", base, False)[0], "INCONCLUSIVE")

    def test_cancellation_must_reap_every_child(self):
        from perf_validate import verdict
        base = {"status": "FAIL", "capacity_boundary": "UNCONFIRMED",
                "cleanup": {"all_owned_processes_reaped": True},
                "errors": [{"type": "KeyboardInterrupt"}]}
        self.assertEqual(verdict("cancel", base, True)[0], "PASS")
        base["cleanup"]["all_owned_processes_reaped"] = False
        self.assertEqual(verdict("cancel", base, True)[0], "FAIL")


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
            jeprof.write_text("#!/bin/sh\necho 'sample allocation: 123 bytes'\n")
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
