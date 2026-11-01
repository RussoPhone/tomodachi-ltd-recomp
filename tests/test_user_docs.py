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

    def test_documented_entry_points_exist(self):
        for command_file in ("install.sh", "play.sh", "scripts/play-nxrt.sh", "scripts/launcher.py"):
            with self.subTest(command_file=command_file):
                self.assertTrue((ROOT / command_file).is_file())

        for doc in (ROOT / "README.md", ROOT / "README.pt-BR.md"):
            text = doc.read_text(encoding="utf-8")
            with self.subTest(doc=doc.name):
                for option in ("./play.sh", "--nogui", "--fullscreen", "--language", "./install.sh --report"):
                    self.assertIn(option, text)

    def test_ci_steps_match_installer(self):
        installer = (ROOT / "scripts/install.py").read_text(encoding="utf-8")
        self.assertIn("CI_STEPS = ('check', 'fetch', 'patches', 'backend')", installer)
