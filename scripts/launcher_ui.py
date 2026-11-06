"""Window for scripts/launcher.py, drawn in the Island UI Kit style (https://russophone.github.io/island-ui-kit/).

Soft pills with a darker side and a soft shadow, cream surfaces with doodles, red primary buttons with wavy
stripes, cloud dialogs, and the game's motion: focus in 2 frames (scale 1.08), confirm as squash-and-pop
(1.1 -> 1.05 -> 1.2 -> rest over 12 frames), switches and tabs settle with a small overshoot.
Everything is drawn on one Tk Canvas; the logic (settings, play, backups, report) stays in launcher.py.
"""
import math
import os
import queue
import random
import subprocess
import sys
import tempfile
import threading
import tkinter as tk
import tkinter.font as tkfont
import webbrowser
import zipfile
from pathlib import Path

C = {
    "ink": "#00040D", "paper": "#FFFFFF", "edge": "#C8C8C8",
    "cream": "#FFD97F", "cream_l": "#FFE49B", "cream_p": "#FFEFD7",
    "red": "#FF2400", "red_stripe": "#FF0700", "red_side": "#CD2B00", "red_alt": "#FF3700",
    "focus": "#FFC700", "orange": "#FF8800", "orange_side": "#E06A00", "tab": "#FFB330", "pict": "#FA7407",
    "off_face": "#E6E6E6", "off_fg": "#A7A7A7", "dim": "#3B2A0A", "note": "#7A6A55", "soft_side": "#E9D2A6",
}
FONT_FAMILIES = ("M PLUS Rounded 1c", "Nunito", "Zen Maru Gothic", "Varela Round", "Quicksand",
                 "Noto Sans", "DejaVu Sans")
FRAME_MS = 16                                       # one layout-animation frame at 60/s
DECIDE = ((0, 1.1), (3, 1.05), (6, 1.2), (12, 1.0))  # confirm: frame -> scale (last value = rest)
HOVER = 1.08
BASE_W, BASE_H = 940, 840

CONTROLS = (  # (tab, columns, rows, note); a row is (what, [key caps])
    ("Keyboard", 2, (
        ("Left stick", ["I", "K", "J", "L"]), ("Right stick", ["Q", "E"]), ("D-pad", ["Arrow keys"]),
        ("A", ["X"]), ("B", ["Z"]), ("X", ["S"]), ("Y", ["A"]),
        ("L / R", ["W", "R"]), ("ZL / ZR", ["1", "3"]),
        ("+", ["Enter"]), ("\u2212", ["Backspace"]), ("Close the game", ["Home"]), ("Fullscreen", ["F11"])),
     "Left stick: I up, K down, J left, L right. Remapping is not available yet."),
    ("Mouse", 1, (
        ("Move the island camera", ["Window edge", "Middle drag"]), ("Zoom", ["Wheel"]),
        ("Touch screen / A", ["Left click"]), ("B", ["Right click"]),
        ("Mouse as analog stick", ["F8"]), ("Sensitivity", ["F6", "F7"])),
     "Experimental: great on the island, awkward in some menus."),
    ("Controller", 1, (
        ("A  B  X  Y", ["Right", "Bottom", "Top", "Left"]), ("L / R, ZL / ZR", ["Bumpers", "Triggers"]),
        ("+ / \u2212", ["Start", "Back"]), ("Sticks and D-pad", ["Same on the pad"]),
        ("Close the game", ["Guide"])),
     "Nintendo layout: the right face button is A. Full table: docs/CONTROLS.md"),
)


def register_bundled_fonts(root):
    """Let fontconfig see assets/fonts (an optional OFL font) for this process only; call before Tk starts."""
    fonts = root / "assets" / "fonts"
    if not fonts.is_dir() or sys.platform.startswith("win") or "FONTCONFIG_FILE" in os.environ:
        return
    conf = Path(tempfile.gettempdir()) / f"tomodachi-launcher-fonts-{os.getuid()}.conf"
    try:
        conf.write_text('<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "fonts.dtd"><fontconfig>'
                        '<include ignore_missing="yes">/etc/fonts/fonts.conf</include>'
                        f"<dir>{fonts}</dir></fontconfig>")
        os.environ["FONTCONFIG_FILE"] = str(conf)
    except OSError:
        pass


# --- drawing helpers ------------------------------------------------------------------
def mix(a, b, t):
    a, b = a.lstrip("#"), b.lstrip("#")
    pa = [int(a[i:i + 2], 16) for i in (0, 2, 4)]
    pb = [int(b[i:i + 2], 16) for i in (0, 2, 4)]
    return "#%02x%02x%02x" % tuple(round(x + (y - x) * t) for x, y in zip(pa, pb))


def shadow_on(bg):
    return mix(bg, "#000000", 0.196)  # the game's #00000032 shadow, pre-mixed (Tk has no alpha)


def blob(x0, y0, x1, y1, r, wobble=0.0, seed=0, n=8):
    """Rounded-rect outline points for a smooth polygon; wobble gives the hand-drawn edge."""
    r = max(1.0, min(r, (x1 - x0) / 2, (y1 - y0) / 2))
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    pts, k = [], 0
    for ccx, ccy, a0 in ((x1 - r, y0 + r, -90), (x1 - r, y1 - r, 0), (x0 + r, y1 - r, 90), (x0 + r, y0 + r, 180)):
        for i in range(n + 1):
            a = math.radians(a0 + 90 * i / n)
            px, py = ccx + r * math.cos(a), ccy + r * math.sin(a)
            if wobble:
                d = wobble * math.sin(seed * 1.7 + k * 0.9) * (0.6 + 0.4 * math.sin(seed + k * 0.37))
                nx, ny = px - cx, py - cy
                ln = math.hypot(nx, ny) or 1
                px, py = px + nx / ln * d, py + ny / ln * d
            pts += (px, py)
            k += 1
    return pts


def stadium_span(x, x0, y0, x1, y1):
    r = (y1 - y0) / 2
    cy = (y0 + y1) / 2
    if x < x0 + r:
        dx = x0 + r - x
    elif x > x1 - r:
        dx = x - (x1 - r)
    else:
        return y0, y1
    if dx >= r:
        return None
    dy = math.sqrt(r * r - dx * dx)
    return cy - dy, cy + dy


def wave_bands(x0, y0, x1, y1):
    """Wavy horizontal stripes clipped to a pill (the primary button pattern)."""
    h = y1 - y0
    if h < 4:
        return []
    period, amp, length, thick = h * 0.3, h * 0.035, h * 0.62, h * 0.085
    inset = h * 0.04
    out, y = [], y0 + period * 0.55
    while y < y1:
        tops, bots, x = [], [], x0
        while x <= x1:
            span = stadium_span(x, x0 + inset, y0 + inset, x1 - inset, y1 - inset)
            w = y + amp * math.sin(2 * math.pi * x / length)
            if span:
                t, b = max(w, span[0]), min(w + thick, span[1])
                if t < b:
                    tops.append((x, t))
                    bots.append((x, b))
                elif tops:
                    break
            x += 3
        if len(tops) > 1:
            out.append([c for p in tops for c in p] + [c for p in reversed(bots) for c in p])
        y += period
    return out


def ease_back(t):
    s, t = 1.9, t - 1
    return t * t * ((s + 1) * t + s) + 1


# --- widgets --------------------------------------------------------------------------
class Widget:
    """A group of canvas items under one tag, redrawn on state change.

    Pointer events are NOT bound to the items: redrawing the item under the pointer makes Tk fire
    Leave/Enter again, which restarts the hover animation and loops forever. Window routes motion and
    clicks by hit-testing `box`, so it does not matter how often an item is deleted and recreated.
    """

    def __init__(self, ui, key):
        self.ui, self.cv, self.tag = ui, ui.cv, key
        self.box, self.under, self.anim, self.disposed = (0, 0, 0, 0), C["paper"], None, False

    def blocked(self):
        return self.ui.modal_blocks(self)

    def interactive(self):
        return not self.blocked()

    def hit(self, x, y):
        x0, y0, x1, y1 = self.box
        return x1 > x0 and x0 <= x <= x1 and y0 <= y <= y1

    def enter(self):
        pass

    def leave(self):
        pass

    def press(self):
        pass

    def click(self, x, y):
        pass

    def dispose(self):
        """Stop animating and never draw again (a widget that was removed must not come back from a pending frame)."""
        self.disposed = True
        if self.anim:
            self.cv.after_cancel(self.anim)
            self.anim = None
        self.box = (0, 0, 0, 0)
        self.cv.delete(self.tag)

    def place(self, x0, y0, x1, y1, under=None):
        if self.disposed:
            return
        self.box = (x0, y0, x1, y1)
        if under:
            self.under = under
        self.draw()

    def run(self, steps, frame_fn, done=None):
        if self.disposed:
            return
        if self.anim:
            self.cv.after_cancel(self.anim)

        def tick(i=0):
            if self.disposed:
                return
            frame_fn(i / steps if steps else 1.0)
            if i < steps:
                self.anim = self.cv.after(FRAME_MS, tick, i + 1)
            else:
                self.anim = None
                if done:
                    done()
        tick()


