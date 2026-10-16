"""Run Windows MSVC test artifacts in an isolated Wine prefix."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def run_checked(command, environment, timeout):
    # Keep process output on disk so a failing executable cannot exhaust runner memory.
    with tempfile.TemporaryFile() as output:
        try:
            subprocess.run(command, env=environment, check=True, timeout=timeout,
                           stdout=output, stderr=subprocess.STDOUT)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
            output.seek(0)
            text = output.read(1024 * 1024).decode("utf-8", errors="replace")
            # Only executable/test identifiers enter the public annotation, never raw diagnostics.
            executable = Path(command[1] if command[0] == "wine" and len(command) > 1
                              and not command[1].startswith("-") else command[0]).name
            if not re.fullmatch(r"[A-Za-z0-9_.-]{1,160}", executable):
                executable = "process"
            failed = re.findall(r"^\s*([A-Za-z0-9_:]{1,256}) --- FAILED\s*$", text, re.MULTILINE)
            result = (f"exit {error.returncode}" if isinstance(error, subprocess.CalledProcessError)
                      else "timeout")
            suffix = f"; failed tests: {', '.join(failed[:10])}" if failed else ""
            print(f"::error::Wine validation: {executable}, {result}{suffix}", flush=True)
            raise
        finally:
            output.seek(0)
            # The ordinary job log retains bounded test output, separate from the annotation.
            print(output.read(1024 * 1024).decode("utf-8", errors="replace"), end="", flush=True)


def test_executables(directory, configuration):
    manifest = directory / "manifest.json"
    if manifest.stat().st_size > 262144:
        raise ValueError("Wine test manifest is too large")
    data = json.loads(manifest.read_text(encoding="utf-8-sig"))
    if data.get("schema_version") != 1 or data.get("configuration") != configuration:
        raise ValueError("Wine test manifest has an unexpected schema or configuration")
    names = data.get("executables")
    if not isinstance(names, list) or not 1 <= len(names) <= 256:
        raise ValueError("Wine test manifest must contain a bounded, nonempty executable list")
    if any(not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_-]+\.exe", name) for name in names):
        raise ValueError("Wine test manifest has an invalid executable name")
    if len(set(names)) != len(names):
        raise ValueError("Wine test manifest has duplicate executables")
    files = [directory / name for name in names]
    if any(not file.is_file() or file.is_symlink() for file in files):
        raise ValueError("Wine test manifest references a missing or linked executable")
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tests", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    args = parser.parse_args()
    tests = {configuration: test_executables(args.tests.resolve() / configuration, configuration)
             for configuration in ("debug", "release")}
    runtime = args.runtime.resolve()
    host = runtime / "manny_nexus_addon_smoke_test.exe"
    dlls = [runtime / kind / configuration / "manny_uploader.dll"
            for kind in ("normal", "active") for configuration in tests]
    if not host.is_file() or any(not file.is_file() for file in dlls):
        raise ValueError("Wine runtime artifacts are incomplete")
    prefix = args.prefix.resolve()
    if prefix.exists():
        raise ValueError("Wine validation requires a fresh, dedicated prefix")
    prefix.mkdir(parents=True)
    environment = os.environ.copy()
    environment.pop("MANNY_REQUIRE_NATIVE_DPAPI", None)
    environment.update(WINEPREFIX=str(prefix), WINEARCH="win64", WINEDEBUG="-all",
                       WINEDLLOVERRIDES="mscoree,mshtml=", MANNY_REQUIRE_WINE_DPAPI="1")

    def run(*command, timeout=180):
        run_checked(command, environment, timeout)

    def windows_path(path):
        return subprocess.check_output(["winepath", "-w", str(path)], env=environment,
                                       text=True, timeout=30).strip()

    def smoke(dll, *options):
        run("wine", str(host), windows_path(dll), *options)

    try:
        run("wine", "--version")
        # Starting a second server after wineboot returns Wine's already-running status 2.
        run("wineserver", "-p")
        run("wineboot", "-u")
        for configuration, executables in tests.items():
            for executable in executables:
                print(f"[Wine {configuration}] {executable.name}", flush=True)
                run("wine", str(executable), "--quiet")
            normal = runtime / "normal" / configuration / "manny_uploader.dll"
            smoke(normal)
            smoke(normal, "--lifecycle-proof")
            smoke(runtime / "active" / configuration / "manny_uploader.dll", "--active-runtime-proof")

        extracted = runtime / "package-verified"
        repository = Path(__file__).resolve().parent.parent
        run("cmake", f"-DMANNY_PACKAGE_OUTPUT_DIRECTORY={args.package.resolve()}",
            f"-DMANNY_PACKAGE_EXTRACT_DIRECTORY={extracted}", "-P",
            str(repository / "cmake/VerifyWindowsPackage.cmake"))
        run("cmake", f"-DMANNY_DLL_FILE={extracted / 'manny_uploader.dll'}",
            f"-DMANNY_NOTICES_FILE={repository / 'THIRD_PARTY_NOTICES.md'}", "-P",
            str(repository / "cmake/VerifyWindowsNotices.cmake"))
        smoke(extracted / "manny_uploader.dll")
        smoke(extracted / "manny_uploader.dll", "--lifecycle-proof")
    finally:
        # This process created the prefix and owns every Wine process started inside it.
        subprocess.run(["wineserver", "-k"], env=environment, check=True, timeout=30)


if __name__ == "__main__":
    main()
