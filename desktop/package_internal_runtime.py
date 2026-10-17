#!/usr/bin/env python3
"""为已验证的 Linux x86_64/Qt6 构建生成内部搬运目录，不代表公开分发许可闭合。"""

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone


PLUGINS = (
    "platforms/libqxcb.so",
    "platforms/libqoffscreen.so",
    "tls/libqopensslbackend.so",
)
FFMPEG_LIBRARIES = ("avutil", "avcodec", "avformat", "swscale", "swresample")
# 当前 Ubuntu/Qt 组合的 libproxy 0.4.17 会按编译期绝对路径 dlopen，ldd 看不到这些资源。
LIBPROXY_RESOURCES = (
    "modules/network_networkmanager.so",
    "modules/config_gnome3.so",
    "pxgsettings",
)
LIBPROXY_BUNDLE = Path("lib/libproxy/0.4.17")
HOST_LIBRARIES = re.compile(
    r"^(ld-linux[^/]*|libc\.so\..*|libm\.so\..*|libpthread\.so\..*|libdl\.so\..*|"
    r"librt\.so\..*|libresolv\.so\..*|libutil\.so\..*|libanl\.so\..*|libnss_[^/]*)$"
)


def run(command, env=None):
    return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT, env=env, timeout=60).strip()


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def dependencies(binary, env):
    output = run(["ldd", str(binary)], env)
    if "not found" in output or "undefined symbol:" in output:
        raise RuntimeError(f"依赖未解析：{binary}\n{output}")
    result = {}
    for line in output.splitlines():
        match = re.match(r"\s*(\S+) => (/.+?) \(", line)
        if match:
            result[match[1]] = Path(match[2]).resolve(strict=True)
        else:
            match = re.match(r"\s*(/.+?) \(", line)
            if match:
                path = Path(match[1]).resolve(strict=True)
                result[path.name] = path
    return result


