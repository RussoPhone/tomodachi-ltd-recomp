"""Window for scripts/launcher.py, drawn in the Island UI Kit style (https://russophone.github.io/island-ui-kit/).

Soft pills with a darker side and a soft shadow, cream surfaces with doodles, red primary buttons with wavy
stripes, cloud dialogs, and the game's motion: focus in 2 frames (scale 1.08), confirm as squash-and-pop
(1.1 -> 1.05 -> 1.2 -> rest over 12 frames), switches and tabs settle with a small overshoot.
Everything is drawn on one Tk Canvas; the logic (settings, play, backups, report) stays in launcher.py.
"""
import math
import os
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
from tkinter import filedialog

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
BASE_W, BASE_H = 940, 700

CONTROLS = (
    "KEYBOARD\n"
    "  Left stick: I K J L      Right stick: Q / E      D-pad: arrow keys\n"
    "  A: X   B: Z   X: S   Y: A   L: W   R: R   ZL: 1   ZR: 3\n"
    "  +: Enter   -: Backspace   HOME (close the game): Home   Fullscreen: F11\n\n"
    "MOUSE (experimental, awkward in some menus)\n"
    "  Island: push the mouse to a window edge, or middle-button drag, to move the camera; wheel = zoom\n"
    "  Left click: touch screen / A   Right click: B   F8: capture the mouse as an analog stick (F6/F7 = sensitivity)\n\n"
    "CONTROLLER (Nintendo layout)\n"
    "  Right face button = A, bottom = B, top = X, left = Y   Guide = close the game\n\n"
    "Full table: docs/CONTROLS.md")


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
              "focus": ("focus", "orange_side", "ink", False)}  # the yellow "new build" chip

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
        cv.create_text(cx, cy, text=self.label, fill=fg, font=self.ui.fit_font(self.label, h * 0.36, w - h * 0.6),
                       tags=self.tag)

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
        root.geometry(f"{BASE_W}x{BASE_H}")
        root.minsize(760, 600)
        self.settings = L.load_settings()
        self.playing = False
        self.dialog = None
        self.note = ""
        self._fonts = {}
        self.u = 1.0
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
        self.save_btns = [Pill(self, "bk", "Back up now", "soft", self.on_backup),
                          Pill(self, "rs", "Restore…", "soft", self.on_restore),
                          Pill(self, "of", "Open folder", "soft", lambda: L.open_folder(L.SAVE_DIR))]
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
            return list(self.dialog["buttons"])
        return [self.fullscreen, self.safe, self.updates, self.size, self.language, self.play_btn, self.update_btn] \
            + self.save_btns + self.help_btns

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
        design_h = 696 * u  # height the content needs at this scale
        slack = max(0.0, H - design_h)
        vg = min(slack / 6, 34 * u)  # extra space between the rows; the rest goes below the header
        top_extra = min((slack - 3 * vg) / 2, 60 * u) if slack > 3 * vg else 0.0
        cv.delete("bg", "card", "label", "status", "note")
        self._doodles(W, H, u)
        pad, gap = 26 * u, 22 * u

        # header
        self.hh = hh = 92 * u
        for dy, col in ((7 * u, C["orange_side"]), (0, C["orange"])):
            pts, x = [0, 0, W, 0], W
            while x >= -10:
                pts += (x, hh + dy + 6 * u * math.sin(x / (64 * u)))
                x -= 12
            cv.create_polygon(pts, fill=col, outline="", smooth=True, tags="bg")
        t = cv.create_text(pad, hh * 0.40, text="Tomodachi-LTD-Recomp", anchor="w", fill=C["paper"],
                           font=self.font(32 * u), tags="label")
        if self.version:  # build chip next to the title
            bx = cv.bbox(t)[2] + 14 * u
            f = self.font(14 * u)
            bw, bh2 = f.measure(self.version) + 24 * u, 28 * u
            cv.create_polygon(blob(bx, hh * 0.40 - bh2 / 2 + 4 * u, bx + bw, hh * 0.40 + bh2 / 2 + 4 * u, bh2 / 2),
                              smooth=True, fill=C["orange_side"], outline="", tags="label")
            cv.create_polygon(blob(bx, hh * 0.40 - bh2 / 2, bx + bw, hh * 0.40 + bh2 / 2, bh2 / 2), smooth=True,
                              fill=C["cream_l"], outline="", tags="label")
            cv.create_text(bx + bw / 2, hh * 0.40, text=self.version, fill=C["ink"], font=f, tags="label")
            if self.update_info:  # a newer published build: yellow chip, click for how to update
                label = f'{self.update_info["version"]} available'
                uw = f.measure(label) + 40 * u
                ux = bx + bw + 10 * u
                self.update_btn.set_label(label)
                self.update_btn.place(ux, hh * 0.40 - 18 * u, ux + uw, hh * 0.40 + 18 * u, C["orange"])
            else:
                self.update_btn.box = (0, 0, 0, 0)
                cv.delete(self.update_btn.tag)
        else:
            self.update_btn.box = (0, 0, 0, 0)
            cv.delete(self.update_btn.tag)
        cv.create_text(pad + 2, hh * 0.76, text="Unofficial PC Port · Bring your own copy!",
                       anchor="w", fill=C["cream_l"], font=self.font(14 * u), tags="label")

        # row 1: play | settings
        top = hh + 32 * u + top_extra
        mid = W * 0.47
        h1 = 284 * u
        self._card(pad, top, mid - gap / 2, top + h1, "Play")
        self._card(mid + gap / 2, top, W - pad, top + h1, "Settings")
        ix0, ix1 = pad + 26 * u, mid - gap / 2 - 26 * u
        self.play_btn.place(ix0, top + 66 * u, ix1, top + 170 * u, C["paper"])
        self._hint(ix0, top + 226 * u, "↵", "Play", u)
        cv.create_text(ix0, top + 256 * u, text="First face-paint opening takes ~40 s (it builds a cache).",
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

        # row 2: save data | help
        top2 = top + h1 + gap + 8 * u + vg
        h2 = 168 * u
        split = W * 0.36
        self._card(pad, top2, split - gap / 2, top2 + h2, "Save data")
        self._card(split + gap / 2, top2, W - pad, top2 + h2, "Help")
        bx0, bx1 = pad + 22 * u, split - gap / 2 - 22 * u
        bh, by = 46 * u, top2 + 58 * u
        bw = (bx1 - bx0 - 10 * u) / 2
        self.save_btns[0].place(bx0, by, bx0 + bw, by + bh)
        self.save_btns[1].place(bx0 + bw + 10 * u, by, bx1, by + bh)
        self.save_btns[2].place(bx0, by + bh + 16 * u, bx1, by + 2 * bh + 16 * u)
        hx0, hx1 = split + gap / 2 + 22 * u, W - pad - 22 * u
        cols = 3 if hx1 - hx0 > 480 * u else 2
        hb = bh if cols == 3 else bh * 0.78
        hw = (hx1 - hx0 - (cols - 1) * 10 * u) / cols
        for i, b in enumerate(self.help_btns):
            cx, ry = i % cols, i // cols
            x = hx0 + cx * (hw + 10 * u)
            y = by - (6 * u if cols == 2 else 0) + ry * (hb + (16 if cols == 3 else 9) * u)
            b.place(x, y, x + hw, y + hb)

        # note balloon
        ny = top2 + h2 + gap + 10 * u + vg
        self.note_box = (pad, ny, W - pad, min(H - 16 * u, ny + 54 * u))
        self._draw_note()
        for t in ("play", "upd", "fullscreen", "safe", "updates", "size", "language", "bk", "rs", "of", "hc", "hk", "hr", "hl", "ht", "hp"):
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
        right, cy, h = W - 26 * u, 92 * u * 0.5, 38 * u
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
        return self.dialog is not None and widget not in self.dialog["buttons"]

    def show_dialog(self, message, buttons, body=None):
        """buttons: [(label, style, callback or None)]; the last one answers Return."""
        self.close_popup()
        if self.dialog:
            self.close_dialog()
        pills = []
        for i, (label, style, cb) in enumerate(buttons):
            def act(cb=cb):
                self.close_dialog()
                if cb:
                    cb()
            pills.append(Pill(self, f"dlg_b{i}", label, style, act))
        text = None
        if body:
            text = tk.Text(self.cv, wrap="none", bd=0, highlightthickness=0, bg=C["cream_p"], fg=C["ink"],
                           padx=12, pady=10, cursor="arrow", font=self.font(13 * self.u, "normal"))
            text.insert("1.0", body)
            text.config(state="disabled")
        self._set_hover(None)
        self.dialog = {"message": message, "buttons": pills, "body": text, "anim": None}
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
        big = d["body"] is not None
        cw = min((820 if big else 620) * u, W - 40) * s
        ch = min((520 if big else 340) * u, H - 40) * s
        cx, cy = W / 2, H / 2
        x0, y0, x1, y1 = cx - cw / 2, cy - ch / 2, cx + cw / 2, cy + ch / 2
        r = ch * (0.22 if big else 0.45)
        wob = ch * (0.015 if big else 0.03)
        cv.create_polygon(blob(x0, y0 + 16 * u, x1, y1 + 16 * u, r, wobble=wob, seed=3), smooth=True,
                          fill=mix(C["dim"], "#000000", 0.35), outline="", tags="dlg")
        cv.create_polygon(blob(x0, y0, x1, y1, r, wobble=wob, seed=3), smooth=True, fill=C["cream"],
                          outline="", tags="dlg")
        ty = y0 + ch * (0.12 if big else 0.36)
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
        if big and s >= 0.99:
            tx0, ty0 = x0 + cw * 0.07, ty + 30 * u
            cv.create_window(tx0, ty0, anchor="nw", window=d["body"], width=cw * 0.86,
                             height=max(60, by - ty0 - 24 * u), tags="dlg")

    def close_dialog(self):
        if not self.dialog:
            return
        if self.dialog.get("anim"):
            self.root.after_cancel(self.dialog["anim"])
        for b in self.dialog["buttons"]:
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
    def on_play(self):
        L = self.L
        if self.playing or not L.installed():
            return
        current = dict(self.settings)
        L.save_settings(current)
        L.auto_backup()
        self.playing = True
        self.refresh()
        self.set_note("Playing… close the game window to come back.")
        self.root.iconify()

        def run():
            try:
                env = dict(os.environ)
                if env.get("FONTCONFIG_FILE", "").endswith(f"tomodachi-launcher-fonts-{os.getuid()}.conf"):
                    env.pop("FONTCONFIG_FILE")  # the bundled-font config is for the launcher window only
                code = subprocess.run(L.play_command(current), cwd=L.ROOT, env=env).returncode
                note = ("Have a nice day on the island!" if code == 0
                        else f"The game ended with code {code}. See local/logs/play-nxrt-*.log.")
            except OSError as error:
                note = f"Could not start the game: {error}"
            self.root.after(0, lambda: self._played(note))
        threading.Thread(target=run, daemon=True).start()

    def _played(self, note):
        self.playing = False
        self.root.deiconify()
        self.refresh()
        self.set_note(note)

    def on_backup(self):
        out = self.L.backup_save()
        self.set_note(f"Backup saved: {out.relative_to(self.L.ROOT)}" if out else "There is no save to back up yet.")

    def on_restore(self):
        L = self.L
        archive = filedialog.askopenfilename(title="Choose a save backup (.zip)",
                                             initialdir=L.BACKUPS if L.BACKUPS.exists() else L.ROOT,
                                             filetypes=[("Backups", "*.zip")])
        if not archive:
            return

        def go():
            try:
                safety = L.restore_save(archive)
            except (OSError, ValueError, zipfile.BadZipFile) as error:
                self.show_dialog(f"Restore failed.\n{error}", [("OK", "primary", None)])
                return
            self.set_note("Save restored." + (f" Previous save kept in {safety.relative_to(L.ROOT)}." if safety else ""))
        self.show_dialog("This replaces your current save with the backup.\nThe current save is backed up first.",
                         [("Cancel", "neutral", None), ("Restore", "primary", go)])

    def on_controls(self):
        self.info("Controls", CONTROLS)

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
