import importlib.util
import json
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
        launcher.UPDATE_CACHE = self.tmp / 'update-check.json'
        self.network = []
        launcher.fetch_latest_release = lambda timeout=4: self.network.append(1)  # tests never touch the network
        launcher.build_version = lambda: 'Alpha 1.7.0'
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

    def center(self, widget):
        x0, y0, x1, y1 = widget.box
        return (x0 + x1) / 2, (y0 + y1) / 2

    def click_at(self, x, y):
        class E:  # a left click at canvas coordinates, routed the way the canvas bindings do
            pass
        e = E()
        e.x, e.y = x, y
        self.win._on_motion(e)
        self.win._on_press(e)
        self.win._on_release(e)

    def leftovers(self, tags):
        return [i for t in tags for i in self.win.cv.find_withtag(t)]

    def test_closing_a_dialog_leaves_nothing_behind(self):
        """The OK button used to be redrawn by its own 'pop' animation after the dialog had closed."""
        self.pump(0.4)
        openers = (self.win.on_controls,
                   lambda: self.win.show_dialog('Sure?', [('Cancel', 'neutral', None), ('OK', 'primary', None)]))
        for opener in openers:
            opener()
            self.pump(0.5)
            tags = [b.tag for b in self.win.dialog['buttons']] + ['dlg']
            ok = self.win.dialog['buttons'][-1]
            self.click_at(*self.center(ok))  # the confirm animation is still running when the dialog closes
            self.pump(1.0)
            self.assertIsNone(self.win.dialog)
            self.assertEqual(self.leftovers(tags), [], 'items of the closed dialog are still on the canvas')

    def test_check_my_computer_dialog_closes_cleanly(self):
        self.pump(0.4)
        self.win.info('Check my computer', 'GPU: test')
        self.pump(0.5)
        tags = [b.tag for b in self.win.dialog['buttons']] + ['dlg']
        self.click_at(*self.center(self.win.dialog['buttons'][-1]))
        self.pump(1.0)
        self.assertEqual(self.leftovers(tags), [])

    def open_list(self, dropdown):
        self.click_at(*self.center(dropdown))
        self.pump(0.3)
        self.assertIsNotNone(self.win.popup)
        x0, y0, x1, y1 = self.win.popup['box']
        return x0, y0, x1, y1

    def test_a_dropdown_list_closes_on_a_click_outside(self):
        self.pump(0.4)
        for dropdown in (self.win.size, self.win.language):
            before = dropdown.value
            x0, y0, x1, y1 = self.open_list(dropdown)
            self.click_at(30, 30)  # outside the list (top-left corner of the window)
            self.assertIsNone(self.win.popup)
            self.assertEqual(self.leftovers(['popup']), [])
            self.assertEqual(dropdown.value, before)  # nothing was chosen

    def test_a_click_outside_does_not_press_what_is_under_it(self):
        self.pump(0.4)
        before = self.win.fullscreen.value
        self.open_list(self.win.size)
        self.click_at(*self.center(self.win.fullscreen))
        self.assertIsNone(self.win.popup)
        self.assertEqual(self.win.fullscreen.value, before)

    def test_a_dropdown_list_closes_on_escape_and_on_choosing(self):
        self.pump(0.4)
        self.open_list(self.win.language)
        self.win._on_escape()
        self.assertIsNone(self.win.popup)
        x0, y0, x1, y1 = self.open_list(self.win.language)
        p = self.win.popup
        target = 3 - p['scroll']  # the 4th language
        self.click_at((x0 + x1) / 2, y0 + p['pad'] + (target + 0.5) * p['rh'])
        self.assertIsNone(self.win.popup)
        self.assertEqual(self.win.language.value, launcher.LANGUAGES[3][0])
        self.assertEqual(self.win.settings['language'], launcher.LANGUAGES[3][1])

    def test_a_long_list_scrolls(self):
        self.pump(0.4)
        self.open_list(self.win.language)
        p = self.win.popup
        if p['rows'] < len(launcher.LANGUAGES):
            class E:
                num, delta = 5, -1
            first = p['scroll']
            self.win._on_wheel(E())
            self.assertEqual(self.win.popup['scroll'], min(first + 1, len(launcher.LANGUAGES) - p['rows']))
        self.win._popup_step(1)
        self.assertIsNotNone(self.win.popup['hover'])

    NEWER = {'version': 'Alpha 1.7.4', 'tag': 'v1.7.4', 'url': 'https://example.test/r', 'title': 'the clock is right again'}

    def test_the_new_build_chip_shows_only_for_a_newer_release(self):
        self.pump(0.4)
        self.assertEqual(self.win.update_btn.box, (0, 0, 0, 0))
        self.win._got_release(dict(self.NEWER))
        self.pump(0.4)
        self.assertGreater(self.win.update_btn.box[2], self.win.update_btn.box[0])
        self.assertIn('1.7.4', self.win.update_btn.label)
        self.assertIn('Alpha 1.7.4', self.win.note)
        self.win._got_release({'version': 'Alpha 1.7.0'})  # same as installed: nothing to announce
        self.pump(0.4)
        self.assertEqual(self.win.update_btn.box, (0, 0, 0, 0))
        self.assertEqual(self.win.cv.find_withtag('upd'), ())

    def test_the_update_dialog_explains_and_never_updates_by_itself(self):
        opened = []
        self.ui.webbrowser.open = opened.append
        self.pump(0.4)
        self.win._got_release(dict(self.NEWER))
        self.pump(0.3)
        self.win.on_update()
        self.pump(0.5)
        text = self.win.dialog['body'].get('1.0', 'end')
        for expected in ('Alpha 1.7.0', 'Alpha 1.7.4', 'git pull', './install.sh', 'never updates anything by itself'):
            self.assertIn(expected, text)
        self.assertEqual([b.label for b in self.win.dialog['buttons']], ['Later', 'Release notes'])
        self.click_at(*self.center(self.win.dialog['buttons'][-1]))
        self.pump(0.6)
        self.assertEqual(opened, ['https://example.test/r'])
        self.assertIsNone(self.win.dialog)

    def test_turning_the_check_off_hides_the_notice_and_stops_asking(self):
        self.pump(0.8)  # the look at GitHub that every start makes (stubbed) is over
        self.network.clear()
        self.win._got_release(dict(self.NEWER))
        self.win._toggle_updates(False)
        self.pump(0.4)
        self.assertIsNone(self.win.update_info)
        self.assertFalse(self.win.settings['check_updates'])
        self.assertNotIn('newer build', self.win.note)
        self.win._start_update_check()
        self.pump(0.5)
        self.assertEqual(self.network, [])  # nothing was fetched
        self.win._got_release(dict(self.NEWER))
        self.assertIsNone(self.win.update_info)

    def test_the_window_looks_at_github_once_on_start_when_the_cache_is_stale(self):
        self.pump(0.8)
        self.assertEqual(self.network, [1])

    def test_a_fresh_cache_means_no_look_at_github_on_start(self):
        self.pump(0.8)  # let the setUp window finish its own (stubbed) look first
        self.network.clear()
        launcher.UPDATE_CACHE.write_text(json.dumps({'checked': time.time(), 'release': dict(self.NEWER)}))
        win = self.ui.Window(launcher)
        try:
            end = time.time() + 0.8
            while time.time() < end:
                win.root.update()
                time.sleep(0.005)
            self.assertEqual(self.network, [])
            self.assertEqual(win.update_info['version'], 'Alpha 1.7.4')  # shown straight from the cache
        finally:
            win.root.destroy()

    def test_everything_fits_in_the_smallest_window(self):
        for geometry in ('760x600', '940x700', '1200x640'):
            self.win.root.geometry(geometry)
            self.pump(0.8)
            height = self.win.cv.winfo_height()
            for widget in self.win._widgets():
                if widget.box[2] > widget.box[0]:
                    self.assertLessEqual(widget.box[3], height, f'{widget.tag} falls off the {geometry} window')
            self.assertLessEqual(self.win.note_box[3], height, geometry)


if __name__ == '__main__':
    unittest.main()