class Pill(Widget):
    STYLES = {"primary": ("red", "red_side", "paper", True), "neutral": ("paper", "edge", "ink", False),
              "soft": ("cream_p", "soft_side", "ink", False),  # neutral button on a white card
              "orange": ("orange", "orange_side", "paper", False),
              "focus": ("focus", "orange_side", "ink", False),
              "tab_on": ("tab", "orange_side", "ink", False), "tab_off": ("cream_l", "soft_side", "ink", False)}  # the yellow "new build" chip

    def __init__(self, ui, key, label, style="primary", command=None):
        super().__init__(ui, key)
        self.label, self.style, self.command = label, style, command
        self.enabled, self.hover, self.scale, self.lift = True, False, 1.0, 0.0

    def interactive(self):
        return self.enabled and not self.blocked()

    def set_enabled(self, on):
        if on != self.enabled:
            self.enabled = on
            if not on:
                self.scale, self.lift = 1.0, 0.0
            self.draw()

    def set_label(self, label):
        if label != self.label:
            self.label = label
            self.draw()

    def draw(self):
        cv = self.cv
        cv.delete(self.tag)
        x0, y0, x1, y1 = self.box
        if x1 <= x0:
            return
        s = self.scale
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2 - self.lift
        w, h = (x1 - x0) * s, (y1 - y0) * s
        bx0, by0, bx1, by1 = cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2
        face, side, fg, stripes = self.STYLES[self.style]
        face, side, fg = C[face], C[side], C[fg]
        if not self.enabled:
            face, side, fg, stripes = C["off_face"], C["edge"], C["off_fg"], False
        seed = sum(map(ord, self.tag))
        for dy, col in ((h * 0.18, shadow_on(self.under)), (h * 0.09, side), (0, face)):
            cv.create_polygon(blob(bx0, by0 + dy, bx1, by1 + dy, h / 2, wobble=h * 0.025, seed=seed),
                              smooth=True, fill=col, outline="", tags=self.tag)
        if stripes:
            for poly in wave_bands(bx0, by0, bx1, by1):
                cv.create_polygon(poly, fill=C["red_stripe"], outline="", tags=self.tag)
        self._label(cx, cy, w, h, fg)

    def _label(self, cx, cy, w, h, fg):
        self.cv.create_text(cx, cy, text=self.label, fill=fg,
                            font=self.ui.fit_font(self.label, h * 0.36, w - h * 0.6), tags=self.tag)

    def animate_to(self, scale, lift, frames=2):
        s0, l0 = self.scale, self.lift

        def f(t):
            self.scale, self.lift = s0 + (scale - s0) * t, l0 + (lift - l0) * t
            self.draw()
        self.run(frames, f)

    def enter(self):
        self.hover = True
        if self.interactive():
            self.animate_to(HOVER, 2)

    def leave(self):
        self.hover = False
        if self.enabled:
            self.animate_to(1.0, 0)

    def press(self):
        if self.interactive():
            self.animate_to(1.04, 0, frames=1)

    def click(self, x, y):
        if not self.interactive():
            return
        self.decide()
        if self.command:
            self.cv.after(FRAME_MS * 4, self.command)

    def decide(self):
        keys = list(DECIDE[:-1]) + [(DECIDE[-1][0], HOVER if self.hover else 1.0)]
        end = keys[-1][0]

        def f(t):
            fr = t * end
            for (fa, sa), (fb, sb) in zip(keys, keys[1:]):
                if fa <= fr <= fb:
                    self.scale = sa + (sb - sa) * ((fr - fa) / (fb - fa) if fb > fa else 1)
                    break
            self.draw()
        self.run(end, f)


class Dropdown(Pill):
    """Neutral pill showing the current value; opens a cream menu of choices."""

    def __init__(self, ui, key, choices, value, on_change):
        self.choices, self.value, self.on_change = choices, value, on_change
        super().__init__(ui, key, self._text(value), "soft", self.open)

    def _text(self, value):
        return f"{value}  ▾"

    def open(self):
        self.ui.open_popup(self)

    def pick(self, choice):
        self.value = choice
        self.set_label(self._text(choice))
        self.on_change(choice)


class SaveRow(Pill):
    """One recent save: its name on the left, when it was made on the right. Click it to restore it."""

    def __init__(self, ui, key, command):
        super().__init__(ui, key, "", "soft", command)
        self.title, self.when = "", ""

    def set_text(self, title, when, enabled=True):
        self.title, self.when = title, when
        self.set_enabled(enabled)
        self.draw()

    def _label(self, cx, cy, w, h, fg):
        left, right = cx - w / 2 + h * 0.5, cx + w / 2 - h * 0.5
        when = self.when
        wf = self.ui.font(h * 0.34, "normal")
        self.cv.create_text(right, cy, text=when, anchor="e", fill=C["note"] if self.enabled else C["off_fg"], font=wf,
                            tags=self.tag)
        room = max(20, right - left - wf.measure(when) - h * 0.3)
        self.cv.create_text(left, cy, text=self.title, anchor="w", fill=fg,
                            font=self.ui.fit_font(self.title, h * 0.4, room), tags=self.tag)


class Switch(Widget):
    def __init__(self, ui, key, value, on_change):
        super().__init__(ui, key)
        self.value, self.on_change, self.pos = value, on_change, 1.0 if value else 0.0

    def draw(self):
        cv = self.cv
        cv.delete(self.tag)
        x0, y0, x1, y1 = self.box
        h = y1 - y0
        track = mix(C["edge"], C["red"], max(0.0, min(1.0, self.pos)))
        cv.create_polygon(blob(x0, y0, x1, y1, h / 2), smooth=True, fill=track, outline="", tags=self.tag)
        pad = h * 0.11
        d = h - 2 * pad
        kx = x0 + pad + (x1 - x0 - 2 * pad - d) * self.pos
        cv.create_oval(kx, y0 + pad + h * 0.07, kx + d, y0 + pad + d + h * 0.07, fill=shadow_on(track),
                       outline="", tags=self.tag)
        cv.create_oval(kx, y0 + pad, kx + d, y0 + pad + d, fill=C["paper"], outline="", tags=self.tag)

    def click(self, x, y):
        self.toggle()

    def toggle(self):
        if self.blocked():
            return
        self.value = not self.value
        p0, p1 = self.pos, 1.0 if self.value else 0.0

        def f(t):
            self.pos = p0 + (p1 - p0) * ease_back(t)
            self.draw()
        self.run(10, f)
        self.on_change(self.value)