def package_notice(path, licenses):
    # 自定义编译器目录不是系统包的产物；不能借用另一个 GCC 包的版权说明冒充匹配。
    for candidate in (path, Path("/lib") / path.relative_to("/usr/lib") if path.is_relative_to("/usr/lib") else path):
        owner = subprocess.run(["dpkg-query", "-S", str(candidate)], text=True, capture_output=True, timeout=10)
        if owner.returncode != 0:
            continue
        package = owner.stdout.splitlines()[0].rsplit(": ", 1)[0]
        version = run(["dpkg-query", "-W", "-f=${Version}", package])
        notice = Path("/usr/share/doc") / package.split(":")[0] / "copyright"
        if not notice.is_file():
            return {"package": package, "version": version, "copyright": None}
        destination = licenses / "system-packages" / package.replace(":", "_") / "copyright"
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(notice, destination)
        return {"package": package, "version": version, "copyright": str(destination.relative_to(licenses.parent)),
                "copyright_source": str(notice), "copyright_sha256": sha256(destination)}
    return {"package": None, "version": None, "copyright": None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True, help="已完成的 desktop CMake 构建目录")
    parser.add_argument("--ffmpeg-prefix", type=Path, required=True, help="本轮 shared LGPL FFmpeg 安装前缀")
    parser.add_argument("--ffmpeg-source", type=Path, required=True, help="该 FFmpeg 的完整匹配源码目录，提供许可原件")
    parser.add_argument("--qt-plugin-dir", type=Path, required=True, help="与实际链接 Qt 匹配的 plugins 根目录")
    parser.add_argument("--output-dir", type=Path, required=True, help="必须不存在的新运行目录；失败时保留现场")
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        parser.error("仅验收 Linux x86_64 内部搬运包")
    output = args.output_dir.absolute()
    if output.exists() or output.is_symlink():
        parser.error(f"输出目录已存在，拒绝覆盖：{output}")
    output = output.resolve()
    build = args.build_dir.resolve(strict=True)
    prefix = args.ffmpeg_prefix.resolve(strict=True)
    source = args.ffmpeg_source.resolve(strict=True)
    plugins = args.qt_plugin_dir.resolve(strict=True)
    repository = Path(__file__).resolve().parent.parent
    for item in (build / "gb_monitor", build / "CMakeCache.txt", source / "RELEASE", source / "LICENSE.md", source / "COPYING.LGPLv2.1"):
        if not item.is_file():
            parser.error(f"缺少构建/许可文件：{item}")
    for relative in PLUGINS:
        if not (plugins / relative).is_file():
            parser.error(f"缺少 Qt 插件：{plugins / relative}")

    ffmpeg = {}
    ffmpeg_version = None
    for name in FFMPEG_LIBRARIES:
        library = (prefix / "lib" / f"lib{name}.so").resolve(strict=True)
        if not library.is_relative_to(prefix):
            parser.error(f"FFmpeg 库越出私有前缀：{library}")
        loaded = ctypes.CDLL(str(library), mode=ctypes.RTLD_GLOBAL)
        if name == "avutil":
            loaded.av_version_info.restype = ctypes.c_char_p
            ffmpeg_version = loaded.av_version_info().decode()
        license_function = getattr(loaded, name + "_license")
        license_function.restype = ctypes.c_char_p
        configuration_function = getattr(loaded, name + "_configuration")
        configuration_function.restype = ctypes.c_char_p
        license_text = license_function().decode()
        if license_text != "LGPL version 2.1 or later":
            parser.error(f"本内部包要求私有 LGPL 2.1 shared SDK，实际 {name}: {license_text}")
        ffmpeg[name] = {"library": str(library), "license": license_text,
                        "configuration": configuration_function().decode()}
    if ffmpeg_version != (source / "RELEASE").read_text().strip():
        parser.error(f"FFmpeg 实际运行版本与许可源码不匹配：{ffmpeg_version}")

    # Qt OpenSSL backend 通过 dlopen 加载这些库，不能只依赖 ldd 静态闭包。
    openssl_dir = Path(run(["pkg-config", "--variable=libdir", "openssl"]))
    tls_libraries = [openssl_dir / "libssl.so.3", openssl_dir / "libcrypto.so.3"]
    for library in tls_libraries:
        if not library.is_file():
            parser.error(f"本已验证的 Qt/OpenSSL 3 组合缺少运行库：{library}")
    env = os.environ.copy()
    env.pop("LD_PRELOAD", None)
    env["LD_LIBRARY_PATH"] = str(prefix / "lib")
    main_libraries = dependencies(build / "gb_monitor", env)
    libproxy_resources = {}
    if "libproxy.so.1" in main_libraries:
        libproxy_source = main_libraries["libproxy.so.1"].parent / "libproxy/0.4.17"
        for relative in LIBPROXY_RESOURCES:
            resource = libproxy_source / relative
            if not resource.is_file():
                parser.error(f"本已验证的 libproxy 0.4.17 组合缺少运行资源：{resource}")
            libproxy_resources[LIBPROXY_BUNDLE / relative] = resource
    # 主程序先载入的同 SONAME runtime 会被插件复用；单独 ldd 插件否则可能选中宿主旧 GCC。
    runtime_directories = {str(path.parent) for name, path in main_libraries.items()
                           if name in ("libstdc++.so.6", "libgcc_s.so.1", "libgomp.so.1")}
    env["LD_LIBRARY_PATH"] = os.pathsep.join([str(prefix / "lib"), *sorted(runtime_directories)])
    roots = [build / "gb_monitor", *(plugins / relative for relative in PLUGINS), *tls_libraries, *libproxy_resources.values()]
    libraries = {}
    for binary in roots:
        for name, path in dependencies(binary, env).items():
            previous = libraries.get(name)
            if previous is not None and previous != path:
                # 同一 SONAME 必须使用同一已解析构件，而不是静默覆盖。
                raise RuntimeError(f"库冲突 {name}: {previous} / {path}")
            libraries[name] = path
    for library in tls_libraries:
        libraries[library.name] = library.resolve()
    for name, details in ffmpeg.items():
        resolved = Path(details["library"])
        if resolved not in libraries.values():
            raise RuntimeError(f"gb_monitor 未实际动态使用所指定的 FFmpeg 库：{name}")

    output.mkdir()
    (output / "lib").mkdir()
    licenses = output / "licenses"
    licenses.mkdir()
    install_output = run(["cmake", "--install", str(build), "--prefix", str(output)])
    (output / "install.log").write_text(install_output + "\n")
    plugin_outputs = []
    for relative in PLUGINS:
        target = output / "lib/qt6/plugins" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(plugins / relative, target)
        plugin_outputs.append(target)
    metadata = []
    unresolved_licenses = []
    package_notices = {}
    for name, path in sorted(libraries.items()):
        host = bool(HOST_LIBRARIES.match(name))
        entry = {"soname": name, "source": str(path), "sha256": sha256(path), "host_provided": host}
        if not host:
            destination = output / "lib" / name
            shutil.copy2(path, destination)
            entry["bundle_path"] = str(destination.relative_to(output))
            if path.is_relative_to(prefix):
                entry["license"] = "licenses/ffmpeg/COPYING.LGPLv2.1"
            else:
                if path not in package_notices:
                    package_notices[path] = package_notice(path, licenses)
                entry.update(package_notices[path])
                if entry["copyright"] is None:
                    unresolved_licenses.append(str(path))
        metadata.append(entry)
    # Qt 6.2 的 OpenSSL 动态查找首先尝试 unversioned 名称；不能依赖宿主开发包提供别名。
    runtime_aliases = {"libssl.so": "libssl.so.3", "libcrypto.so": "libcrypto.so.3"}
    for alias, target in runtime_aliases.items():
        (output / "lib" / alias).symlink_to(target)
    plugin_metadata = []
    for plugin in plugin_outputs:
        entry = package_notice(plugins / plugin.relative_to(output / "lib/qt6/plugins"), licenses)
        entry.update({"bundle_path": str(plugin.relative_to(output)), "sha256": sha256(plugin)})
        plugin_metadata.append(entry)
        if entry["copyright"] is None:
            unresolved_licenses.append(str(plugin))
    libproxy_outputs = []
    libproxy_metadata = []
    for relative, resource in libproxy_resources.items():
        destination = output / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(resource, destination)
        libproxy_outputs.append(destination)
        entry = package_notice(resource, licenses)
        entry.update({"source": str(resource), "bundle_path": str(relative), "sha256": sha256(destination)})
        libproxy_metadata.append(entry)
        if entry["copyright"] is None:
            unresolved_licenses.append(str(resource))
    (licenses / "ffmpeg").mkdir()
    for name in ("LICENSE.md", "COPYING.LGPLv2.1"):
        shutil.copy2(source / name, licenses / "ffmpeg" / name)
    for name in ("GPL-2", "GPL-3", "LGPL-2.1", "LGPL-3", "Apache-2.0"):
        notice = Path("/usr/share/common-licenses") / name
        if notice.is_file():
            shutil.copy2(notice, licenses / name)
    (output / "bin/qt.conf").write_text("[Paths]\nPrefix=..\nLibraries=lib\nPlugins=lib/qt6/plugins\n")
    wrapper = output / "run.sh"
    wrapper.write_text(
        '#!/bin/sh\nset -eu\n'
        'root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
        'export LD_LIBRARY_PATH="$root/lib"\n'
        'export QT_PLUGIN_PATH="$root/lib/qt6/plugins"\n'
        + ('export PX_MODULE_PATH="$root/lib/libproxy/0.4.17/modules"\n'
           'export PX_GSETTINGS="$root/lib/libproxy/0.4.17/pxgsettings"\n'
           'if [ "${GNOME_DESKTOP_SESSION_ID+x}" = x ] || [ "${DESKTOP_SESSION-}" = gnome ] || [ "${DESKTOP_SESSION-}" = mate ]; then\n'
           '  case "$root" in *[!A-Za-z0-9_./-]*)\n'
           '    echo "libproxy 0.4.17 的 GNOME helper 不支持此安装路径，请使用仅含字母、数字、下划线、点、斜线和连字符的路径" >&2\n'
           '    exit 1;;\n'
           '  esac\n'
           'fi\n' if libproxy_resources else '')
        +
        'unset LD_PRELOAD QT_QPA_PLATFORM_PLUGIN_PATH\n'
        'exec "$root/bin/gb_monitor" "$@"\n'
    )
    wrapper.chmod(0o755)
    (output / "README.md").write_text(
        '# gb_monitor 内部搬运验证包\n\n仅供内部验证，禁止据此公开发行。项目自身许可证、'
        '自定义 GCC runtime 许可/来源及完整分发义务尚未闭合；详见 manifest.json。\n\n'
        '运行 `./run.sh`；可以整体移动目录。无需设置开发机库路径。\n'
        '只验收 Linux x86_64、构建机同代 glibc ABI；不附带 glibc、加载器、字体、显卡驱动、'
        'X11/Wayland/PulseAudio 服务或系统 CA。真实图形、声音和网络仍需目标系统提供。\n'
        '仅包含 xcb/offscreen 平台及 OpenSSL 3 TLS 插件；不附带输入法插件。\n'
        'libproxy 0.4.17 的现有 NetworkManager/GSettings 模块和辅助程序随包携带；'
        'D-Bus/NetworkManager 服务、GNOME GSettings schema 与用户代理配置仍由目标桌面系统提供，'
        '本包不声称验证完整桌面代理/PAC 环境。\n'
        'libproxy 0.4.17 的 GNOME helper 使用 shell 执行路径；GNOME/MATE 模式仅接受由字母、数字、'
        '下划线、点、斜线及连字符组成的安装路径，否则启动器明确拒绝，不静默跳过代理。\n'
        'manifest 中 dirty 构建不能冒充纯净 HEAD 可重现发行版；原始路径是追溯信息，不是运行依赖。\n'
    )
    verify_env = env.copy()
    verify_env["LD_LIBRARY_PATH"] = str(output / "lib")
    verification = {}
    for binary in [output / "bin/gb_monitor", *plugin_outputs, *libproxy_outputs]:
        text = run(["ldd", "-r", str(binary)], verify_env)
        if "not found" in text or "undefined symbol:" in text:
            raise RuntimeError(f"运行包动态链接失败：{binary}\n{text}")
        verification[str(binary.relative_to(output))] = text
        for name, path in dependencies(binary, verify_env).items():
            if not HOST_LIBRARIES.match(name) and not path.is_relative_to(output):
                raise RuntimeError(f"非 glibc 依赖逃逸出运行包：{name}: {path}")
    dirty = run(["git", "-C", str(repository), "status", "--porcelain=v1", "--untracked-files=all"])
    cache = (build / "CMakeCache.txt").read_text()
    compiler_match = re.search(r"^CMAKE_CXX_COMPILER:(?:FILEPATH|STRING)=(.+)$", cache, re.MULTILINE)
    source_files = [path for path in (repository / "desktop").rglob("*")
                    if path.is_file() and (path.suffix in (".cpp", ".h", ".py", ".md") or path.name == "CMakeLists.txt")]
    source_files += [repository / "signaling/gb_http.go", repository / "signaling/gb_http_test.go"]
    manifest = {
        "scope": "INTERNAL_VALIDATION_ONLY", "public_distribution": "BLOCKED",
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "git_head": run(["git", "-C", str(repository), "rev-parse", "HEAD"]),
        "git_dirty": bool(dirty), "git_status": dirty.splitlines(),
        "package_script_sha256": sha256(Path(__file__)),
        "build_dir": str(build), "build_binary_sha256": sha256(build / "gb_monitor"),
        "cmake_cache_sha256": sha256(build / "CMakeCache.txt"),
        "compiler_version": run([compiler_match[1], "--version"]) if compiler_match else "NOT_RECORDED",
        "build_settings": [line for line in cache.splitlines() if line.startswith((
            "CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS", "FFMPEG_", "Qt6_DIR:"))],
        "host": {"machine": platform.machine(), "kernel": platform.release(),
                 "glibc": run(["getconf", "GNU_LIBC_VERSION"]),
                 "os_release": Path("/etc/os-release").read_text()},
        "ffmpeg_version": ffmpeg_version, "ffmpeg": ffmpeg,
        "qt_plugin_source": str(plugins), "plugins": plugin_metadata, "libraries": metadata,
        "libproxy_resources": libproxy_metadata,
        "runtime_aliases": runtime_aliases,
        "project_source_sha256": {str(path.relative_to(repository)): sha256(path) for path in sorted(source_files)},
        "unresolved_license_sources": sorted(set(unresolved_licenses)),
        "project_license": "NOT_DECLARED; public redistribution not authorized by this package",
        "link_verification": verification,
        "files": {str(path.relative_to(output)): sha256(path) for path in sorted(output.rglob("*")) if path.is_file()},
    }
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    files = [path for path in sorted(output.rglob("*")) if path.is_file()]
    (output / "SHA256SUMS").write_text("".join(f"{sha256(path)}  {path.relative_to(output)}\n" for path in files))
    print(json.dumps({"result": "PASS", "scope": "internal packaging and ldd only; no media/deployment claim",
                      "output": str(output), "manifest_sha256": sha256(output / "manifest.json"),
                      "git_dirty": bool(dirty), "public_distribution": "BLOCKED",
                      "unresolved_license_sources": sorted(set(unresolved_licenses))}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"内部运行包生成失败（已有现场保留）：{error}", file=sys.stderr)
        sys.exit(1)
