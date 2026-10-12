#!/usr/bin/env python3
"""Offline profiler and runtime-manifest acceptance; no media or performance load."""

import contextlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

BENCH = Path(__file__).resolve().parents[1] / "bench"


def load(name):
    spec = importlib.util.spec_from_file_location(name, BENCH / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ProfileAcceptance(unittest.TestCase):
    def invoke(self, root, mode, body):
        fake = root / "diagnostic"
        fake.write_text("#!/usr/bin/env python3\n" + body)
        fake.chmod(0o755)
        option = "--offcpu-bin" if mode == "offcpu" else "--perf-bin"
        finished = subprocess.run([sys.executable, str(BENCH / "perf_profile.py"), "--mode", mode,
            "--pid", str(os.getpid()), "--duration", "1", option, str(fake), "--output", str(root / "result")],
            capture_output=True, text=True, timeout=10)
        return finished, json.loads((root / "result/result.json").read_text())

    def test_empty_stat_is_not_pass(self):
        with tempfile.TemporaryDirectory() as folder:
            finished, result = self.invoke(Path(folder), "cpu-stat", "pass\n")
            self.assertEqual(finished.returncode, 1)
            self.assertEqual(result["status"], "BLOCKED")
            self.assertIn("requested counters", result["reason"])

    def test_real_format_stat_is_attributable_and_noninteractive(self):
        with tempfile.TemporaryDirectory() as folder:
            body = ("import sys\nassert sys.stdin.read() == ''\n"
                    "for event in ('task-clock','context-switches','cpu-migrations','page-faults','cycles','instructions'):\n"
                    " print('1\\t\\t'+event+'\\t1\\t100.0')\n")
            finished, result = self.invoke(Path(folder), "cpu-stat", body)
            self.assertEqual(finished.returncode, 0, result)
            self.assertEqual(result["counters"]["task-clock"], 1)
            step = result["steps"][0]
            self.assertGreater(step["ended_monotonic_ns"], step["started_monotonic_ns"])
            self.assertTrue(step["tool_sha256"])
            self.assertTrue(result["binary_sha256"])

    def test_unavailable_stat_is_not_zero(self):
        with tempfile.TemporaryDirectory() as folder:
            body = "print('<not supported>\\t\\tcycles\\t0\\t0')\n"
            finished, result = self.invoke(Path(folder), "cpu-stat", body)
            self.assertEqual(finished.returncode, 1)
            self.assertIsNone(result["counters"]["cycles"])

    def test_warning_is_not_folded_evidence(self):
        with tempfile.TemporaryDirectory() as folder:
            finished, result = self.invoke(Path(folder), "offcpu", "print('WARNING: 12 stack traces lost')\n")
            self.assertEqual(finished.returncode, 1)
            self.assertEqual(result["folded_stacks"], 0)

    def test_folded_wait_has_units(self):
        with tempfile.TemporaryDirectory() as folder:
            finished, result = self.invoke(Path(folder), "offcpu", "print('dut;epoll_wait 123')\n")
            self.assertEqual(finished.returncode, 0, result)
            self.assertEqual(result["offcpu_microseconds"], 123)

    def test_record_header_without_samples_is_blocked(self):
        with tempfile.TemporaryDirectory() as folder:
            body = ("import pathlib,sys\n"
                    "if sys.argv[1]=='record': pathlib.Path(sys.argv[sys.argv.index('-o')+1]).write_bytes(b'header')\n"
                    "else: print('# Samples: 0 of event cycles')\n")
            finished, result = self.invoke(Path(folder), "cpu-record", body)
            self.assertEqual(finished.returncode, 1)
            self.assertEqual(result["samples"], 0)

    def test_timeout_reaps_own_group(self):
        profile = load("perf_profile")
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            child_pid = root / "child.pid"
            command = [sys.executable, "-c", "import subprocess,time,pathlib; "
                       "p=subprocess.Popen(['sleep','20']); "
                       f"pathlib.Path({str(child_pid)!r}).write_text(str(p.pid)); time.sleep(20)"]
            step = profile.execute(command, root / "tool.log", 0.2)
            self.assertIn("TimeoutExpired", step["error"])
            pid = int(child_pid.read_text())
            status = Path(f"/proc/{pid}/stat")
            self.assertTrue(not status.exists() or status.read_text().rsplit(") ", 1)[1].split()[0] == "Z")


class RuntimeAcceptance(unittest.TestCase):
    def test_static_elf_has_no_dynamic_closure(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("no C compiler for static ELF fixture")
        package = load("package_runtime")
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / "static"
            built = subprocess.run([compiler, "-static", "-x", "c", "-o", str(executable), "-"],
                input="int main(void) { return 0; }\n", capture_output=True, text=True, timeout=30)
            if built.returncode:
                self.skipTest("static libc unavailable: " + built.stderr)
            self.assertEqual(list(package.runtime_libraries(executable)), [])

    def test_manifest_includes_launcher_and_build_provenance(self):
        package = load("package_runtime")
        previous = Path.cwd()
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            build = root / "build"
            build.mkdir()
            names = ("media_server", "whep_fanout", "whip_publisher", "rtmp_fanout", "rtsp_fanout", "http_flv_fanout", "rtsp_publisher", "gb_ps_fanout")
            for name in names:
                (build / name).write_bytes(b"test ELF placeholder")
            (build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\nCMAKE_CXX_FLAGS:STRING=-Werror\n")
            fixture = root / "fixture.flv"
            fixture.write_bytes(b"fixture")
            modules = root / "nss"
            modules.mkdir()
            for name in package.NSS_MODULES:
                (modules / name).write_bytes(b"test module placeholder")
            argv = ["package_runtime.py", "--build-dir", str(build), "--fixture", str(fixture),
                    "--ffmpeg-bin", sys.executable, "--ffprobe-bin", sys.executable, "--output", str(root / "bundle")]
            try:
                os.chdir(BENCH.parent)
                with patch.object(sys, "argv", argv), patch.object(package, "NSS_MODULE_DIRECTORY", modules), \
                     patch.object(package, "runtime_libraries", return_value=[]), contextlib.redirect_stdout(None):
                    package.main()
            finally:
                os.chdir(previous)
            manifest = json.loads((root / "bundle/manifest.json").read_text())
            self.assertEqual(manifest["files"]["run.sh"], package.sha256(root / "bundle/run.sh"))
            self.assertEqual(manifest["build"]["cmake_cache"]["CMAKE_BUILD_TYPE"], "RelWithDebInfo")
            self.assertEqual(manifest["build"]["binary_source_head"], "NOT_RECORDED")
            self.assertIn("build/CMakeCache.txt", manifest["files"])
            self.assertIn("perf/BCC/jeprof", manifest["optional_assets"]["not_bundled"])


if __name__ == "__main__":
    unittest.main()
