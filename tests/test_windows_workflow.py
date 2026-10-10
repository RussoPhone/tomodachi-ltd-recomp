import ast
import re
import unittest
from pathlib import Path


WORKFLOW = Path(__file__).resolve().parents[1] / ".github/workflows/windows.yml"
REQUIRED_PATHS = {
    "scripts/**",
    "patches/**",
    "upstream.lock.json",
    "install.bat",
    "play.bat",
    "tests/**",
    ".github/workflows/windows.yml",
}


class WindowsWorkflowTests(unittest.TestCase):
    def test_build_check_runs_for_pushes_and_pull_requests_with_windows_changes(self):
        workflow = WORKFLOW.read_text(encoding="utf-8")
        for event in ("push", "pull_request"):
            with self.subTest(event=event):
                match = re.search(rf"^  {event}:\s*\n((?:    .*\n|\n)*)", workflow, re.MULTILINE)
                self.assertIsNotNone(match, f"missing {event} trigger")
                paths_match = re.search(r"^    paths:\s*(\[[^\n]*\])\s*$", match.group(1), re.MULTILINE)
                self.assertIsNotNone(paths_match, f"{event} trigger must filter relevant paths")
                paths = set(ast.literal_eval(paths_match.group(1)))
                self.assertTrue(REQUIRED_PATHS.issubset(paths), f"{event} trigger misses {REQUIRED_PATHS - paths}")


if __name__ == "__main__":
    unittest.main()
