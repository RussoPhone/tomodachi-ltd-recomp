import importlib.util
import json
import pathlib
import socket
import sys
import tempfile
import types
import unittest
from unittest import mock


REPO = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = REPO / "scripts/export-aot.py"


class FakeGui:
    returncode = None

    def poll(self):
        return self.returncode

    def terminate(self):
        self.returncode = 0

    def wait(self, timeout=None):
        return self.returncode


def load_exporter(rpc):
    mcp = types.ModuleType("mcp")
    mcp.rpc = rpc
    with mock.patch.dict(sys.modules, {"mcp": mcp}):
        spec = importlib.util.spec_from_file_location("export_aot", SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    return module


class ExportAotTests(unittest.TestCase):
    def run_export(self, root, gui, rpc, *args):
        (root / "local").mkdir(parents=True, exist_ok=True)
        (root / "local/runtime.json").write_text(json.dumps({"dump_path": "fixture-dump"}))
        module = load_exporter(rpc)
        module.ROOT = root
        with mock.patch.object(module.lab, "ROOT", root), \
                mock.patch.object(module.lab, "tool", side_effect=lambda name: root / "fake-suyu"), \
                mock.patch.object(module.lab, "portable_profile"), \
                mock.patch.object(module, "port_open", side_effect=[False, True, True]), \
                mock.patch.object(module.subprocess, "Popen", return_value=gui), \
                mock.patch.object(module.time, "sleep"), \
                mock.patch.object(module.sys, "argv", [str(SCRIPT), *args]):
            return module.main()

    def test_creates_ignored_artifacts_directory_before_logging(self):
        with tempfile.TemporaryDirectory(prefix="export-aot-") as tmp:
            root = pathlib.Path(tmp)
            gui = FakeGui()

            def rpc(method, payload, timeout=30.0):
                if payload["name"] == "trigger_ui_action":
                    if payload["arguments"]["action"] == "export_game":
                        raise socket.timeout()
                    return {"result": {"content": [{"type": "text", "text": "{}"}]}}
                return {"result": {"content": [{
                    "type": "text", "text": json.dumps({"done": True, "success": True})
                }]}}

            result = self.run_export(root, gui, rpc)

            self.assertEqual(result, 0, (root / "artifacts/aot-export-status.json").read_text())
            self.assertTrue((root / "artifacts/aot-export-status.json").is_file())
            self.assertTrue((root / "artifacts/aot-export-gui.log").is_file())
            self.assertEqual(gui.returncode, 0)

    def test_status_rpc_timeout_is_capped_by_remaining_export_budget(self):
        with tempfile.TemporaryDirectory(prefix="export-aot-") as tmp:
            root = pathlib.Path(tmp)
            (root / "artifacts").mkdir()
            (root / "local").mkdir()
            (root / "local/runtime.json").write_text(json.dumps({"dump_path": "fixture-dump"}))
            gui = FakeGui()
            observed_timeouts = []

            def rpc(method, payload, timeout=30.0):
                if payload["name"] == "trigger_ui_action":
                    if payload["arguments"]["action"] == "export_game":
                        raise socket.timeout()
                    return {"result": {"content": [{"type": "text", "text": "{}"}]}}
                observed_timeouts.append(timeout)
                return {"result": {"content": [{
                    "type": "text", "text": json.dumps({"done": True, "success": True})
                }]}}

            module = load_exporter(rpc)
            module.ROOT = root
            with mock.patch.object(module.lab, "ROOT", root), \
                    mock.patch.object(module.lab, "tool", side_effect=lambda name: root / "fake-suyu"), \
                    mock.patch.object(module.lab, "portable_profile"), \
                    mock.patch.object(module, "port_open", side_effect=[False, True, True]), \
                    mock.patch.object(module.subprocess, "Popen", return_value=gui), \
                    mock.patch.object(module.time, "sleep"), \
                    mock.patch.object(module.time, "time", side_effect=[0.0, 59.5]), \
                    mock.patch.object(module.sys, "argv", [str(SCRIPT), "--timeout-min", "1"]):
                result = module.main()
            self.assertEqual(result, 0)
            self.assertEqual(observed_timeouts, [0.5])

    def test_status_write_failure_does_not_skip_gui_cleanup(self):
        with tempfile.TemporaryDirectory(prefix="export-aot-") as tmp:
            root = pathlib.Path(tmp)
            (root / "artifacts").mkdir()
            (root / "local").mkdir()
            (root / "local/runtime.json").write_text(json.dumps({"dump_path": "fixture-dump"}))
            gui = FakeGui()

            def rpc(method, payload, timeout=30.0):
                if payload["name"] == "trigger_ui_action":
                    if payload["arguments"]["action"] == "export_game":
                        raise socket.timeout()
                    return {"result": {"content": [{"type": "text", "text": "{}"}]}}
                return {"result": {"content": [{
                    "type": "text", "text": json.dumps({"done": True, "success": True})
                }]}}

            module = load_exporter(rpc)
            module.ROOT = root
            original_write_text = pathlib.Path.write_text

            def failing_status_write(path, *args, **kwargs):
                if path.name == "aot-export-status.json":
                    raise OSError("simulated status storage failure")
                return original_write_text(path, *args, **kwargs)

            with mock.patch.object(module.lab, "ROOT", root), \
                    mock.patch.object(module.lab, "tool", side_effect=lambda name: root / "fake-suyu"), \
                    mock.patch.object(module.lab, "portable_profile"), \
                    mock.patch.object(module, "port_open", side_effect=[False, True, True]), \
                    mock.patch.object(module.subprocess, "Popen", return_value=gui), \
                    mock.patch.object(module.time, "sleep"), \
                    mock.patch.object(module.sys, "argv", [str(SCRIPT)]), \
                    mock.patch.object(pathlib.Path, "write_text", failing_status_write):
                result = module.main()

            self.assertEqual(result, 1)
            self.assertEqual(gui.returncode, 0)


if __name__ == "__main__":
    unittest.main()
