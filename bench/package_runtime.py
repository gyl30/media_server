#!/usr/bin/env python3

import argparse
import hashlib
import json
import shutil
import subprocess
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
    output = subprocess.check_output(["ldd", str(executable)], text=True)
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
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    args = parser.parse_args()
    if args.output.exists():
        parser.error(f"output already exists: {args.output}")

    head = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    executables = ("media_server", "whep_fanout", "rtmp_fanout", "rtsp_fanout", "gb_ps_fanout")
    files = [(args.build_dir / name, Path("build") / name) for name in executables]
    files.extend((path, Path("bench") / path.name) for path in Path("bench").glob("*.py") if path.name != "package_runtime.py")
    files.extend(((args.ffmpeg_bin, Path("bin/ffmpeg")), (args.fixture, Path(".cache/perf_fixture_720p30.flv"))))
    for source, _ in files:
        if not source.is_file():
            parser.error(f"missing runtime file: {source}")

    libraries = {}
    nss_modules = {name: NSS_MODULE_DIRECTORY / name for name in NSS_MODULES}
    for source in nss_modules.values():
        if not source.is_file():
            parser.error(f"missing NSS runtime module: {source}")
    for source, _ in files:
        if source.suffix == ".py" or source == args.fixture:
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

    args.output.mkdir(parents=True)
    manifest = {"head": head, "files": {}, "libraries": {}, "nss_modules": {}}
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
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"packaged {len(files)} files, {len(libraries)} libraries and {len(nss_modules)} NSS modules from {head} into {args.output}")


if __name__ == "__main__":
    main()
