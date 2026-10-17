"""Build ESP-IDF firmware and publish a verified offline bundle; never flash.

ESP-IDF and tools are read-only inputs. On Windows, install them at paths
without spaces, or use this workspace's already patched isolated SDK.
"""
import argparse
import contextlib
import ctypes
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import uuid

PROJECT = Path(__file__).resolve().parent.parent
WORKSPACE = PROJECT.parent.parent
PINNED_IDF = (5, 2, 3)
COMPILER_RELEASE = "esp-13.2.0_20230928"
FLASH_SIZE = 16 * 1024 * 1024


def first_existing(candidates, label):
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return Path(candidate).resolve()
    raise RuntimeError(f"Cannot find {label}. Pass its explicit path; no SDK or tools are installed automatically.")


def directory_alias(path):
    """Use existing directory aliases; never shorten an executable filename."""
    path = Path(path).resolve()
    if os.name != "nt":
        return path
    suffix = []
    ancestor = path
    while not ancestor.exists():
        suffix.append(ancestor.name)
        ancestor = ancestor.parent
    output = ctypes.create_unicode_buffer(32768)
    length = ctypes.windll.kernel32.GetShortPathNameW(str(ancestor), output, len(output))
    if not length or length >= len(output):
        raise RuntimeError(f"Windows directory aliases are unavailable for {ancestor}. Use SDK/tools/build paths without spaces.")
    result = Path(output.value).joinpath(*reversed(suffix))
    if " " in str(result):
        raise RuntimeError("Windows could not remove spaces from a build path. Choose a directory without spaces.")
    return result


def executable_alias(path):
    path = Path(path).resolve()
    return directory_alias(path.parent) / path.name


def sdk_version(idf):
    source = (idf / "tools/cmake/version.cmake").read_text(encoding="utf-8")
    version = []
    for field in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(rf"set\(IDF_VERSION_{field}\s+(\d+)\)", source)
        if not match:
            raise RuntimeError("Cannot identify the ESP-IDF release.")
        version.append(int(match.group(1)))
    if tuple(version) != PINNED_IDF:
        raise RuntimeError(f"This project requires ESP-IDF 5.2.3; selected {'.'.join(map(str, version))}.")
    return ".".join(map(str, version))


def check_windows_toolchain(idf, tools, compiler):
    if os.name != "nt" or not any(" " in str(p) for p in (idf, tools)):
        return
    source = (idf / "tools/cmake/toolchain-esp32s3.cmake").read_text(encoding="utf-8")
    for kind, suffix in (("C", "gcc"), ("CXX", "g++"), ("ASM", "gcc")):
        match = re.search(rf'set\(CMAKE_{kind}_COMPILER\s+(?:"([^"]+)"|([^\s)]+))\)', source)
        value = (match.group(1) or match.group(2)) if match else ""
        expected = compiler / f"xtensa-esp32s3-elf-{suffix}.exe"
        candidate = Path(value)
        if (not candidate.is_absolute() or " " in value or
                candidate.name.lower() != expected.name.lower() or
                not candidate.exists() or not os.path.samefile(candidate, expected)):
            raise RuntimeError(
                "ESP-IDF/tools paths contain spaces and the selected SDK lacks the compatible compiler-path fix. "
                "The build wrapper does not modify SDK files. Use this workspace's prepared isolated SDK, "
                "or install ESP-IDF 5.2.3 and its tools under paths without spaces (for example C:\\Espressif)."
            )


