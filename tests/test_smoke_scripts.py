"""Keep all smoke subprocesses on the requested server build."""
import importlib.util
import os
from pathlib import Path
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]


class SmokeBinarySelectionTests(unittest.TestCase):
    def test_all_launchers_honor_sanitizer_binary(self):
        binary = "./build-sanitize/mini_oss"
        launchers = {
            "smoke_test_http": ("start_server", "start_resource_guard_server"),
            "smoke_test_redis_cache": ("start_server",),
        }
        for script, functions in launchers.items():
            spec = importlib.util.spec_from_file_location(
                script, ROOT / "scripts" / (script + ".py")
            )
            module = importlib.util.module_from_spec(spec)
            with patch.dict(os.environ, {"MINI_OSS_BINARY": binary}):
                spec.loader.exec_module(module)
            for function in functions:
                with self.subTest(script=script, launcher=function):
                    with patch.object(module.subprocess, "Popen") as popen:
                        with patch.object(module.time, "sleep"):
                            getattr(module, function)(Path("smoke.ini"))
                    command = popen.call_args.args[0]
                    self.assertEqual(command[0], binary)
                    self.assertEqual(command[1:3], ["--config", "smoke.ini"])


if __name__ == "__main__":
    unittest.main()