# --- window ---------------------------------------------------------------------------
class Window:
    def __init__(self, L):
        self.L = L  # the launcher module (settings, play_command, backups, report)
        register_bundled_fonts(L.ROOT)
        self.root = root = tk.Tk()
        self.version = L.build_version()  # read from the installed binary, never from the changelog
        root.title("Tomodachi-LTD-Recomp" + (f" · {self.version}" if self.version else ""))
        root.geometry(f"{BASE_W}x{min(BASE_H, max(600, root.winfo_screenheight() - 90))}")
        root.minsize(760, 600)
        self.settings = L.load_settings()
        self.playing = False
        self.dialog = None
        self.note = ""
        self._fonts = {}
        self.u = 1.0
        self.hh = 140.0  # header height, set properly by every relayout
        fams = set(tkfont.families(root))
        self.family = next((f for f in FONT_FAMILIES if f in fams), "TkDefaultFont")
        self.cv = tk.Canvas(root, highlightthickness=0, bd=0, bg=C["cream"])
        self.cv.pack(fill="both", expand=True)

        s = self.settings
        lang_name = next((n for n, c in L.LANGUAGES if c == s["language"]), L.LANGUAGES[0][0])
        sizes = list(L.SIZES) if s["size"] in L.SIZES else list(L.SIZES) + [s["size"]]
        self.play_btn = Pill(self, "play", "Play", "primary", self.on_play)
        self.fullscreen = Switch(self, "fullscreen", s["fullscreen"], lambda v: self._set(fullscreen=v))
        self.safe = Switch(self, "safe", s.get("safe", False), lambda v: self._set(safe=v))
        self.updates = Switch(self, "updates", s.get("check_updates", True), self._toggle_updates)
        self.update_btn = Pill(self, "upd", "", "focus", self.on_update)
        self.update_info = None  # the release that is newer than the installed build, when there is one
        self.size = Dropdown(self, "size", sizes, s["size"], lambda v: self._set(size=v))
        self.language = Dropdown(self, "language", [n for n, _ in L.LANGUAGES], lang_name,
                                 lambda v: self._set(language=next(c for n, c in L.LANGUAGES if n == v)))
        # saves: the launcher opens on the one the game wrote to last; "+ New save" starts a separate fresh game
        self.profiles = L.load_profiles()
        self.profile = L.most_recent_profile()
        self.rows_data, self._last_activity, self.watch = [], 0.0, None
        self._game_events = queue.Queue()  # the game runs in a thread; it reports back through this, never into Tk
        self.profile_dd = Dropdown(self, "profile", [], self.profiles[self.profile]["name"], self._on_profile_pick)
        self.save_rows = [SaveRow(self, f"sv{i}", lambda i=i: self.on_restore_row(i)) for i in range(3)]
        self.save_btns = [Pill(self, "bk", "Back up now", "soft", self.on_backup),
                          Pill(self, "as", "All saves…", "soft", self.on_all_saves)]
        self.folder_btn = Pill(self, "fd", "Folder", "soft", lambda: L.open_folder(L.profile_dir(self.profile)))
        self.help_btns = [Pill(self, "hc", "Controls", "soft", self.on_controls),
                          Pill(self, "hk", "Check my computer", "soft", self.on_check),
                          Pill(self, "hr", "Bug report", "soft", self.on_report),
                          Pill(self, "hl", "Open logs", "soft", lambda: L.open_folder(L.LOGS)),
                          Pill(self, "ht", "Testing guide", "soft", lambda: webbrowser.open(L.TESTING_URL)),
                          Pill(self, "hp", "Report a problem", "orange", lambda: webbrowser.open(L.ISSUES_URL))]
        self._job = None
        self.popup = None     # open dropdown list (drawn on the canvas, so a click outside can dismiss it)
        self.hovered = None   # widget under the pointer
        self.pressed = None   # widget the left button went down on
        self.cv.bind("<Configure>", lambda e: self._schedule())
        self.cv.bind("<Motion>", self._on_motion)
        self.cv.bind("<Leave>", lambda e: self._set_hover(None))
        self.cv.bind("<ButtonPress-1>", self._on_press)
        self.cv.bind("<ButtonRelease-1>", self._on_release)
        for seq in ("<MouseWheel>", "<Button-4>", "<Button-5>"):
            root.bind(seq, self._on_wheel)
        root.bind("<Up>", lambda e: self._popup_step(-1))
        root.bind("<Down>", lambda e: self._popup_step(1))
        root.bind("<Return>", lambda e: self._on_return())
        root.bind("<Escape>", lambda e: self._on_escape())
        if L.LANGUAGE_RESET:  # a language from an older launcher that the game has no text for
            self._set(language=self.settings["language"])
            self.set_note(f'The game has no "{L.LANGUAGE_RESET}" text, so the language is back to English (US).')
        else:
            self.set_note("Your save is backed up automatically before each play session.")
        self._refresh_saves()
        self._start_update_check()

    # --- new-build notice ------------------------------------------------------------
    def _toggle_updates(self, on):
        self._set(check_updates=on)
        if on:
            self._start_update_check()
        else:
            had = self.update_info is not None
            self.update_info = None
            if had:
                self.set_note("The check for new builds is off.")
            self._schedule()

    def _start_update_check(self):
        """Show what is cached at once; look at GitHub off the UI thread when the cache is stale."""
        if not self.settings.get("check_updates", True) or not self.version:
            return
        cached, fresh = self.L.cached_release()
        if cached:
            self._got_release(cached)
        if fresh:
            return
        box = {}

        def look():
            box["release"] = self.L.check_for_update()
        thread = threading.Thread(target=look, daemon=True)
        thread.start()

        def poll():
            if thread.is_alive():
                self.root.after(300, poll)
            elif box.get("release"):
                self._got_release(box["release"])
        self.root.after(300, poll)

    def _got_release(self, release):
        if not self.settings.get("check_updates", True) or not self.L.newer_release(self.version, release):
            if self.update_info:
                self.update_info = None
                self._schedule()
            return
        first = self.update_info is None or self.update_info.get("version") != release.get("version")
        self.update_info = release
        if first:
            self.set_note(f'A newer build is available: {release["version"]} (this one is {self.version}). '
                          'Click the yellow chip to see how to update.')
        self._schedule()

    def on_update(self):
        info = self.update_info
        if not info:
            return
        text = (f'Installed:  {self.version}\nAvailable:  {info["version"]}\n'
                + (f'\nWhat is new:  {info["title"]}\n' if info.get("title") else '')
                + '\nTo update: open a terminal in this folder, run  git pull  and then  ./install.sh  again.\n'
                  'It redoes only the steps that changed, but updating can still mean a long rebuild.\n'
                  'Your saves stay where they are (and a backup is made each time you press Play).\n\n'
                  'The launcher never updates anything by itself.')
        buttons = [('Later', 'neutral', None)]
        if info.get("url"):
            buttons.append(('Release notes', 'primary', lambda: webbrowser.open(info["url"])))
        self.show_dialog('A newer build is available', buttons, body=text)

    # --- pointer routing (hit-test on boxes, so redrawing items cannot re-trigger hover) --------
    def _widgets(self):
        if self.dialog:
            return list(self.dialog["buttons"]) + self.dialog["tabs"]
        return [self.fullscreen, self.safe, self.updates, self.size, self.language, self.play_btn, self.update_btn,
                self.profile_dd, self.folder_btn] + self.save_rows + self.save_btns + self.help_btns

    def _pick(self, x, y):
        if self.popup:
            return None
        for wd in reversed(self._widgets()):
            if wd.interactive() and wd.hit(x, y):
                return wd
        return None

    def _set_hover(self, wd):
        if wd is self.hovered:
            return
        old, self.hovered = self.hovered, wd
        if old is not None:
            old.leave()
        if wd is not None:
            wd.enter()
        self.cv.config(cursor="hand2" if wd is not None else "")

    def _on_motion(self, e):
        if self.popup:
            idx = self._popup_row(e.x, e.y)
            self.cv.config(cursor="hand2" if idx is not None else "")
            if idx != self.popup["hover"]:
                self.popup["hover"] = idx
                self._draw_popup()
            return
        self._set_hover(self._pick(e.x, e.y))

    def _on_press(self, e):
        if self.popup:
            if self._popup_row(e.x, e.y) is None:
                self.close_popup()  # a click anywhere outside the list dismisses it (and does nothing else)
                return
            self.popup["down"] = self._popup_row(e.x, e.y)
            return
        self.pressed = self._pick(e.x, e.y)
        if self.pressed is not None:
            self.pressed.press()

    def _on_release(self, e):
        if self.popup:
            idx, self.popup["down"] = self._popup_row(e.x, e.y), self.popup.get("down")
            if idx is not None and idx == self.popup["down"]:
                self._popup_choose(idx)
            return
        wd, self.pressed = self.pressed, None
        if wd is not None and wd.interactive() and wd.hit(e.x, e.y):
            wd.click(e.x, e.y)

    # --- dropdown list (drawn on the canvas) -------------------------------------------
    def open_popup(self, owner):
        self.close_popup()
        self._set_hover(None)
        u, cv = self.u, self.cv
        W, H = cv.winfo_width(), cv.winfo_height()
        rh, pad = 40 * u, 10 * u
        f = self.font(16 * u)
        width = max(owner.box[2] - owner.box[0], max(f.measure(c) for c in owner.choices) + 84 * u)
        width = min(width, W - 24 * u)
        x1 = min(owner.box[2], W - 12 * u)
        x0 = max(12 * u, x1 - width)
        below = H - owner.box[3] - 16 * u
        above = owner.box[1] - 16 * u
        room = below if below >= min(above, len(owner.choices) * rh + 2 * pad) or below >= above else above
        rows = max(2, min(len(owner.choices), int((room - 2 * pad) // rh)))
        height = rows * rh + 2 * pad
        y0 = owner.box[3] + 8 * u if room is below else owner.box[1] - 8 * u - height
        cur = owner.choices.index(owner.value) if owner.value in owner.choices else 0
        scroll = max(0, min(cur - rows // 2, len(owner.choices) - rows))
        self.popup = {"owner": owner, "box": (x0, y0, x0 + width, y0 + height), "rows": rows, "rh": rh, "pad": pad,
                      "scroll": scroll, "hover": cur, "down": None, "font": f}
        self._draw_popup()

    def close_popup(self):
        if self.popup:
            self.popup = None
            self.cv.delete("popup")
            self.cv.config(cursor="")

    def _popup_row(self, x, y):
        p = self.popup
        x0, y0, x1, y1 = p["box"]
        if not (x0 <= x <= x1 and y0 + p["pad"] <= y < y0 + p["pad"] + p["rows"] * p["rh"]):
            return None
        return p["scroll"] + int((y - y0 - p["pad"]) // p["rh"])

    def _popup_choose(self, idx):
        owner = self.popup["owner"]
        choice = owner.choices[idx]
        self.close_popup()
        owner.pick(choice)

    def _popup_step(self, d):
        p = self.popup
        if not p:
            return
        n = len(p["owner"].choices)
        p["hover"] = max(0, min(n - 1, (p["hover"] if p["hover"] is not None else -1) + d))
        p["scroll"] = max(min(p["scroll"], p["hover"]), p["hover"] - p["rows"] + 1)
        self._draw_popup()

    def _on_wheel(self, e):
        p = self.popup
        if not p:
            return
        up = e.num == 4 or getattr(e, "delta", 0) > 0
        n = len(p["owner"].choices)
        p["scroll"] = max(0, min(n - p["rows"], p["scroll"] + (-1 if up else 1)))
        p["hover"] = self._popup_row(self.root.winfo_pointerx() - self.cv.winfo_rootx(),
                                     self.root.winfo_pointery() - self.cv.winfo_rooty())
        self._draw_popup()

    def _draw_popup(self):
        p, cv, u = self.popup, self.cv, self.u
        cv.delete("popup")
        x0, y0, x1, y1 = p["box"]
        for dy, col, grow in ((10 * u, mix(C["cream"], "#000000", 0.3), 0), (0, C["cream_l"], 4 * u), (0, C["paper"], 0)):
            cv.create_polygon(blob(x0 - grow, y0 + dy - grow, x1 + grow, y1 + dy + grow, 26 * u, wobble=1.2 * u, seed=5),
                              smooth=True, fill=col, outline="", tags="popup")
        owner = p["owner"]
        for i in range(p["rows"]):
            idx = p["scroll"] + i
            if idx >= len(owner.choices):
                break
            ry = y0 + p["pad"] + i * p["rh"]
            if idx == p["hover"]:
                cv.create_polygon(blob(x0 + 8 * u, ry + 2 * u, x1 - 8 * u, ry + p["rh"] - 2 * u, 16 * u), smooth=True,
                                  fill=C["focus"], outline="", tags="popup")
            cv.create_text(x0 + 24 * u, ry + p["rh"] / 2, text=owner.choices[idx], anchor="w", fill=C["ink"],
                           font=p["font"], tags="popup")
            if owner.choices[idx] == owner.value:
                cv.create_text(x1 - 24 * u, ry + p["rh"] / 2, text="✓", anchor="e", fill=C["red"],
                               font=self.font(18 * u), tags="popup")
        n = len(owner.choices)
        if n > p["rows"]:  # scroll thumb
            track = p["rows"] * p["rh"] - 8 * u
            th = max(24 * u, track * p["rows"] / n)
            ty = y0 + p["pad"] + 4 * u + (track - th) * (p["scroll"] / (n - p["rows"]))
            cv.create_polygon(blob(x1 - 9 * u, ty, x1 - 4 * u, ty + th, 2.5 * u), smooth=True, fill=C["edge"], outline="",
                              tags="popup")
        cv.tag_raise("popup")

    def _on_escape(self):
        if self.popup:
            self.close_popup()
        else:
            self.close_dialog()

    # --- wordmark ---------------------------------------------------------------------
    def _logo(self, target_h):
        """The wordmark PNG whose height is closest to target_h (pre-scaled files: Tk only scales by whole numbers).
        None when assets/wordmark is missing or unreadable, and the header falls back to the text title."""
        if not hasattr(self, "_logo_files"):
            self._logos, self._logo_files = {}, {}
            for f in (self.L.ROOT / "assets/wordmark").glob("wordmark-h*.png"):
                try:
                    self._logo_files[int(f.stem.rsplit("h", 1)[1])] = f
                except ValueError:
                    pass
        if not self._logo_files:
            return None
        h = min(self._logo_files, key=lambda k: abs(k - target_h))
        if h not in self._logos:
            try:
                self._logos[h] = tk.PhotoImage(master=self.root, file=str(self._logo_files[h]))
            except tk.TclError:
                self._logos[h] = None
        return self._logos[h]

    # --- fonts ----------------------------------------------------------------------
    def font(self, px, weight="bold"):
        key = (max(8, round(px)), weight)
        if key not in self._fonts:
            self._fonts[key] = tkfont.Font(self.root, family=self.family, size=-key[0], weight=weight)
        return self._fonts[key]

    def fit_font(self, text, px, width, weight="bold"):
        f = self.font(px, weight)
        while px > 9 and f.measure(text) > width:
            px -= 1
            f = self.font(px, weight)
        return f

    # --- settings and state ---------------------------------------------------------
    def _set(self, **kw):
        self.settings.update(kw)
        try:
            self.L.save_settings(self.settings)
        except OSError:
            pass

    def set_note(self, text):
        self.note = text
        self.cv.delete("note")
        if hasattr(self, "note_box"):
            self._draw_note()

    def status(self):
        if not self.L.installed():
            return "Not installed", C["red_alt"], C["paper"], C["paper"]
        if self.playing:
            return "Playing", C["focus"], C["ink"], C["red"]
        return "Ready", C["paper"], C["ink"], C["pict"]

    def refresh(self):
        self.play_btn.set_enabled(self.L.installed() and not self.playing)
        self.play_btn.set_label("Playing…" if self.playing else "Play")
        self.cv.delete("status")
        self._draw_status()
        if not self.L.installed():
            self.set_note("The native port is not installed yet. Run ./install.sh first.")

    # --- layout ---------------------------------------------------------------------
    def _schedule(self):
        """Redraw in the same turn as the resize events (coalesced), so no stale frame shows while dragging."""
        if self._job:
            self.root.after_cancel(self._job)
        self._job = self.root.after_idle(self.relayout)

    def relayout(self):
        self._job = None
        cv = self.cv
        W, H = cv.winfo_width(), cv.winfo_height()
        if W < 50:
            return
        self.close_popup()
        raw = max(0.78, min(1.6, min(W / BASE_W, H / BASE_H)))
        u = self.u = round(raw * 25) / 25  # steps of 0.04 keep the font cache small while dragging
        design_h = 836 * u  # height the content needs at this scale
        slack = max(0.0, H - design_h)
        vg = min(slack / 6, 34 * u)  # extra space between the rows; the rest goes below the header
        top_extra = min((slack - 3 * vg) / 2, 60 * u) if slack > 3 * vg else 0.0
        cv.delete("bg", "card", "label", "status", "note")
        self._doodles(W, H, u)
        pad, gap = 26 * u, 22 * u

        # header
        self.hh = hh = 140 * u
        for dy, col in ((7 * u, C["orange_side"]), (0, C["orange"])):
            pts, x = [0, 0, W, 0], W
            while x >= -10:
                pts += (x, hh + dy + 6 * u * math.sin(x / (64 * u)))
                x -= 12
            cv.create_polygon(pts, fill=col, outline="", smooth=True, tags="bg")
        logo = self._logo(118 * u)
        if logo:  # the wordmark on a cream sticker (its red-orange letters vanish on the orange band); chips beside it
            m = 12 * u
            sx0, sy0 = pad - m, 6 * u
            sx1, sy1 = pad + logo.width() + m, sy0 + logo.height() + 2 * m
            for dy, col in ((8 * u, shadow_on(C["orange"])), (0, C["cream_l"])):
                cv.create_polygon(blob(sx0, sy0 + dy, sx1, sy1 + dy, 34 * u, wobble=2.0 * u, seed=9), smooth=True,
                                  fill=col, outline="", tags="label")
            cv.create_image(pad, sy0 + m, image=logo, anchor="nw", tags="label")
            tx = sx1 + 24 * u
            row, sub_x, sub_y, sub_px = hh * 0.36, tx, hh * 0.64, 17
        else:  # no image shipped: the plain-text title, as before
            t = cv.create_text(pad, hh * 0.40, text="Tomodachi-LTD-Recomp", anchor="w", fill=C["paper"],
                               font=self.font(32 * u), tags="label")
            tx, row, sub_x, sub_y, sub_px = cv.bbox(t)[2] + 14 * u, hh * 0.40, pad + 2, hh * 0.76, 14
        if self.version:  # build chip
            bx = tx
            f = self.font(14 * u)
            bw, bh2 = f.measure(self.version) + 24 * u, 28 * u
            cv.create_polygon(blob(bx, row - bh2 / 2 + 4 * u, bx + bw, row + bh2 / 2 + 4 * u, bh2 / 2),
                              smooth=True, fill=C["orange_side"], outline="", tags="label")
            cv.create_polygon(blob(bx, row - bh2 / 2, bx + bw, row + bh2 / 2, bh2 / 2), smooth=True,
                              fill=C["cream_l"], outline="", tags="label")
            cv.create_text(bx + bw / 2, row, text=self.version, fill=C["ink"], font=f, tags="label")
            if self.update_info:  # a newer published build: yellow chip, click for how to update
                label = f'{self.update_info["version"]} available'
                uw = f.measure(label) + 40 * u
                ux = bx + bw + 10 * u
                self.update_btn.set_label(label)
                self.update_btn.place(ux, row - 18 * u, ux + uw, row + 18 * u, C["orange"])
            else:
                self.update_btn.box = (0, 0, 0, 0)
                cv.delete(self.update_btn.tag)
        else:
            self.update_btn.box = (0, 0, 0, 0)
            cv.delete(self.update_btn.tag)
        cv.create_text(sub_x, sub_y, text="Unofficial PC Port · Bring your own copy!", anchor="w", fill=C["cream_l"],
                       font=self.font(sub_px * u), tags="label")

        # row 1: play | settings
        top = hh + 32 * u + top_extra
        mid = W * 0.47
        h1 = 312 * u
        self._card(pad, top, mid - gap / 2, top + h1, "Play")
        self._card(mid + gap / 2, top, W - pad, top + h1, "Settings")
        ix0, ix1 = pad + 26 * u, mid - gap / 2 - 26 * u
        self.play_btn.place(ix0, top + 66 * u, ix1, top + 170 * u, C["paper"])
        sy = top + 208 * u
        cv.create_text(ix0, sy, text="Save", anchor="w", fill=C["ink"], font=self.font(17 * u), tags="label")
        ddw = min(ix1 - ix0 - 64 * u, 260 * u)
        self.profile_dd.place(ix1 - ddw, sy - 19 * u, ix1, sy + 19 * u, C["paper"])
        last = self._last_activity
        cv.create_text(ix0, top + 238 * u, anchor="w", fill=C["note"], font=self.font(12.5 * u, "normal"), tags="label",
                       text=(f"Last saved {self.L.when_text(last)}" if last else "Nothing saved here yet: the game starts fresh."))
        self._hint(ix0, top + 268 * u, "↵", "Play", u)
        cv.create_text(ix0, top + 296 * u, text="First face-paint opening takes ~40 s (it builds a cache).",
                       anchor="w", fill=C["note"], font=self.fit_font("x" * 52, 12.5 * u, ix1 - ix0, "normal"),
                       tags="label")

        sx0, sx1 = mid + gap / 2 + 26 * u, W - pad - 26 * u
        rows = [top + 70 * u + i * 42 * u for i in range(5)]
        lab = self.font(17 * u)
        for y, text in zip(rows, ("Fullscreen", "Safe mode", "Window size", "Language", "Check for new builds")):
            cv.create_text(sx0, y, text=text, anchor="w", fill=C["ink"], font=lab, tags="label")
        cv.create_text(sx0 + lab.measure("Safe mode") + 10 * u, rows[1], text="if the picture looks wrong",
                       anchor="w", fill=C["note"], font=self.font(12.5 * u, "normal"), tags="label")
        sw_w, sw_h = 66 * u, 36 * u
        self.fullscreen.place(sx1 - sw_w, rows[0] - sw_h / 2, sx1, rows[0] + sw_h / 2, C["paper"])
        self.safe.place(sx1 - sw_w, rows[1] - sw_h / 2, sx1, rows[1] + sw_h / 2, C["paper"])
        dd_h = 38 * u
        self.size.place(sx1 - 170 * u, rows[2] - dd_h / 2, sx1, rows[2] + dd_h / 2, C["paper"])
        self.language.place(sx1 - min(250 * u, sx1 - sx0 - 130 * u), rows[3] - dd_h / 2, sx1, rows[3] + dd_h / 2,
                            C["paper"])
        self.updates.place(sx1 - sw_w, rows[4] - sw_h / 2, sx1, rows[4] + sw_h / 2, C["paper"])

        # row 2: saves | help
        top2 = top + h1 + gap + 8 * u + vg
        h2 = 232 * u
        split = W * 0.36
        self._card(pad, top2, split - gap / 2, top2 + h2, "Saves")
        self._card(split + gap / 2, top2, W - pad, top2 + h2, "Help")
        bx0, bx1 = pad + 22 * u, split - gap / 2 - 22 * u
        self.folder_btn.place(bx1 - 84 * u, top2 + 11 * u, bx1, top2 + 43 * u, C["paper"])
        row_h, ry = 34 * u, top2 + 54 * u
        for i, row in enumerate(self.save_rows):
            if i < len(self.rows_data):
                e = self.rows_data[i]
                row.set_text(self.L.save_title(e), self.L.when_text(e["created"]))
            elif i == 0:
                row.set_text("No saves yet", "", False)
            else:
                row.box = (0, 0, 0, 0)
                cv.delete(row.tag)
                continue
            row.place(bx0, ry + i * (row_h + 6 * u), bx1, ry + i * (row_h + 6 * u) + row_h, C["paper"])
        by = top2 + 54 * u + 3 * (row_h + 6 * u) + 4 * u
        bh = 40 * u
        bw = (bx1 - bx0 - 10 * u) / 2
        self.save_btns[0].place(bx0, by, bx0 + bw, by + bh)
        self.save_btns[1].place(bx0 + bw + 10 * u, by, bx1, by + bh)
        hx0, hx1 = split + gap / 2 + 22 * u, W - pad - 22 * u
        cols = 3 if hx1 - hx0 > 480 * u else 2
        hb = 46 * u if cols == 3 else 36 * u
        hw = (hx1 - hx0 - (cols - 1) * 10 * u) / cols
        rows_n = -(-len(self.help_btns) // cols)
        hgap = 16 * u if cols == 3 else 9 * u
        hy0 = top2 + 24 * u + (h2 - 24 * u - (rows_n * hb + (rows_n - 1) * hgap)) / 2
        for i, b in enumerate(self.help_btns):
            cx, r_ = i % cols, i // cols
            x = hx0 + cx * (hw + 10 * u)
            y = hy0 + r_ * (hb + hgap)
            b.place(x, y, x + hw, y + hb)

        # note balloon
        ny = top2 + h2 + gap + 10 * u + vg
        self.note_box = (pad, ny, W - pad, min(H - 16 * u, ny + 54 * u))
        self._draw_note()
        for t in ("play", "upd", "profile", "fd", "sv0", "sv1", "sv2", "fullscreen", "safe", "updates", "size", "language", "bk", "as", "hc", "hk", "hr", "hl", "ht", "hp"):
            cv.tag_raise(t)
        self.refresh()
        if self.dialog:
            self._draw_dialog(1.0)

    def _doodles(self, W, H, u):
        rnd = random.Random(7)
        col, w, step = C["cream_l"], max(4, 8 * u), 150 * u
        y = 0
        while y < H + step:
            x = (y // step % 2) * step / 2
            while x < W + step:
                px, py, k = x + rnd.uniform(-30, 30) * u, y + rnd.uniform(-30, 30) * u, rnd.randrange(5)
                s = 22 * u
                if k == 0:
                    self.cv.create_line(px - s, py, px - s / 2, py - s / 2, px, py, px + s / 2, py + s / 2, px + s, py,
                                        smooth=True, width=w, fill=col, capstyle="round", tags="bg")
                elif k == 1:
                    self.cv.create_oval(px - s * 0.6, py - s * 0.6, px + s * 0.6, py + s * 0.6, width=w, outline=col,
                                        tags="bg")
                elif k == 2:
                    self.cv.create_polygon(px - s * 0.7, py + s * 0.5, px + s * 0.1, py - s * 0.7, px + s * 0.7,
                                           py + s * 0.6, fill="", outline=col, width=w, joinstyle="round", tags="bg")
                elif k == 3:
                    self.cv.create_line(px - s, py + s * 0.3, px, py - s * 0.4, px + s * 0.6, py + s * 0.6,
                                        smooth=True, width=w, fill=col, capstyle="round", tags="bg")
                else:
                    self.cv.create_line(px - s * 0.4, py, px + s * 0.4, py, width=w * 1.6, fill=col, capstyle="round",
                                        tags="bg")
                x += step
            y += step

    def _card(self, x0, y0, x1, y1, title):
        u = self.u
        for dy, col in ((14 * u, shadow_on(C["cream"])), (7 * u, C["edge"]), (0, C["paper"])):
            self.cv.create_polygon(blob(x0, y0 + dy, x1, y1 + dy, 32 * u, wobble=2.2 * u, seed=int(x0 + y0)),
                                   smooth=True, fill=col, outline="", tags="card")
        self.cv.create_oval(x0 + 24 * u, y0 + 22 * u, x0 + 36 * u, y0 + 34 * u, fill=C["pict"], outline="", tags="card")
        self.cv.create_text(x0 + 44 * u, y0 + 28 * u, text=title, anchor="w", fill=C["pict"], font=self.font(17 * u),
                            tags="card")

    def _hint(self, x, y, glyph, label, u):
        f = self.font(12 * u)
        gw = max(28 * u, f.measure(glyph) + 14 * u)
        self.cv.create_polygon(blob(x, y - 14 * u, x + gw, y + 14 * u, 14 * u), smooth=True, fill=C["ink"], outline="",
                               tags="label")
        self.cv.create_text(x + gw / 2, y, text=glyph, fill=C["paper"], font=f, tags="label")
        self.cv.create_text(x + gw + 8 * u, y, text=label, anchor="w", fill=C["ink"], font=self.font(15 * u), tags="label")

    def _draw_status(self):
        u, W = self.u, self.cv.winfo_width()
        label, face, fg, dot = self.status()
        f = self.font(16 * u)
        right, cy, h = W - 26 * u, self.hh * 0.36, 38 * u
        x0 = right - f.measure(label) - 42 * u
        for dy, col in ((5 * u, C["orange_side"]), (0, face)):
            self.cv.create_polygon(blob(x0, cy - h / 2 + dy, right, cy + h / 2 + dy, h / 2), smooth=True, fill=col,
                                   outline="", tags="status")
        self.cv.create_oval(x0 + 14 * u, cy - 5 * u, x0 + 24 * u, cy + 5 * u, fill=dot, outline="", tags="status")
        self.cv.create_text(x0 + 31 * u, cy, text=label, anchor="w", fill=fg, font=f, tags="status")

    def _draw_note(self):
        """The message line, as a speech balloon at the bottom."""
        u = self.u
        x0, y0, x1, y1 = self.note_box
        if y1 - y0 < 24:
            return
        self.cv.create_polygon(blob(x0, y0 + 6 * u, x1, y1 + 6 * u, (y1 - y0) / 2), smooth=True,
                               fill=shadow_on(C["cream"]), outline="", tags="note")
        self.cv.create_polygon(blob(x0, y0, x1, y1, (y1 - y0) / 2, wobble=1.5 * u, seed=11), smooth=True,
                               fill=C["cream_p"], outline="", tags="note")
        cy = (y0 + y1) / 2
        r = 13 * u
        self.cv.create_oval(x0 + 18 * u, cy - r, x0 + 18 * u + 2 * r, cy + r, fill=C["pict"], outline="", tags="note")
        self.cv.create_text(x0 + 18 * u + r, cy, text="i", fill=C["paper"], font=self.font(15 * u), tags="note")
        self.cv.create_text(x0 + 30 * u + 2 * r, cy, text=self.note, anchor="w", fill=C["ink"],
                            width=x1 - x0 - 60 * u - 2 * r, font=self.font(14.5 * u), tags="note")

    # --- dialogs (cloud) --------------------------------------------------------------
    def modal_blocks(self, widget):
        return self.dialog is not None and widget not in self.dialog["buttons"] + self.dialog["tabs"]

    def show_dialog(self, message, buttons, body=None, tabs=None, collect=None, compact=False):
        """buttons: [(label, style, callback or None)]; the last one answers Return.
        body: text, or a function that builds a widget on the canvas (a list, an entry). collect(): reads that widget
        before the dialog closes; when given, every callback is called with its result."""
        self.close_popup()
        if self.dialog:
            self.close_dialog()
        pills = []
        for i, (label, style, cb) in enumerate(buttons):
            def act(cb=cb):
                payload = collect() if collect and self.dialog else None
                self.close_dialog()
                if cb:
                    cb(payload) if collect else cb()
            pills.append(Pill(self, f"dlg_b{i}", label, style, act))
        text = None
        if callable(body):
            text = body(self.cv)
        elif body:
            text = tk.Text(self.cv, wrap="none", bd=0, highlightthickness=0, bg=C["cream_p"], fg=C["ink"],
                           padx=12, pady=10, cursor="arrow", font=self.font(13 * self.u, "normal"))
            text.insert("1.0", body)
            text.config(state="disabled")
        self._set_hover(None)
        self.dialog = {"message": message, "buttons": pills, "body": text, "anim": None, "tabs": [], "tab": 0,
                       "compact": compact}
        if tabs:  # the controls sheet: one tab per CONTROLS entry
            def pick(i):
                self.dialog["tab"] = i
                self._draw_dialog(1.0)
            self.dialog["tabs"] = [Pill(self, f"dlg_t{i}", name, "tab_off", lambda i=i: pick(i))
                                   for i, name in enumerate(tabs)]
        self._pop_dialog(6)  # dialog in: 6 frames

    def _pop_dialog(self, steps):
        d = self.dialog

        def tick(i=0):
            if self.dialog is not d:
                return
            self._draw_dialog(0.6 + 0.4 * ease_back(i / steps))
            d["anim"] = self.root.after(FRAME_MS, tick, i + 1) if i < steps else None
        tick()

    def _draw_dialog(self, s):
        cv, u, d = self.cv, self.u, self.dialog
        cv.delete("dlg")
        W, H = cv.winfo_width(), cv.winfo_height()
        cv.create_rectangle(0, 0, W, H, fill=C["dim"], stipple="gray50", outline="", tags="dlg")
        sheet = bool(d["tabs"])
        compact = bool(d.get("compact"))
        big = (d["body"] is not None and not compact) or sheet
        cw = min((820 if big else 620) * u, W - 40) * s
        ch = min((640 if sheet else 520 if big else 340) * u, H - 40) * s
        cx, cy = W / 2, H / 2
        x0, y0, x1, y1 = cx - cw / 2, cy - ch / 2, cx + cw / 2, cy + ch / 2
        r = ch * (0.12 if sheet else 0.22 if big else 0.45)
        wob = ch * (0.015 if big else 0.03)
        cv.create_polygon(blob(x0, y0 + 16 * u, x1, y1 + 16 * u, r, wobble=wob, seed=3), smooth=True,
                          fill=mix(C["dim"], "#000000", 0.35), outline="", tags="dlg")
        cv.create_polygon(blob(x0, y0, x1, y1, r, wobble=wob, seed=3), smooth=True, fill=C["cream"],
                          outline="", tags="dlg")
        ty = y0 + (ch * 0.075 if sheet else ch * (0.12 if big else 0.27 if compact else 0.36))
        cv.create_text(cx, ty, text=d["message"], width=cw * 0.78, justify="center", fill=C["ink"],
                       font=self.font((22 if big else 24) * u * s), tags="dlg")
        bh = 60 * u * s
        n = len(d["buttons"])
        bw = min(200 * u * s, (cw * 0.8 - (n - 1) * 16 * u) / n)
        total = n * bw + (n - 1) * 16 * u
        by = y1 - ch * (0.12 if big else 0.2) - bh
        for i, b in enumerate(d["buttons"]):
            bx = cx - total / 2 + i * (bw + 16 * u)
            b.place(bx, by, bx + bw, by + bh, C["cream"])
            cv.tag_raise(b.tag)
        if sheet:
            self._draw_sheet(x0, y0, x1, y1, cw, by, s)
        elif compact and s >= 0.99:
            cv.create_window(cx - cw * 0.34, y0 + ch * 0.43, anchor="nw", window=d["body"], width=cw * 0.68,
                             height=46 * u, tags="dlg")
        elif big and s >= 0.99:
            tx0, ty0 = x0 + cw * 0.07, ty + 30 * u
            cv.create_window(tx0, ty0, anchor="nw", window=d["body"], width=cw * 0.86,
                             height=max(60, by - ty0 - 24 * u), tags="dlg")

    def _draw_sheet(self, x0, y0, x1, y1, cw, by, s):
        """Controls sheet: tab pills, cream rows with white key caps (Island UI Kit rows and chips)."""
        cv, u, d = self.cv, self.u, self.dialog
        n = len(d["tabs"])
        tw, th, gap = min(190 * u * s, cw * 0.27), 44 * u * s, 10 * u * s
        tx = (x0 + x1) / 2 - (n * tw + (n - 1) * gap) / 2
        ty = y0 + 84 * u * s
        for i, t in enumerate(d["tabs"]):
            t.style = "tab_on" if i == d["tab"] else "tab_off"
            t.place(tx + i * (tw + gap), ty, tx + i * (tw + gap) + tw, ty + th, C["cream"])
            cv.tag_raise(t.tag)
        if s < 0.99:
            return
        _, cols, rows, note = CONTROLS[d["tab"]]
        top = ty + th + 22 * u
        left, right = x0 + cw * 0.06, x1 - cw * 0.06
        cgap = 14 * u
        colw = (right - left - (cols - 1) * cgap) / cols
        per = -(-len(rows) // cols)
        avail = by - 36 * u - top - (26 * u if note else 0)
        rh = min(46 * u, (avail + 6 * u) / per - 6 * u)
        for k, (what, keys) in enumerate(rows):
            c, r = divmod(k, per)
            rx0 = left + c * (colw + cgap)
            ry0 = top + r * (rh + 6 * u)
            cv.create_polygon(blob(rx0, ry0 + rh * 0.12, rx0 + colw, ry0 + rh * 1.12, rh / 2),
                              smooth=True, fill=shadow_on(C["cream"]), outline="", tags="dlg")
            cv.create_polygon(blob(rx0, ry0, rx0 + colw, ry0 + rh, rh / 2), smooth=True,
                              fill=C["cream_p"], outline="", tags="dlg")
            ch_ = rh * 0.68
            widths = [max(ch_, self.font(ch_ * 0.5).measure(kc) + ch_ * 0.7) for kc in keys]
            kx = rx0 + colw - rh * 0.3 - sum(widths) - (len(keys) - 1) * 6 * u
            for kc, kw in zip(keys, widths):
                ky = ry0 + (rh - ch_) / 2 - 1
                cv.create_polygon(blob(kx, ky + ch_ * 0.14, kx + kw, ky + ch_ * 1.14, ch_ * 0.32), smooth=True,
                                  fill=C["edge"], outline="", tags="dlg")
                cv.create_polygon(blob(kx, ky, kx + kw, ky + ch_, ch_ * 0.32), smooth=True,
                                  fill=C["paper"], outline="", tags="dlg")
                cv.create_text(kx + kw / 2, ky + ch_ / 2, text=kc, fill=C["ink"],
                               font=self.font(ch_ * 0.5), tags="dlg")
                kx += kw + 6 * u
            avail_w = colw - (rh * 0.3 * 2 + sum(widths) + len(keys) * 6 * u)
            cv.create_text(rx0 + rh * 0.4, ry0 + rh / 2, text=what, anchor="w", fill=C["ink"],
                           font=self.fit_font(what, rh * 0.38, max(40, avail_w)), tags="dlg")
        if note:
            cv.create_text((x0 + x1) / 2, by - 22 * u, text=note, fill=C["note"],
                           font=self.fit_font(note, 14 * u, cw * 0.84, "normal"), tags="dlg")
        for b in d["buttons"] + d["tabs"]:
            cv.tag_raise(b.tag)

    def close_dialog(self):
        if not self.dialog:
            return
        if self.dialog.get("anim"):
            self.root.after_cancel(self.dialog["anim"])
        for b in self.dialog["buttons"] + self.dialog["tabs"]:
            b.dispose()
        if self.dialog["body"] is not None:
            self.dialog["body"].destroy()
        self.cv.delete("dlg")
        self.dialog = None
        self.hovered = None
        self.cv.config(cursor="")

    def _on_return(self):
        if self.popup:
            if self.popup["hover"] is not None:
                self._popup_choose(self.popup["hover"])
            return
        if self.dialog:
            self.dialog["buttons"][-1].command()
        elif self.play_btn.enabled:
            self.play_btn.decide()
            self.on_play()

    def info(self, title, text):
        self.show_dialog(title, [("OK", "primary", None)], body=text)

    # --- actions (logic lives in launcher.py) ---------------------------------------
    # --- saves ----------------------------------------------------------------------------
    NEW_SAVE, RENAME_SAVE = "+ New save…", "Rename this save…"

    def _profile_name(self, slug=None):
        return self.profiles.get(slug or self.profile, {}).get("name", "Main")

    def _refresh_saves(self):
        """Re-read the saves from disk (after playing, restoring, creating...) and redraw."""
        L = self.L
        self.profiles = L.load_profiles()
        if self.profile not in self.profiles:
            self.profile = L.MAIN
        self.rows_data = L.list_saves(self.profile)[:len(self.save_rows)]
        self._last_activity = L.last_activity(self.profile)
        dd = self.profile_dd
        dd.choices = [p["name"] for p in self.profiles.values()] + [self.NEW_SAVE, self.RENAME_SAVE]
        dd.value = self._profile_name()
        dd.set_label(dd._text(dd.value))
        self._schedule()

    def _on_profile_pick(self, choice):
        if choice == self.NEW_SAVE:
            self._refresh_saves()  # the list showed an action, not a save: put the real name back
            self.ask_text("Name for the new save", f"Game {len(self.profiles) + 1}", self._create_profile)
        elif choice == self.RENAME_SAVE:
            self._refresh_saves()
            self.ask_text("Rename this save", self._profile_name(), self._rename_profile)
        else:
            self.profile = next((k for k, p in self.profiles.items() if p["name"] == choice), self.profile)
            self._refresh_saves()
            self.set_note(f'Playing from "{choice}".' + ("" if self._last_activity else " It is empty: the game starts fresh."))

    def _create_profile(self, text):
        try:
            self.profile = self.L.create_profile(text)
        except ValueError as error:
            self.show_dialog(str(error), [("OK", "primary", None)])
            return
        self._refresh_saves()
        self.set_note(f'New save "{self._profile_name()}" is ready: Play starts a fresh game in it. Your other saves are untouched.')

    def _rename_profile(self, text):
        try:
            self.L.rename_profile(self.profile, text)
        except ValueError as error:
            self.show_dialog(str(error), [("OK", "primary", None)])
            return
        self._refresh_saves()

    def ask_text(self, title, initial, on_ok):
        """A small dialog with one line of text; on_ok(text) when confirmed with OK or Return."""
        box = {}

        def make(parent):
            e = tk.Entry(parent, bd=0, highlightthickness=0, bg=C["cream_p"], fg=C["ink"], insertbackground=C["ink"],
                         justify="center", font=self.font(18 * self.u, "bold"))
            e.insert(0, initial)
            e.select_range(0, "end")
            box["entry"] = e
            self.root.after(150, e.focus_set)
            return e
        self.show_dialog(title, [("Cancel", "neutral", None), ("OK", "primary", on_ok)], body=make,
                         collect=lambda: box["entry"].get(), compact=True)

    def on_backup(self):
        L = self.L
        if not L.save_files(self.profile):
            self.set_note("There is no save to back up yet.")
            return

        def go(label):
            out = L.backup_save("save", self.profile, " ".join(label.split())[:50] or None)
            self._refresh_saves()
            self.set_note(f'Backed up "{self._profile_name()}"' + (f' as "{" ".join(label.split())[:50]}".' if label.strip() else "."))
        self.ask_text("Name this backup (or leave it empty)", "", go)

    def on_restore_row(self, i):
        if i < len(self.rows_data):
            self._confirm_restore(self.rows_data[i])

    def _confirm_restore(self, entry):
        L = self.L
        name = self._profile_name(entry["profile"])

        def go():
            try:
                L.restore_save(entry["path"], entry["profile"])
            except (OSError, ValueError, zipfile.BadZipFile) as error:
                self.show_dialog(f"Restore failed.\n{error}", [("OK", "primary", None)])
                return
            self.profile = entry["profile"]
            self._refresh_saves()
            self.set_note(f'"{name}" is back to {L.when_text(entry["created"])}. What it was before is kept as "Before restore".')
        self.show_dialog(f'Go back to "{L.save_title(entry)}" ({L.when_text(entry["created"])})?\n'
                         f'"{name}" is backed up first, so you can undo this.',
                         [("Cancel", "neutral", None), ("Restore", "primary", go)])

    def on_all_saves(self):
        L = self.L
        entries = L.list_saves(self.profile)
        if not entries:
            self.show_dialog(f'"{self._profile_name()}" has no backups yet.\nThey are made before you play, while you '
                             'play (each time the game saves) and when you press Back up now.', [("OK", "primary", None)])
            return
        box = {}

        def make(parent):
            lb = tk.Listbox(parent, activestyle="none", bd=0, highlightthickness=0, relief="flat", exportselection=False,
                            bg=C["cream_p"], fg=C["ink"], selectbackground=C["focus"], selectforeground=C["ink"],
                            font=self.font(15 * self.u, "bold"))
            for e in entries:
                lb.insert("end", f'  {L.when_text(e["created"])}   ·   {L.save_title(e)}   ·   {L.size_text(e["size"])}')
            lb.selection_set(0)
            lb.activate(0)
            lb.bind("<Double-Button-1>", lambda _e: self._dialog_press(-1))
            box["list"] = lb
            return lb

        def chosen():
            sel = box["list"].curselection()
            return entries[sel[0]] if sel else None

        def restore(entry):
            if entry:
                self._confirm_restore(entry)

        def rename(entry):
            if entry:
                self.ask_text("Name for this save", entry["label"] or "",
                              lambda text: (L.rename_save(entry["path"], text), self._refresh_saves(), self.on_all_saves()))

        def delete(entry):
            if not entry:
                return
            self.show_dialog(f'Delete "{L.save_title(entry)}" ({L.when_text(entry["created"])})?\nThis cannot be undone.',
                             [("Keep it", "neutral", None),
                              ("Delete", "primary", lambda: (L.delete_save(entry["path"]), self._refresh_saves(), self.on_all_saves()))])
        self.show_dialog(f'Saves of "{self._profile_name()}"',
                         [("Close", "neutral", None), ("Rename", "neutral", rename), ("Delete", "neutral", delete),
                          ("Restore", "primary", restore)], body=make, collect=chosen)

    def _dialog_press(self, index):
        if self.dialog:
            self.dialog["buttons"][index].command()

    def on_play(self):
        L = self.L
        if self.playing or not L.installed():
            return
        current = dict(self.settings)
        L.save_settings(current)
        profile = self.profile
        L.mark_played(profile)  # the launcher opens on this save next time, even if the game saves nothing
        L.auto_backup(profile)
        self.playing = True
        self.watch = L.SaveWatch(profile)
        self.refresh()
        self.set_note(f'Playing "{self._profile_name(profile)}"… close the game window to come back.')
        self._minimize(True)
        self.root.after(2000, self._watch_tick)
        exit_file = Path(tempfile.gettempdir()) / f"tomodachi-launcher-exit-{os.getpid()}.txt"

        def run():
            error, code = None, None
            try:
                env = dict(os.environ)
                if env.get("FONTCONFIG_FILE", "").endswith(f"tomodachi-launcher-fonts-{os.getuid()}.conf"):
                    env.pop("FONTCONFIG_FILE")  # the bundled-font config is for the launcher window only
                exit_file.unlink(missing_ok=True)
                env["NXRT_EXIT_FILE"] = str(exit_file)  # play-nxrt.sh writes the game's own exit code here
                code = subprocess.run(L.play_command(current, L.profile_dir(profile)), cwd=L.ROOT, env=env).returncode
                try:
                    code = int(exit_file.read_text().strip())
                except (OSError, ValueError):
                    pass  # an older play-nxrt.sh: keep the script's code
            except OSError as exc:
                error = f"Could not start the game: {exc}"
            self._game_events.put((code, error))
        threading.Thread(target=run, daemon=True).start()
        self.root.after(250, self._poll_game)

    def _minimize(self, on):
        """Get out of the way while the game runs. Some window managers do not allow it: then the window just stays."""
        try:
            self.root.iconify() if on else self.root.deiconify()
        except tk.TclError:
            pass

    def _poll_game(self):
        """Pick up the end of the game from the thread that runs it."""
        try:
            code, error = self._game_events.get_nowait()
        except queue.Empty:
            if self.playing:
                self.root.after(250, self._poll_game)
            return
        self._played(code, error)

    def _watch_tick(self):
        """While the game runs: take a save point whenever it has written a save and gone quiet."""
        if not self.playing or not self.watch:
            return
        if self.watch.poll():
            self._refresh_saves()
        self.root.after(2000, self._watch_tick)

    def _played(self, code, error=None):
        L = self.L
        self.playing = False
        self._minimize(False)
        summary = self.watch.finish(code) if self.watch and not error else {}
        self.watch = None
        self._refresh_saves()
        self.refresh()
        if error:
            self.set_note(error)
            return
        note = summary.get("text") or ""
        if code not in (0, None) and not summary.get("interrupted"):
            note = (note + " " if note else "") + f"The game ended with code {code} (see local/logs/play-nxrt-*.log)."
        self.set_note(note or "Have a nice day on the island!")
        if summary.get("interrupted"):
            previous = next((e for e in L.list_saves(self.profile) if e["kind"] in ("point", "session", "auto")), None)
            if previous:
                self.show_dialog(summary["text"] + f'\n\nGo back to "{L.save_title(previous)}" ({L.when_text(previous["created"])})?',
                                 [("Keep it", "neutral", None), ("Go back", "primary", lambda: self._confirm_restore(previous))])

    def on_controls(self):
        self.show_dialog("Controls", [("OK", "primary", None)], tabs=[t[0] for t in CONTROLS])

    def on_check(self):
        L = self.L
        result = subprocess.run([sys.executable, str(L.ROOT / "scripts/doctor.py")], cwd=L.ROOT,
                                capture_output=True, text=True)
        self.info("Check my computer", (result.stdout or result.stderr).strip() or "No output.")

    def on_report(self):
        self.set_note("Collecting logs…")
        self.root.update_idletasks()
        out = self.L.make_report()
        self.set_note(f"Bug report saved: {out.name} (no game data or keys inside). Attach it to a GitHub issue."
                      if out else "Could not create the report. Run ./install.sh --report in a terminal.")

    def mainloop(self, smoke=False):
        if smoke:  # used by the tests: build the window, then close it
            self.root.after(400, self.root.destroy)
        self.root.mainloop()


def run(L, smoke=False):
    Window(L).mainloop(smoke)
