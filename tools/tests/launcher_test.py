"""Launcher integration (mkdd.sh), with stand-ins for cmake and what it builds:
a disc is an image or an extracted folder, only the supported game is built,
and a failed change of disc keeps the previous playable build."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

SHA = "e96b8578451b9157e2b68fe5e918ebb572940c3ea54d6c8c7d45c24382bf12ae"

# cmake: configuring does nothing; building the disc tool or the game writes a stand-in for it.
# The stand-in qilin-disc calls anything with "other" in its path another game, and anything with
# "broken" in it unreadable.
CMAKE = r'''import os, sys
from pathlib import Path
if os.environ.get("FAIL_CMAKE"):
    sys.exit(1)
if "--build" in sys.argv:
    build = Path(sys.argv[2])
    target = sys.argv[sys.argv.index("--target") + 1]
    if target == "qilin_disc_tool":
        tool = build / "qilin-disc"
        tool.write_text("""#!/bin/sh
case "$2" in *broken*) echo "qilin-disc: $2: not a disc image" >&2; exit 1 ;; esac
id=GM4E01
case "$2" in *other*) id=GALE01 ;; esac
case "$1" in
    info) printf '{\\n  "format": "iso",\\n  "id": "%s",\\n  "revision": 0,\\n  "title": "A game",\\n  "dol_sha256": "SHA",\\n  "dol_size": 4\\n}\\n' "$id" ;;
    dol) printf 'dol of %s' "$2" > "$3" ;;
esac
""".replace("SHA", os.environ["TEST_SHA"]))
        tool.chmod(0o755)
    elif target == "mkdd":
        if not (build / "main.dol").exists():
            sys.exit(1)
        exe = build / "mkdd"
        exe.write_text('#!/bin/sh\nprintf "played:%s\\n" "$1"\n')
        exe.chmod(0o755)
'''


class LauncherTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="mkdd launcher ")
        self.root = root = Path(self.tmp.name)
        shutil.copyfile(Path(__file__).resolve().parents[2] / "mkdd.sh", root / "mkdd.sh")
        tools = root / "tools"
        tools.mkdir()
        cmake = tools / "cmake"
        cmake.write_text(f"#!{sys.executable}\n" + CMAKE)
        cmake.chmod(0o755)
        dpkg = tools / "dpkg"
        dpkg.write_text("#!/bin/sh\nexit 0\n")
        dpkg.chmod(0o755)
        self.env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"], CC="gcc-14", CXX="g++-14", TEST_SHA=SHA)
        self.record = root / "build-game" / "disc-folder.txt"

    def tearDown(self):
        self.tmp.cleanup()

    def folder(self, name):
        disc = self.root / name
        (disc / "sys").mkdir(parents=True)
        (disc / "files").mkdir()
        (disc / "sys" / "main.dol").touch()
        (disc / "sys" / "fst.bin").touch()
        return disc

    def run_launcher(self, *args, fail=False, sha=SHA):
        env = dict(self.env, TEST_SHA=sha)
        if fail:
            env["FAIL_CMAKE"] = "1"
        return subprocess.run(["sh", str(self.root / "mkdd.sh"), *map(str, args)], env=env, capture_output=True, text=True, timeout=30)

    def test_disc_change_is_committed_only_after_success(self):
        discs = [self.folder("old disc"), self.folder("new disc")]
        result = self.run_launcher("build", "--disc", discs[0])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.record.read_text(), str(discs[0]))
        self.assertEqual((self.root / "build-game" / "main.dol").read_text(), f"dol of {discs[0]}")
        result = self.run_launcher("build", "--disc", discs[1], fail=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.record.read_text(), str(discs[0]))
        result = self.run_launcher()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("played:" + str(discs[0]), result.stdout)
        result = self.run_launcher("build", "--disc", discs[1])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.record.read_text(), str(discs[1]))
        shutil.rmtree(discs[1] / "files")
        result = self.run_launcher()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no files folder", result.stderr)

    def test_an_image_is_used_as_it_is(self):
        image = self.root / "my games" / "double dash.ciso"
        image.parent.mkdir()
        image.write_bytes(b"CISO")
        result = self.run_launcher("--disc", image)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("played:" + str(image), result.stdout)
        self.assertEqual(self.record.read_text(), str(image))
        # The next start goes straight to the game, with the image.
        result = self.run_launcher()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("played:" + str(image), result.stdout)
        image.unlink()
        result = self.run_launcher()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("isn't there", result.stderr)

    def test_only_the_supported_game_is_built(self):
        good = self.folder("good disc")
        self.assertEqual(self.run_launcher("build", "--disc", good).returncode, 0)
        other = self.root / "other game.iso"
        other.write_bytes(b"x")
        result = self.run_launcher("build", "--disc", other)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("GALE01", result.stderr)
        self.assertIn("GM4E01", result.stderr)
        broken = self.root / "broken.iso"
        broken.write_bytes(b"x")
        result = self.run_launcher("build", "--disc", broken)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not a disc image", result.stderr)
        modified = self.root / "modified.iso"
        modified.write_bytes(b"x")
        result = self.run_launcher("build", "--disc", modified, sha="0" * 64)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("isn't the supported build's", result.stderr)
        # None of them replaced the disc that works, or its main.dol.
        self.assertEqual(self.record.read_text(), str(good))
        self.assertEqual((self.root / "build-game" / "main.dol").read_text(), f"dol of {good}")
        self.assertEqual(self.run_launcher("build").returncode, 0)

    def test_no_disc_says_what_to_give(self):
        result = self.run_launcher()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--disc", result.stderr)
        self.assertIn(".ciso", result.stderr)


if __name__ == "__main__":
    unittest.main()
