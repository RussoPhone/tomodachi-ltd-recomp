import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
spec = importlib.util.spec_from_file_location('launcher', SCRIPTS / 'launcher.py')
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)

try:
    import tkinter
    HAVE_DISPLAY = bool(os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY'))
except ImportError:
    HAVE_DISPLAY = False


@unittest.skipUnless(HAVE_DISPLAY, 'needs Tk and a display')
class LauncherWindowTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        launcher.ROOT = self.tmp
        launcher.SETTINGS = self.tmp / 'launcher.json'
        launcher.SAVE_DIR = self.tmp / 'user-nxrt'
        launcher.BACKUPS = self.tmp / 'backups'
        launcher.installed = lambda: True
        sys.path.insert(0, str(SCRIPTS))
        import launcher_ui
        self.ui = launcher_ui
        self.win = launcher_ui.Window(launcher)

    def tearDown(self):
        self.win.root.destroy()

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            self.win.root.update()
            time.sleep(0.005)

    def test_hovering_a_button_does_not_redraw_forever(self):
        """Redrawing the item under the pointer used to fire Leave/Enter again, looping at 100% CPU."""
        self.pump(0.6)
        x0, y0, x1, y1 = self.win.play_btn.box
        self.win.cv.event_generate('<Motion>', x=int((x0 + x1) / 2), y=int((y0 + y1) / 2), warp=True)
        self.pump(0.4)
        draws = []
        original = self.win.play_btn.draw
        self.win.play_btn.draw = lambda: (draws.append(1), original())
        self.pump(1.0)
        self.assertEqual(draws, [])
        self.assertAlmostEqual(self.win.play_btn.scale, self.ui.HOVER, places=2)

    def test_a_dialog_blocks_the_widgets_behind_it(self):
        self.pump(0.4)
        self.win.show_dialog('Sure?', [('Cancel', 'neutral', None), ('OK', 'primary', None)])
        self.pump(0.4)
        x0, y0, x1, y1 = self.win.play_btn.box
        self.assertIsNone(self.win._pick((x0 + x1) / 2, (y0 + y1) / 2))
        self.win.close_dialog()
        self.assertIs(self.win._pick((x0 + x1) / 2, (y0 + y1) / 2), self.win.play_btn)


if __name__ == '__main__':
    unittest.main()