def configuration(args):
    work = WORKSPACE / "work"
    for value, label in ((args.idf_path, "--idf-path"), (args.tools_path, "--tools-path"), (args.python_env, "--python-env")):
        if value is not None and not value.is_dir():
            raise RuntimeError(f"Explicit {label} directory does not exist: {value}")
    idf = first_existing([args.idf_path, os.environ.get("IDF_PATH"), work / "esp-idf"], "ESP-IDF (--idf-path)")
    tools = first_existing([args.tools_path, os.environ.get("IDF_TOOLS_PATH"), work / "idf-tools", Path.home() / ".espressif"], "IDF tools (--tools-path)")
    version = sdk_version(idf)
    environments = sorted((tools / "python_env").glob("idf5.2_py*_env"), reverse=True)
    venv = first_existing([args.python_env, os.environ.get("IDF_PYTHON_ENV_PATH"), *environments], "IDF Python environment (--python-env)")
    windows = os.name == "nt"
    executable = ".exe" if windows else ""
    scripts = venv / ("Scripts" if windows else "bin")
    python = scripts / ("python.exe" if windows else "python")
    compiler = tools / "tools/xtensa-esp-elf" / COMPILER_RELEASE / "xtensa-esp-elf/bin"
    gcc = compiler / f"xtensa-esp32s3-elf-gcc{executable}"
    ninja = first_existing([tools / f"tools/ninja/1.11.1/ninja{executable}",
        *sorted((tools / "tools/ninja").glob(f"*/ninja{executable}"), reverse=True)], "Ninja")
    cmake = first_existing([tools / f"tools/cmake/3.24.0/bin/cmake{executable}",
        *sorted((tools / "tools/cmake").glob(f"*/bin/cmake{executable}"), reverse=True)], "CMake")
    for path in (python, gcc, compiler / f"xtensa-esp32s3-elf-g++{executable}", idf / "tools/idf.py"):
        if not path.is_file():
            raise RuntimeError(f"Required build tool is missing: {path}")
    check_windows_toolchain(idf, tools, compiler)
    build = (args.build_dir or work / "esp32-dot-build").resolve()
    firmware = (args.firmware_dir or PROJECT / "firmware").resolve()
    if build == PROJECT or build == firmware or build in firmware.parents or firmware in build.parents:
        raise RuntimeError("Build and firmware directories must be separate and cannot contain one another.")
    env = os.environ.copy()
    env.update(IDF_PATH=str(directory_alias(idf)), IDF_TOOLS_PATH=str(directory_alias(tools)),
        IDF_PYTHON_ENV_PATH=str(directory_alias(venv)), IDF_CCACHE_ENABLE="0", IDF_TARGET="esp32s3")
    # Construct the subprocess PATH here: starting another PowerShell process
    # can restore its profile PATH and silently drop the selected toolchain.
    prefix = [directory_alias(scripts), directory_alias(compiler), directory_alias(ninja.parent), directory_alias(cmake.parent)]
    env["PATH"] = os.pathsep.join(map(str, prefix)) + os.pathsep + env.get("PATH", "")
    # Scope Git's SDK trust/ignore settings to this child process only.
    count = int(env.get("GIT_CONFIG_COUNT", "0"))
    for key, value in (("safe.directory", idf.as_posix()), ("safe.directory", directory_alias(idf).as_posix()),
            ("core.excludesFile", (directory_alias(build) / "empty-git-ignore").as_posix())):
        env[f"GIT_CONFIG_KEY_{count}"] = key
        env[f"GIT_CONFIG_VALUE_{count}"] = value
        count += 1
    env["GIT_CONFIG_COUNT"] = str(count)
    command = [str(executable_alias(python)), str(directory_alias(idf) / "tools/idf.py"),
        "-C", str(directory_alias(PROJECT)), "-B", str(directory_alias(build)),
        "-D", "IDF_TARGET=esp32s3", "-D", f"CMAKE_PROGRAM_PATH={directory_alias(compiler).as_posix()}",
        "-D", f"CMAKE_MAKE_PROGRAM={executable_alias(ninja).as_posix()}", "build"]
    return idf, tools, build, firmware, version, env, command


@contextlib.contextmanager
def build_lock(build):
    path = build / ".esp32-dot-build.lock"
    try:
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        raise RuntimeError(f"Another wrapper build holds {path}. Remove a stale lock only after confirming no build is running.") from None
    try:
        with os.fdopen(descriptor, "w") as stream:
            stream.write(str(os.getpid()) + "\n")
        yield
    finally:
        path.unlink(missing_ok=True)


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest() if hasattr(hashlib, "file_digest") else hashlib.sha256(stream.read()).hexdigest()


