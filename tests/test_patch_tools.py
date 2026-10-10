import pathlib
import json
import shutil
import subprocess
import tempfile
import unittest


REPO = pathlib.Path(__file__).resolve().parents[1]


class PatchToolTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="patch-tools-test-")
        self.root = pathlib.Path(self.temp.name)
        tools = self.root / "research/tools"
        tools.mkdir(parents=True)
        (self.root / "patches").mkdir()
        self.source = self.root / "upstream/mk8-recomp/third_party/suyu"
        self.source.mkdir(parents=True)
        shutil.copy(REPO / "research/tools/mkpatch.sh", tools / "mkpatch.sh")
        shutil.copy(REPO / "research/tools/verify-patches.sh", tools / "verify-patches.sh")
        self.git("init", "-q")

    def tearDown(self):
        self.temp.cleanup()

    def git(self, *args):
        return subprocess.run(
            ["git", *args], cwd=self.source, check=True, capture_output=True, text=True
        )

    def commit_source(self):
        self.git("add", "-A")
        self.git(
            "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
            "commit", "-qm", "fixture base",
        )

    def run_tool(self, tool, *args):
        return subprocess.run(
            ["bash", str(self.root / "research/tools" / tool), *args],
            cwd=self.root, capture_output=True, text=True,
        )

    def test_mkpatch_writes_relative_output_in_calling_directory(self):
        (self.source / "sample.txt").write_text("pristine base\n")
        self.commit_source()
        (self.root / "patches/0001-base.patch").write_text(
            "diff --git a/sample.txt b/sample.txt\n"
            "--- a/sample.txt\n+++ b/sample.txt\n"
            "@@ -1 +1 @@\n-pristine base\n+patched base\n"
        )
        (self.source / "sample.txt").write_text("working tree\n")

        result = self.run_tool("mkpatch.sh", "generated.patch", "1", "sample.txt")

        self.assertEqual(result.returncode, 0, result.stderr)
        output = self.root / "generated.patch"
        self.assertTrue(output.is_file())
        patch = output.read_text()
        self.assertIn("-patched base", patch)
        self.assertIn("+working tree", patch)

    def test_verify_patches_detects_deleted_file_missing_from_working_tree(self):
        (self.source / "gone.txt").write_text("still present\n")
        self.commit_source()
        (self.root / "patches/0001-delete.patch").write_text(
            "diff --git a/gone.txt b/gone.txt\n"
            "deleted file mode 100644\n"
            "--- a/gone.txt\n+++ /dev/null\n"
            "@@ -1 +0,0 @@\n-still present\n"
        )

        result = self.run_tool("verify-patches.sh")

        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("gone.txt", result.stdout + result.stderr)

    def test_verify_patches_accepts_matching_regular_file(self):
        (self.source / "sample.txt").write_text("pristine\n")
        self.commit_source()
        (self.root / "patches/0001-change.patch").write_text(
            "diff --git a/sample.txt b/sample.txt\n"
            "--- a/sample.txt\n+++ b/sample.txt\n"
            "@@ -1 +1 @@\n-pristine\n+patched\n"
        )
        (self.source / "sample.txt").write_text("patched\n")

        result = self.run_tool("verify-patches.sh")

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("all 1 patches reproduce the tree (1 files)", result.stdout)

    def test_verify_upstream_checks_locked_revision_without_network(self):
        scripts = self.root / "scripts"
        scripts.mkdir()
        shutil.copy(REPO / "scripts/verify-upstream.py", scripts / "verify-upstream.py")
        upstream = self.root / "upstream/mk8-recomp"
        upstream.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "init", "-q"], cwd=upstream, check=True)
        (upstream / "fixture.txt").write_text("pinned revision\n")
        subprocess.run(["git", "add", "fixture.txt"], cwd=upstream, check=True)
        subprocess.run(
            ["git", "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
             "commit", "-qm", "fixture"], cwd=upstream, check=True,
        )
        commit = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=upstream, text=True
        ).strip()
        lock = self.root / "fixture.lock.json"
        lock.write_text(json.dumps({"commit": commit, "submodules": []}))

        result = subprocess.run(
            ["python3", str(scripts / "verify-upstream.py"), "--lock", str(lock)],
            cwd=self.root, capture_output=True, text=True,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Verified backend and 0 pinned submodules.", result.stdout)

        lock.write_text(json.dumps({"commit": "0" * 40, "submodules": []}))
        drift = subprocess.run(
            ["python3", str(scripts / "verify-upstream.py"), "--lock", str(lock)],
            cwd=self.root, capture_output=True, text=True,
        )
        self.assertEqual(drift.returncode, 1)
        self.assertIn("Backend revision drift", drift.stderr)


if __name__ == "__main__":
    unittest.main()
