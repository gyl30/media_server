#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


SYSTEM_RUNTIME_LIBRARIES = {
    "ld-linux-x86-64.so.2",
    "libc.so.6",
    "libm.so.6",
    "libmvec.so.1",
    "libpthread.so.0",
    "libdl.so.2",
    "librt.so.1",
    "libutil.so.1",
    "libresolv.so.2",
    "libanl.so.1",
    "libnss_files.so.2",
    "libnss_dns.so.2",
}
NSS_MODULES = ("libsoftokn3.so", "libfreebl3.so", "libnssckbi.so")
NSS_MODULE_DIRECTORY = Path("/usr/lib/x86_64-linux-gnu/nss")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def runtime_libraries(executable):
    checked = subprocess.run(["ldd", str(executable)], stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30,
                             env={**os.environ, "LC_ALL": "C"})
    output = checked.stdout + checked.stderr
    if checked.returncode:
        with executable.open("rb") as source:
            header = source.read(18)
        if (len(header) >= 18 and header[:4] == b"\x7fELF" and
                int.from_bytes(header[16:18], "little" if header[5] == 1 else "big") in (2, 3) and
                ("not a dynamic executable" in output or "statically linked" in output)):
            return
        raise RuntimeError(f"ldd failed for {executable}: {output.strip()}")
    for line in output.splitlines():
        fields = line.split()
        if "not found" in line:
            raise RuntimeError(f"missing dependency for {executable}: {line}")
        if len(fields) >= 3 and fields[1] == "=>" and fields[2].startswith("/"):
            yield fields[0], Path(fields[2])
        elif fields and fields[0].startswith("/"):
            path = Path(fields[0])
            yield path.name, path


def main():
    parser = argparse.ArgumentParser(description="Package built media server and benchmark tools for a runtime-only machine")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--ffmpeg-bin", type=Path, default=Path("/home/gyl/bin/ffmpeg"))
    parser.add_argument("--ffprobe-bin", type=Path, default=Path("/home/gyl/bin/ffprobe"))
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--whip-fixture", type=Path, help="H.264/Opus fixture derived offline")
    parser.add_argument("--signaling-bin", type=Path, help="optional built Go signaling service for real authorization verification")
    args = parser.parse_args()
    if args.output.exists():
        parser.error(f"output already exists: {args.output}")

    head = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=all"], text=True)
    submodules = subprocess.check_output(["git", "submodule", "status", "--recursive"], text=True).splitlines()
    executables = ("media_server", "whep_fanout", "whip_publisher", "rtmp_fanout", "rtsp_fanout", "http_flv_fanout", "rtsp_publisher", "gb_ps_fanout")
    files = [(args.build_dir / name, Path("build") / name) for name in executables]
    files.extend((path, Path("bench") / path.name) for path in Path("bench").glob("*.py") if path.name != "package_runtime.py")
    files.append((Path("bench/requirements.txt"), Path("bench/requirements.txt")))
    if args.signaling_bin:
        files.append((args.signaling_bin, Path("bin/signaling")))
    files.extend(((args.ffprobe_bin, Path("bin/ffprobe")), (args.ffmpeg_bin, Path("bin/ffmpeg")), (args.fixture, Path(".cache/perf_fixture_720p30.flv"))))
    if args.whip_fixture:
        files.append((args.whip_fixture, Path(".cache/perf_fixture_whip.mkv")))
    cache = args.build_dir / "CMakeCache.txt"
    build = {"directory": str(args.build_dir.resolve()), "binary_source_head": "NOT_RECORDED",
             "note": "head describes the packaging source tree, not an independently verified ELF build commit"}
    if cache.is_file():
        files.append((cache, Path("build/CMakeCache.txt")))
        build["cmake_cache"] = {}
        for line in cache.read_text().splitlines():
            if not line or line.startswith(("#", "//")) or "=" not in line or ":" not in line.split("=", 1)[0]:
                continue
            key, value = line.split("=", 1)
            name = key.split(":", 1)[0]
            if name.startswith(("CMAKE_", "Boost_", "BOOST_", "FFMPEG_", "OPENSSL_", "MEDIA_SERVER_")):
                build["cmake_cache"][name] = value
    for source, _ in files:
        if not source.is_file():
            parser.error(f"missing runtime file: {source}")

    libraries = {}
    nss_modules = {name: NSS_MODULE_DIRECTORY / name for name in NSS_MODULES}
    for source in nss_modules.values():
        if not source.is_file():
            parser.error(f"missing NSS runtime module: {source}")
    for source, _ in files:
        if source.suffix in {".py", ".txt"} or source in {args.fixture, args.whip_fixture}:
            continue
        for name, path in runtime_libraries(source):
            if name in SYSTEM_RUNTIME_LIBRARIES:
                continue
            if name in libraries and sha256(libraries[name]) != sha256(path):
                raise RuntimeError(f"different libraries use the same SONAME: {name}")
            libraries[name] = path
    for source in nss_modules.values():
        for name, path in runtime_libraries(source):
            if name in SYSTEM_RUNTIME_LIBRARIES:
                continue
            if name in libraries and sha256(libraries[name]) != sha256(path):
                raise RuntimeError(f"different libraries use the same SONAME: {name}")
            libraries[name] = path

    args.output.mkdir(parents=True, mode=0o700)
    manifest = {"head": head, "dirty": bool(dirty), "worktree_status": dirty.splitlines(),
                "submodules": submodules, "build": build, "fixture_sha256": sha256(args.fixture),
                "files": {}, "libraries": {}, "nss_modules": {},
                "packaging_python": sys.version, "tool_versions": {},
                "host_requirements": {"python": ">=3.10", "python_packages": Path("bench/requirements.txt").read_text().splitlines(),
                                      "system_runtime": sorted(SYSTEM_RUNTIME_LIBRARIES), "commands": ["taskset", "ldd"]},
                "optional_assets": {"signaling": bool(args.signaling_bin), "whip_fixture": bool(args.whip_fixture),
                                    "not_bundled": ["Chrome/Firefox", "Playwright", "GB28181 simulator", "perf/BCC/jeprof"]}}
    for name, executable in (("ffmpeg", args.ffmpeg_bin), ("ffprobe", args.ffprobe_bin)):
        version = subprocess.run([str(executable), "-version"], stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10,
                                 env={**os.environ, "LC_ALL": "C"})
        manifest["tool_versions"][name] = {"exit_code": version.returncode,
                                           "version": (version.stdout or version.stderr).splitlines()[:1]}
    for source, destination in files:
        target = args.output / destination
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        manifest["files"][str(destination)] = sha256(target)
    for name, source in sorted(libraries.items()):
        target = args.output / "lib" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        manifest["libraries"][name] = {"source": str(source), "sha256": sha256(target)}
    for name, source in nss_modules.items():
        target = args.output / "lib" / "nss" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        manifest["nss_modules"][name] = {"source": str(source), "sha256": sha256(target)}

    runner = args.output / "run.sh"
    runner.write_text(
        "#!/bin/sh\n"
        "set -eu\n"
        'bundle_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
        'export LD_LIBRARY_PATH="$bundle_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\n'
        f"export MEDIA_SERVER_BENCH_HEAD={head}\n"
        'cd "$bundle_dir"\n'
        'exec "$@"\n'
    )
    runner.chmod(0o755)
    manifest["files"]["run.sh"] = sha256(runner)
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"packaged {len(files)} files, {len(libraries)} libraries and {len(nss_modules)} NSS modules from {head} into {args.output}")


if __name__ == "__main__":
    main()
