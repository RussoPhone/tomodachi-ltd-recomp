import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DOCS = (ROOT / "README.md", ROOT / "README.pt-BR.md", ROOT / "CONTRIBUTING.md")


class UserDocsTests(unittest.TestCase):
    def test_relative_markdown_links_resolve(self):
        for doc in DOCS:
            text = doc.read_text(encoding="utf-8")
            links = re.findall(r"(?<!!)\[[^]]*\]\(([^)]+)\)", text)
            for target in links:
                if target.startswith(("https://", "http://", "mailto:", "#")):
                    continue
                with self.subTest(doc=doc.name, target=target):
                    path = target.split("#", 1)[0]
                    self.assertTrue((doc.parent / path).exists(), f"broken relative link: {target}")

    def test_documented_entry_points_and_windows_patch_option_exist(self):
        for command_file in ("install.sh", "install.bat", "play.sh", "play.bat"):
            with self.subTest(command_file=command_file):
                self.assertTrue((ROOT / command_file).is_file())

        installer = (ROOT / "scripts/windows/install.ps1").read_text(encoding="utf-8")
        self.assertRegex(installer, r"param\([^\n]*\[string\]\$PatchExe")

        for doc in (ROOT / "README.md", ROOT / "README.pt-BR.md"):
            text = doc.read_text(encoding="utf-8")
            with self.subTest(doc=doc.name):
                self.assertIn("-PatchExe", text)
                for option in ("--fullscreen", "--scale 2"):
                    self.assertIn(option, text)

    def test_windows_ci_documentation_matches_ci_steps(self):
        installer = (ROOT / "scripts/install.py").read_text(encoding="utf-8")
        self.assertIn("CI_STEPS = ('check', 'fetch', 'patches', 'backend')", installer)

        self.assertIn("steps 1, 3, 4 and 5", (ROOT / "README.md").read_text(encoding="utf-8"))
        self.assertIn("passos 1, 3, 4 e 5", (ROOT / "README.pt-BR.md").read_text(encoding="utf-8"))
        self.assertIn("steps 1, 3, 4 and 5", (ROOT / "CONTRIBUTING.md").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
