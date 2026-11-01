#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Gera font_nintendo_extended.ttf (slot 5 do pl:u, nintendo_ext_003): ícones de botões
# em área privada (U+E0A0.., U+E0E0.., U+E0B3/E0B4, U+E0EF/E0F0). Todos os contornos
# são desenhados aqui, em formas geométricas simples (círculo, retângulo arredondado,
# letras poligonais); nenhuma fonte de terceiros fornece contornos. O codepoint e o
# nome de glifo de cada ícone seguem a fonte anterior (YuzuOSSExtension) para manter
# o cmap idêntico. Licença do resultado: GPL-3.0-or-later, do projeto nxrt.
# Uso: python3 make_nintendo_extended.py <saida.ttf>     (requer fontTools, MIT)
import math
import sys

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

UPM = 1024
ADV = 1024
X0, X1 = 20, 1004          # caixa horizontal de todos os ícones
RING = 72                  # espessura do contorno dos ícones vazados

# Letras como polígonos simples em coordenadas locais (altura 560), cada letra um
# contorno externo + furos, sem sobreposição (preenchimento nonzero).
LETTERS = {
    "A": ([(0, 0), (115, 0), (148, 110), (292, 110), (325, 0), (440, 0), (270, 560), (170, 560)],
          [[(179, 210), (261, 210), (220, 346)]]),
    "B": ([(0, 0), (340, 0), (430, 70), (430, 230), (380, 290), (430, 350), (430, 490), (350, 560), (0, 560)],
          [[(100, 100), (300, 100), (330, 130), (330, 200), (300, 230), (100, 230)],
           [(100, 330), (300, 330), (330, 360), (330, 430), (300, 460), (100, 460)]]),
    "R": ([(0, 0), (100, 0), (100, 220), (170, 220), (300, 0), (430, 0), (310, 230), (380, 280),
           (430, 350), (430, 490), (350, 560), (0, 560)],
          [[(100, 330), (300, 330), (330, 360), (330, 430), (300, 460), (100, 460)]]),
    "X": ([(0, 0), (120, 0), (220, 210), (320, 0), (440, 0), (285, 280), (440, 560), (320, 560),
           (220, 350), (120, 560), (0, 560), (155, 280)], []),
    "Y": ([(0, 560), (125, 560), (220, 330), (315, 560), (440, 560), (280, 250), (280, 0), (160, 0),
           (160, 250)], []),
    "L": ([(0, 0), (400, 0), (400, 105), (105, 105), (105, 560), (0, 560)], []),
    "Z": ([(0, 0), (440, 0), (440, 100), (170, 100), (440, 460), (440, 560), (0, 560), (0, 460),
           (270, 460), (0, 100)], []),
    "S": ([(0, 0), (380, 0), (440, 60), (440, 270), (380, 330), (110, 330), (110, 450), (440, 450),
           (440, 560), (60, 560), (0, 500), (0, 290), (60, 230), (330, 230), (330, 110), (0, 110)], []),
    "+": ([(165, 0), (275, 0), (275, 165), (440, 165), (440, 275), (275, 275), (275, 440), (165, 440),
           (165, 275), (0, 275), (0, 165), (165, 165)], []),
    "-": ([(0, 0), (440, 0), (440, 110), (0, 110)], []),
}
HEIGHT = {"+": 440, "-": 110}


def area(p):
    return sum(p[i][0] * p[(i + 1) % len(p)][1] - p[(i + 1) % len(p)][0] * p[i][1]
               for i in range(len(p))) / 2


def cw(p):   # contorno externo: horário (área negativa), convenção TrueType
    return p if area(p) < 0 else p[::-1]


def ccw(p):  # furo: anti-horário
    return p if area(p) > 0 else p[::-1]


def rrect(x0, y0, x1, y1, r):
    """Retângulo arredondado: lista de (x, y, on_curve), anti-horário."""
    return [(x0 + r, y0, 1), (x1 - r, y0, 1), (x1, y0, 0), (x1, y0 + r, 1),
            (x1, y1 - r, 1), (x1, y1, 0), (x1 - r, y1, 1),
            (x0 + r, y1, 1), (x0, y1, 0), (x0, y1 - r, 1),
            (x0, y0 + r, 1), (x0, y0, 0)]


def circle(cx, cy, r):
    """Círculo quadrático só com pontos fora da curva (octógono circunscrito)."""
    k = 1 / math.cos(math.pi / 8)
    return [(cx + r * k * math.cos(math.pi / 8 + i * math.pi / 4),
             cy + r * k * math.sin(math.pi / 8 + i * math.pi / 4), 0) for i in range(8)]


def orient(pts, clockwise):
    """Ajusta a orientação de um contorno de pontos (x, y, on)."""
    a = area([(x, y) for x, y, _ in pts])
    if (a < 0) != clockwise:
        pts = pts[::-1]
    return pts


class G:
    def __init__(self):
        self.pen = TTGlyphPen(None)

    def poly(self, pts, clockwise):
        p = cw(pts) if clockwise else ccw(pts)
        self.pen.moveTo(tuple(round(v) for v in p[0]))
        for q in p[1:]:
            self.pen.lineTo(tuple(round(v) for v in q))
        self.pen.closePath()

    def curve(self, pts, clockwise):
        pts = orient(pts, clockwise)
        if all(not on for _, _, on in pts):
            self.pen.qCurveTo(*[(round(x), round(y)) for x, y, _ in pts], None)
            self.pen.closePath()
            return
        # rotaciona para começar num ponto na curva
        i = next(j for j, (_, _, on) in enumerate(pts) if on)
        pts = pts[i:] + pts[:i]
        self.pen.moveTo((round(pts[0][0]), round(pts[0][1])))
        n = len(pts)
        j = 1
        while j < n:
            x, y, on = pts[j % n]
            if on:
                self.pen.lineTo((round(x), round(y)))
                j += 1
            else:
                ex, ey, _ = pts[(j + 1) % n]
                self.pen.qCurveTo((round(x), round(y)), (round(ex), round(ey)))
                j += 2
        self.pen.closePath()

    def glyph(self):
        return self.pen.glyph()


