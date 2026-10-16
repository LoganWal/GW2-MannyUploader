import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location(
    "wine_runner", Path(__file__).resolve().parents[2] / "tools/test-rust-wine.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class ManifestTests(unittest.TestCase):
    def test_manifest_selects_every_declared_test_and_ignores_other_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("unit-123.exe", "integration-456.exe", "unrelated.exe"):
                (root / name).write_bytes(b"MZ")
            names = ["unit-123.exe", "integration-456.exe"]
            (root / "manifest.json").write_text(json.dumps({
                "schema_version": 1, "configuration": "debug", "executables": names
            }), encoding="utf-8-sig")
            self.assertEqual([file.name for file in runner.test_executables(root, "debug")], names)

    def test_invalid_manifests_cannot_silently_skip_or_escape_tests(self):
        for names in ([], ["../outside.exe"], ["C:/outside.exe"], ["unit.exe", "unit.exe"],
                      ["missing.exe"], [None], "unit.exe", ["unit.exe"] * 257):
            with self.subTest(names=names), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / "unit.exe").write_bytes(b"MZ")
                (root / "manifest.json").write_text(json.dumps({
                    "schema_version": 1, "configuration": "debug", "executables": names
                }), encoding="utf-8")
                with self.assertRaises(ValueError):
                    runner.test_executables(root, "debug")

    def test_release_cannot_reuse_a_debug_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "unit.exe").write_bytes(b"MZ")
            (root / "manifest.json").write_text(json.dumps({
                "schema_version": 1, "configuration": "debug", "executables": ["unit.exe"]
            }), encoding="utf-8")
            with self.assertRaises(ValueError):
                runner.test_executables(root, "release")


class WineRunTests(unittest.TestCase):
    def fixtures(self, root):
        for configuration in ("debug", "release"):
            directory = root / "tests" / configuration
            directory.mkdir(parents=True)
            (directory / "suite.exe").write_bytes(b"MZ")
            (directory / "manifest.json").write_text(json.dumps({
                "schema_version": 1, "configuration": configuration, "executables": ["suite.exe"]
            }), encoding="utf-8")
            for kind in ("normal", "active"):
                dll = root / "runtime" / kind / configuration / "manny_uploader.dll"
                dll.parent.mkdir(parents=True)
                dll.write_bytes(b"MZ")
        (root / "runtime/manny_nexus_addon_smoke_test.exe").write_bytes(b"MZ")
        return ["test-rust-wine.py", "--tests", str(root / "tests"),
                "--runtime", str(root / "runtime"), "--package", str(root / "package"),
                "--prefix", str(root / "prefix")]

    def test_both_configurations_and_packaged_copy_run_in_owned_wine_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            arguments = self.fixtures(root)
            with mock.patch("sys.argv", arguments), mock.patch.object(runner.subprocess, "run") as run, \
                    mock.patch.object(runner.subprocess, "check_output", return_value="Z:\\fixture.dll\n"):
                runner.main()
            commands = [call.args[0] for call in run.call_args_list]
            self.assertEqual(sum(command[-1] == "--quiet" for command in commands), 2)
            self.assertEqual(sum(command[-1] == "--lifecycle-proof" for command in commands), 3)
            self.assertEqual(sum(command[-1] == "--active-runtime-proof" for command in commands), 2)
            self.assertEqual(commands[-1], ["wineserver", "-k"])
            for call in run.call_args_list:
                self.assertEqual(call.kwargs["env"]["MANNY_REQUIRE_WINE_DPAPI"], "1")
                self.assertNotIn("MANNY_REQUIRE_NATIVE_DPAPI", call.kwargs["env"])
                # Windows TEMP can contain an 8.3 alias, while the owned prefix is canonical.
                self.assertEqual(call.kwargs["env"]["WINEPREFIX"], str((root / "prefix").resolve()))
                self.assertTrue(call.kwargs["check"])
                self.assertGreater(call.kwargs["timeout"], 0)

    def test_test_failure_stops_validation_and_still_stops_owned_wine_processes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            arguments = self.fixtures(root)

            def fail_suite(command, **kwargs):
                if command[-1] == "--quiet":
                    raise runner.subprocess.CalledProcessError(1, command)

            with mock.patch("sys.argv", arguments), \
                    mock.patch.object(runner.subprocess, "run", side_effect=fail_suite) as run:
                with self.assertRaises(runner.subprocess.CalledProcessError):
                    runner.main()
            commands = [call.args[0] for call in run.call_args_list]
            self.assertEqual(commands[-1], ["wineserver", "-k"])
            self.assertFalse(any(command[0] == "cmake" for command in commands))

    def test_existing_prefix_is_rejected_before_launching_wine(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            arguments = self.fixtures(root)
            (root / "prefix").mkdir()
            with mock.patch("sys.argv", arguments), mock.patch.object(runner.subprocess, "run") as run:
                with self.assertRaises(ValueError):
                    runner.main()
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
