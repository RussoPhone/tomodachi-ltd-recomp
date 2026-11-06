import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import time
import subprocess
import zipfile
import unittest

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
spec = importlib.util.spec_from_file_location('launcher', SCRIPTS / 'launcher.py')
launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launcher)

subprocess_run = subprocess.run  # the real one, to restore after a test replaces it

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
        harmless = self.tmp / 'play-nxrt.sh'
        harmless.write_text('#!/bin/sh\nexit 0\n')
        harmless.chmod(0o755)
        launcher.PLAY = harmless
        launcher.UPDATE_CACHE = self.tmp / 'update-check.json'
        self.network = []
        launcher.fetch_latest_release = lambda timeout=4: self.network.append(1)  # tests never touch the network
        launcher.build_version = lambda: 'Alpha 1.7.0'
        sys.path.insert(0, str(SCRIPTS))
        import launcher_ui
        self.ui = launcher_ui
        self.win = self.new_window()

    def new_window(self):
        """A launcher window the user's real keyboard and mouse cannot reach: these tests run on the live desktop, and a
        stray Enter or a pointer passing over a button would press things (it did: a test window started the game)."""
        win = self.ui.Window(launcher)
        win.root.overrideredirect(True)      # unmanaged: the window manager gives it no focus
        win.root.geometry('+6000+6000')      # and it sits outside every screen
        return win

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
        self.win.cv.event_generate('<Motion>', x=int((x0 + x1) / 2), y=int((y0 + y1) / 2))
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
        win = self.new_window()
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

    def pump_window(self, win, seconds):
        end = time.time() + seconds
        while time.time() < end:
            win.root.update()
            time.sleep(0.005)

    def canvas_texts(self, win):
        return [win.cv.itemcget(i, 'text') for i in win.cv.find_all() if win.cv.type(i) == 'text']

    def test_without_the_wordmark_files_the_header_is_the_plain_text_title(self):
        self.pump(0.5)
        self.assertIn('Tomodachi-LTD-Recomp', self.canvas_texts(self.win))
        self.assertEqual([i for i in self.win.cv.find_all() if self.win.cv.type(i) == 'image'], [])

    def test_the_header_shows_the_wordmark_in_place_of_the_title_text(self):
        shutil.copytree(Path(__file__).resolve().parents[1] / 'assets/wordmark', self.tmp / 'assets/wordmark')
        win = self.new_window()
        try:
            self.pump_window(win, 0.6)
            images = [i for i in win.cv.find_all() if win.cv.type(i) == 'image']
            self.assertEqual(len(images), 1)
            self.assertNotIn('Tomodachi-LTD-Recomp', self.canvas_texts(win))  # the image replaced the text
            self.assertIn('Alpha 1.7.0', self.canvas_texts(win))               # the build chip is still there
            self.assertIn('Unofficial PC Port · Bring your own copy!', self.canvas_texts(win))
            # the closest pre-scaled file is used (Tk cannot scale smoothly)
            self.assertEqual(win._logo(131).height(), 128)
            self.assertEqual(win._logo(10).height(), 96)
            self.assertEqual(win._logo(999).height(), 192)
        finally:
            win.root.destroy()

    def test_a_broken_wordmark_file_falls_back_to_the_text_title(self):
        (self.tmp / 'assets/wordmark').mkdir(parents=True)
        (self.tmp / 'assets/wordmark/wordmark-h120.png').write_bytes(b'not a png')
        win = self.new_window()
        try:
            self.pump_window(win, 0.6)
            self.assertIn('Tomodachi-LTD-Recomp', self.canvas_texts(win))
        finally:
            win.root.destroy()

    # --- saves ----------------------------------------------------------------------------------------
    def write_save(self, slug, content=b'x', mtime=None):
        folder = launcher.profile_dir(slug)
        folder.mkdir(parents=True, exist_ok=True)
        f = folder / 'game.sav'
        f.write_bytes(content)
        if mtime:
            os.utime(f, (mtime, mtime))
        return f

    def add_backup(self, name, kind, created, content=b'old', label=None, profile='main'):
        launcher.BACKUPS.mkdir(parents=True, exist_ok=True)
        path = launcher.BACKUPS / name
        with zipfile.ZipFile(path, 'w') as z:
            z.writestr('game.sav', content)
        info = {'kind': kind, 'profile': profile, 'created': created}
        if label:
            info['label'] = label
        launcher._remember(name, **info)
        return path

    def fresh_window(self):
        win = self.new_window()
        self.addCleanup(win.root.destroy)
        self.pump_window(win, 0.4)
        return win

    def press(self, widget):
        self.click_at(*self.center(widget))
        self.pump(0.4)

    def test_it_opens_on_the_save_the_game_wrote_to_last(self):
        other = launcher.create_profile('Practice island')
        self.write_save('main', mtime=1000)
        self.write_save(other, mtime=2000)
        win = self.fresh_window()
        self.assertEqual(win.profile, other)
        self.assertEqual(win.profile_dd.value, 'Practice island')

    def test_pressing_play_makes_that_save_the_one_the_launcher_opens_on_next_time(self):
        other = launcher.create_profile('Practice island')
        self.write_save('main', mtime=time.time())       # main is the one with the newest files
        self.win._refresh_saves()
        self.win.profile_dd.pick('Practice island')
        self.ui.subprocess.run = lambda cmd, **kw: type('R', (), {'returncode': 0})()
        self.addCleanup(setattr, self.ui.subprocess, 'run', subprocess_run)
        self.win.on_play()
        self.pump(1.0)
        self.assertEqual(launcher.most_recent_profile(), other)
        self.assertEqual(self.fresh_window().profile, other)

    def test_the_recent_saves_are_on_the_main_screen_newest_first(self):
        now = time.time()
        self.add_backup('a.zip', 'auto', now - 7200)
        self.add_backup('b.zip', 'point', now - 600)
        self.add_backup('c.zip', 'save', now - 86400 * 3, label='Before the festival')
        self.add_backup('d.zip', 'point', now - 3600)
        self.win._refresh_saves()
        self.pump(0.5)
        titles = [self.win.L.save_title(e) for e in self.win.rows_data]
        self.assertEqual(titles, ['Save point', 'Save point', 'Before playing'])  # the 3 newest
        self.assertEqual([r.title for r in self.win.save_rows], titles)
        self.assertEqual(self.win.save_rows[0].when, '10 min ago')

    def test_the_saves_card_shows_an_empty_state_not_blank_rows(self):
        self.pump(0.5)
        self.assertEqual(self.win.save_rows[0].title, 'No saves yet')
        self.assertFalse(self.win.save_rows[0].enabled)
        self.assertEqual(self.win.save_rows[1].box, (0, 0, 0, 0))

    def test_clicking_a_recent_save_asks_and_restores_it_keeping_a_way_back(self):
        self.write_save('main', b'now')
        self.add_backup('old.zip', 'point', time.time() - 900, content=b'before')
        self.win._refresh_saves()
        self.pump(0.5)
        self.press(self.win.save_rows[0])
        self.assertIsNotNone(self.win.dialog)
        self.assertIn('Save point', self.win.dialog['message'])
        self.press(self.win.dialog['buttons'][-1])        # Restore
        self.pump(0.5)
        self.assertEqual((launcher.SAVE_DIR / 'game.sav').read_bytes(), b'before')
        self.assertIn('safety', [e['kind'] for e in launcher.list_saves('main')])  # "Before restore" exists

    def test_cancel_leaves_the_save_alone(self):
        self.write_save('main', b'now')
        self.add_backup('old.zip', 'point', time.time() - 900, content=b'before')
        self.win._refresh_saves()
        self.pump(0.5)
        self.press(self.win.save_rows[0])
        self.press(self.win.dialog['buttons'][0])         # Cancel
        self.assertEqual((launcher.SAVE_DIR / 'game.sav').read_bytes(), b'now')

    def type_and_confirm(self, text):
        entry = self.win.dialog['body']
        entry.delete(0, 'end')
        entry.insert(0, text)
        self.press(self.win.dialog['buttons'][-1])        # OK

    def test_a_new_save_is_a_separate_fresh_game_and_play_uses_its_folder(self):
        self.write_save('main', b'my island')
        self.win.profile_dd.pick(self.win.NEW_SAVE)
        self.pump(0.5)
        self.assertIn('Name for the new save', self.win.dialog['message'])
        self.type_and_confirm('Second island')
        self.pump(0.4)
        self.assertEqual(self.win.profile_dd.value, 'Second island')
        self.assertEqual(self.win._profile_name(), 'Second island')
        self.assertEqual((launcher.SAVE_DIR / 'game.sav').read_bytes(), b'my island')  # the other save is untouched
        calls = []

        class Done:
            returncode = 0
        self.addCleanup(setattr, self.ui.subprocess, 'run', self.ui.subprocess.run)
        self.ui.subprocess.run = lambda cmd, **kw: (calls.append((cmd, kw)), Done())[1]
        self.win.on_play()
        deadline = time.time() + 5
        while time.time() < deadline and not calls:
            self.pump(0.1)
        self.pump(0.5)
        cmd, kw = calls[0]
        self.assertEqual(cmd[cmd.index('--user') + 1], str(launcher.profile_dir(self.win.profile)))
        self.assertIn('NXRT_EXIT_FILE', kw['env'])

    def test_a_duplicate_save_name_is_explained_not_created(self):
        self.win.profile_dd.pick(self.win.NEW_SAVE)
        self.pump(0.5)
        self.type_and_confirm('main')
        self.pump(0.5)
        self.assertIn('already a save called', self.win.dialog['message'])
        self.assertEqual(list(launcher.load_profiles()), ['main'])

    def test_choosing_another_save_in_the_list_switches_to_it(self):
        other = launcher.create_profile('Practice')
        self.win._refresh_saves()
        self.win.profile_dd.pick('Practice')
        self.assertEqual(self.win.profile, other)
        self.win.profile_dd.pick('Main')
        self.assertEqual(self.win.profile, 'main')

    def test_play_makes_a_backup_first_and_tells_what_happened_when_it_ends(self):
        self.write_save('main', b'island')
        self.addCleanup(setattr, self.ui.subprocess, 'run', self.ui.subprocess.run)
        self.ui.subprocess.run = lambda cmd, **kw: type('R', (), {'returncode': 0})()
        self.win.on_play()
        self.pump(1.0)
        self.assertIn('auto', [e['kind'] for e in launcher.list_saves('main')])   # "Before playing"
        self.assertFalse(self.win.playing)
        self.assertIsNone(self.win.watch)

    def test_closing_the_game_tells_when_the_last_save_was(self):
        self.write_save('main', b'island')
        self.win.watch = launcher.SaveWatch('main', now=time.time() - 3600)
        self.win.watch.last_write = time.time() - 47 * 60
        self.win.playing = True
        self.win._played(0)
        self.assertIn('47 min', self.win.note)
        self.assertIn('lost', self.win.note)

    def test_a_kill_during_a_write_offers_to_go_back_to_the_previous_save(self):
        self.write_save('main', b'island')
        self.add_backup('prev.zip', 'auto', time.time() - 600, content=b'good')
        self.win._refresh_saves()
        self.win.watch = launcher.SaveWatch('main', now=time.time() - 60)
        self.write_save('main', b'half-written')
        self.win.watch.poll(time.time() - 1)
        self.win.playing = True
        self.win._played(137)
        self.pump(0.5)
        self.assertIsNotNone(self.win.dialog)
        self.assertIn('writing the save', self.win.dialog['message'])
        self.assertEqual([b.label for b in self.win.dialog['buttons']], ['Keep it', 'Go back'])

    def test_all_saves_lists_everything_and_can_rename_and_delete(self):
        now = time.time()
        first = self.add_backup('a.zip', 'point', now - 600)
        self.add_backup('b.zip', 'auto', now - 7200)
        self.win._refresh_saves()
        self.win.on_all_saves()
        self.pump(0.5)
        lb = self.win.dialog['body']
        self.assertEqual(lb.size(), 2)
        self.assertEqual(lb.curselection(), (0,))
        buttons = {b.label: b for b in self.win.dialog['buttons']}
        self.press(buttons['Rename'])
        self.type_and_confirm('Before the festival')
        self.pump(0.6)
        self.assertIn('Before the festival', [launcher.save_title(e) for e in launcher.list_saves('main')])
        self.assertIsNotNone(self.win.dialog)               # back on the list, now showing the new name
        buttons = {b.label: b for b in self.win.dialog['buttons']}
        self.press(buttons['Delete'])
        self.press(self.win.dialog['buttons'][-1])          # confirm
        self.pump(0.6)
        self.assertFalse(first.exists())

    def test_all_saves_without_backups_explains_when_they_appear(self):
        self.win.on_all_saves()
        self.pump(0.4)
        self.assertIn('no backups yet', self.win.dialog['message'])

    def test_back_up_now_asks_for_a_name_and_files_it_under_the_current_save(self):
        self.write_save('main', b'island')
        self.win.on_backup()
        self.pump(0.5)
        self.type_and_confirm('After the first flower')
        self.pump(0.5)
        entries = launcher.list_saves('main')
        self.assertEqual([launcher.save_title(e) for e in entries], ['After the first flower'])
        self.assertEqual(entries[0]['kind'], 'save')

    def test_back_up_now_with_nothing_to_back_up_says_so(self):
        self.win.on_backup()
        self.pump(0.3)
        self.assertIsNone(self.win.dialog)
        self.assertIn('no save to back up', self.win.note)


if __name__ == '__main__':
    unittest.main()