def put_letters(g, text, cx, cy, height, filled_body):
    """Escreve o texto centrado em (cx, cy) com a altura de caixa pedida.
    Em ícone preenchido as letras são furos (anti-horário); em vazado, preto."""
    s = height / 560
    gap = 70 * s
    widths = [max(x for x, _ in LETTERS[c][0]) * s for c in text]
    total = sum(widths) + gap * (len(text) - 1)
    x = cx - total / 2
    for c, w in zip(text, widths):
        h = HEIGHT.get(c, 560) * s
        outer, holes = LETTERS[c]
        place = lambda poly: [(x + px * s, cy - h / 2 + py * s) for px, py in poly]
        g.poly(place(outer), clockwise=not filled_body)
        for hole in holes:
            g.poly(place(hole), clockwise=filled_body)
        x += w + gap


def icon(shape, text, filled, y0, y1, letter_h):
    g = G()
    cy = (y0 + y1) / 2
    if shape == "circle":
        r = (X1 - X0) / 2
        g.curve(circle((X0 + X1) / 2, cy, r), True)
        if not filled:
            g.curve(circle((X0 + X1) / 2, cy, r - RING), False)
    else:
        rad = min(90, (y1 - y0) // 4)
        g.curve(rrect(X0, y0, X1, y1, rad), True)
        if not filled:
            g.curve(rrect(X0 + RING, y0 + RING, X1 - RING, y1 - RING, max(rad - RING // 2, 10)), False)
    put_letters(g, text, (X0 + X1) / 2, cy, letter_h, filled)
    return g.glyph()


SPEC = [  # (codepoint, texto, forma, y0, y1, altura da letra)
    (0xE0A0, "A", "circle", -104, 880, 460), (0xE0A1, "B", "circle", -104, 880, 460),
    (0xE0A2, "X", "circle", -104, 880, 460), (0xE0A3, "Y", "circle", -104, 880, 460),
    (0xE0A4, "L", "rect", -67, 843, 470), (0xE0A5, "R", "rect", -67, 843, 470),
    (0xE0A6, "ZL", "rect", -11, 787, 380), (0xE0A7, "ZR", "rect", -11, 787, 380),
    (0xE0A8, "SL", "rect", 82, 694, 280), (0xE0A9, "SR", "rect", 83, 694, 280),
    (0xE0B3, "+", "circle", -104, 880, 460), (0xE0B4, "-", "circle", -104, 880, 460),
]
FILLED = [(0xE0E0 + (cp - 0xE0A0), t, s, a, b, h) for cp, t, s, a, b, h in SPEC[:10]]
FILLED += [(0xE0EF, "+", "circle", -104, 880, 460), (0xE0F0, "-", "circle", -104, 880, 460)]


def build(path):
    order = [".notdef", "uni0000", "uni000D", "space"]
    glyphs = {}
    metrics = {".notdef": (1024, 164), "uni0000": (0, 0), "uni000D": (256, 0), "space": (256, 0)}
    nd = G()
    nd.poly([(164, 0), (860, 0), (860, 700), (164, 700)], True)
    nd.poly([(264, 100), (264, 600), (760, 600), (760, 100)], False)
    glyphs[".notdef"] = nd.glyph()
    for n in order[1:]:
        glyphs[n] = TTGlyphPen(None).glyph()
    cmap = {0x0: "uni0000", 0xD: "uni000D", 0x20: "space"}
    for filled, items in ((False, SPEC), (True, FILLED)):
        for cp, text, shape, y0, y1, lh in items:
            name = f"uni{cp:04X}"
            glyphs[name] = icon(shape, text, filled, y0, y1, lh)
            cmap[cp] = name
            order.append(name)
            metrics[name] = (ADV, X0)
    order = order[:4] + sorted(order[4:])

    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf({n: glyphs[n] for n in order})
    glyf = fb.font["glyf"]
    fb.setupHorizontalMetrics({n: (metrics[n][0], glyf[n].xMin if glyf[n].numberOfContours else metrics[n][1])
                               for n in order})
    fb.setupHorizontalHeader(ascent=922, descent=-128, lineGap=0)
    fb.setupNameTable({
        "familyName": "NxrtIcons", "styleName": "Regular", "uniqueFontIdentifier": "NxrtIcons-1.0",
        "fullName": "NxrtIcons", "psName": "NxrtIcons", "version": "Version 1.0",
        "copyright": "Copyright (C) 2026 the nxrt project. Original button-icon outlines drawn for nxrt.",
        "licenseDescription": "GNU General Public License, version 3 or later (GPL-3.0-or-later).",
        "licenseInfoURL": "https://www.gnu.org/licenses/gpl-3.0.html",
    })
    fb.setupOS2(sTypoAscender=922, sTypoDescender=-128, sTypoLineGap=0, usWinAscent=922,
                usWinDescent=128, achVendID="NXRT", fsType=0, usWeightClass=400)
    fb.setupPost(keepGlyphNames=True)
    fb.save(path)


if __name__ == "__main__":
    build(sys.argv[1])
