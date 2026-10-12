#!/usr/bin/env python3
"""Publish runner setup failures must preserve bounded, private evidence."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BENCH = Path(__file__).resolve().parents[1] / "bench"


class PublishSetup(unittest.TestCase):
    def test_missing_fixture_and_invalid_affinity_write_failed_result(self):
        for cpus in (str(min(os.sched_getaffinity(0))), "invalid"):
            with self.subTest(cpus=cpus), tempfile.TemporaryDirectory() as folder:
                output = Path(folder) / "run"
                finished = subprocess.run([
                    sys.executable, str(BENCH / "perf_publish.py"), "--output", str(output),
                    "--signaling-url", "http://127.0.0.1:1", "--client-cpus", cpus,
                    "--fixture", str(Path(folder) / "missing.flv")],
                    stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=5)
                self.assertEqual(finished.returncode, 1, finished.stdout + finished.stderr)
                result = json.loads((output / "result.json").read_text())
                self.assertEqual(result["status"], "FAIL")
                self.assertTrue(result["errors"])
                self.assertEqual(result["generations"], [])
                self.assertTrue(result["cleanup"]["all_owned_publishers_reaped"])
                self.assertGreaterEqual(result["ended_monotonic_ns"], result["started_monotonic_ns"])
                self.assertEqual(output.stat().st_mode & 0o777, 0o700)


if __name__ == "__main__":
    unittest.main()