def build_artifacts(build):
    description = json.loads((build / "project_description.json").read_text(encoding="utf-8"))
    flash = json.loads((build / "flasher_args.json").read_text(encoding="utf-8"))
    if description.get("target") != "esp32s3" or description.get("project_name") != "esp32_dot":
        raise RuntimeError("Build metadata does not describe this ESP32-S3 project.")
    if flash.get("extra_esptool_args", {}).get("chip") != "esp32s3" or flash.get("flash_settings", {}).get("flash_size") != "16MB":
        raise RuntimeError("Build flash metadata does not target a 16 MB ESP32-S3.")
    result = {}

    def add(relative, name=None):
        source = (build / relative).resolve()
        if not source.is_relative_to(build.resolve()) or not source.is_file() or source.stat().st_size == 0:
            raise RuntimeError(f"Build artifact is missing, empty, or outside the build directory: {relative}")
        name = name or source.name
        if Path(name).name != name or name in result:
            raise RuntimeError(f"Duplicate or unsafe artifact filename: {name}")
        result[name] = source

    flash_files = flash.get("flash_files", {})
    if not isinstance(flash_files, dict) or not flash_files:
        raise RuntimeError("Build flash-file list is empty.")
    for offset, relative in flash_files.items():
        if not re.fullmatch(r"0x[0-9a-fA-F]+", offset) or not isinstance(relative, str):
            raise RuntimeError("Invalid build flash-file metadata.")
        add(relative)
    for required in ("esp32_dot.bin", "partition-table.bin", "bootloader.bin", "ota_data_initial.bin"):
        if required not in result:
            raise RuntimeError(f"Required firmware binary is absent: {required}")
    if result["esp32_dot.bin"].read_bytes()[0] != 0xE9:
        raise RuntimeError("App image has an invalid ESP image header.")
    add(description.get("app_elf", "esp32_dot.elf"), "esp32_dot.elf")
    config = Path(description.get("config_file", PROJECT / "sdkconfig")).resolve()
    if config != (PROJECT / "sdkconfig").resolve() or not config.is_file():
        raise RuntimeError("Compiled sdkconfig does not belong to this project.")
    if 'CONFIG_IDF_TARGET="esp32s3"' not in config.read_text(encoding="utf-8"):
        raise RuntimeError("Compiled sdkconfig is not for ESP32-S3.")
    result["sdkconfigcompiled.txt"] = config
    result["partitions.csv"] = PROJECT / "partitions.csv"
    flash["flash_files"] = {offset: Path(relative).name for offset, relative in flash_files.items()}
    for field in ("app", "bootloader", "partition-table", "otadata"):
        if isinstance(flash.get(field), dict) and "file" in flash[field]:
            flash[field]["file"] = Path(flash[field]["file"]).name
    return result, description, flash


def remove_owned_directory(path, build):
    resolved = path.resolve()
    if resolved.parent != build.resolve() or not resolved.name.startswith(("firmware-stage-", "firmware-previous-")):
        raise RuntimeError("Refusing to remove a directory outside this build's generated staging paths.")
    shutil.rmtree(resolved)


def publish_bundle(build, firmware, version):
    files, description, flash = build_artifacts(build)
    if firmware.exists():
        manifest_path = firmware / "manifest.json"
        if not manifest_path.is_file():
            raise RuntimeError("Existing firmware directory has no manifest. Choose another --firmware-dir to preserve its files.")
        old = json.loads(manifest_path.read_text(encoding="utf-8"))
        if not isinstance(old, dict):
            raise RuntimeError("Existing firmware manifest is not an object; refusing to replace it.")
        names = old.get("files", {})
        if old.get("target") != "esp32s3" or not isinstance(names, dict) or not names:
            raise RuntimeError("Existing firmware manifest is not recognized; refusing to replace it.")
        if {p.name for p in firmware.iterdir()} != set(names) | {"manifest.json"}:
            raise RuntimeError("Existing firmware directory contains unlisted files; refusing to replace them.")
        for name, checksum in names.items():
            if Path(name).name != name or not (firmware / name).is_file() or sha256(firmware / name) != checksum:
                raise RuntimeError("Existing firmware bundle has modified files; preserve it or choose another output directory.")
    firmware.parent.mkdir(parents=True, exist_ok=True)
    # Inherit workspace ACLs. Python's private temporary-directory ACL on
    # Windows can otherwise make the published output unreadable to the app.
    stage = build / ("firmware-stage-" + uuid.uuid4().hex)
    stage.mkdir(mode=0o777)
    previous = build / ("firmware-previous-" + uuid.uuid4().hex)
    try:
        for name, source in files.items():
            shutil.copyfile(source, stage / name)
        (stage / "flasher_args.json").write_text(json.dumps(flash, indent=2) + "\n", encoding="utf-8")
        metadata = {"project": "esp32_dot", "project_version": description.get("project_version"),
            "idf_version": version, "target": "esp32s3", "board": "lantern-original",
            "built_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "sdk_modified_by_wrapper": False, "device_flash_performed": False,
            "source_sha256": {p.relative_to(PROJECT).as_posix(): sha256(p)
                for p in sorted([PROJECT / "CMakeLists.txt", PROJECT / "sdkconfig.defaults",
                    PROJECT / "partitions.csv", *list((PROJECT / "main").rglob("*.c")),
                    *list((PROJECT / "main").rglob("*.h"))])}}
        (stage / "build_metadata.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
        manifest = {"format": "esp32-dot-firmware-bundle-v1", "target": "esp32s3", "flash_size": FLASH_SIZE,
            "app_offset": 0x20000, "files": {p.name: sha256(p) for p in sorted(stage.iterdir())}}
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        # Rename only complete, verified bundles. Build/output should be on the
        # same filesystem; a failed rename restores the existing bundle.
        if firmware.exists():
            os.replace(firmware, previous)
        try:
            os.replace(stage, firmware)
        except OSError:
            if previous.exists() and not firmware.exists():
                os.replace(previous, firmware)
            raise RuntimeError("Cannot publish atomically; place build/output on the same filesystem and close open bundle files.") from None
        if previous.exists():
            remove_owned_directory(previous, build)
    finally:
        if stage.exists():
            remove_owned_directory(stage, build)
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path)
    parser.add_argument("--tools-path", type=Path)
    parser.add_argument("--python-env", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--firmware-dir", type=Path)
    parser.add_argument("--check", action="store_true", help="Validate configuration only; do not compile or publish")
    parser.add_argument("--quiet", action="store_true", help="Write build output to build-wrapper.log; show its tail on failure")
    args = parser.parse_args(argv)
    idf, tools, build, firmware, version, env, command = configuration(args)
    print(f"ESP-IDF {version}\nProject: {PROJECT}\nSDK: {idf}\nTools: {tools}\nBuild: {build}\nFirmware: {firmware}", flush=True)
    if args.check:
        print("Configuration checked. No compilation, firmware output, or device access was performed.")
        return 0
    build.mkdir(parents=True, exist_ok=True)
    with build_lock(build):
        (build / "empty-git-ignore").touch()
        log_path = build / "build-wrapper.log"
        with log_path.open("w", encoding="utf-8") as log:
            if args.quiet:
                code = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
            else:
                with subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        encoding="utf-8", errors="replace") as process:
                    for line in process.stdout:
                        log.write(line)
                        sys.stdout.write(line)
                    code = process.wait()
        if code:
            if args.quiet:
                print("\n".join(log_path.read_text(encoding="utf-8", errors="replace").splitlines()[-80:]), file=sys.stderr)
            raise RuntimeError(f"ESP-IDF build failed with exit {code}. No firmware bundle published. Log: {log_path}")
        manifest = publish_bundle(build, firmware, version)
    print(f"Build and bundle verified: {firmware} ({len(manifest['files'])} files). No device was accessed or flashed.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"Build stopped: {exc}", file=sys.stderr)
        sys.exit(1)
