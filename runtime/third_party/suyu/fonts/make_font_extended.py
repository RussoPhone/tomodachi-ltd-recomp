#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Gera font_extended_chinese_simplified.ttf: Open Sans Regular (font_standard.ttf,
# Apache-2.0) inalterada, mais cinco símbolos que o jogo usa fora dela
# (♪ ₩ △ ■ ◆). As formas dos cinco são desenhadas aqui, à mão; nenhuma fonte de
# terceiros fornece esses contornos. O resultado é derivado da Open Sans: Apache-2.0.
# Uso: python3 make_font_extended.py font_standard.ttf <saida.ttf>   (requer fontTools, MIT)
import math
import sys

from fontTools.ttLib import TTFont
from fontTools.pens.ttGlyphPen import TTGlyphPen

UPM = 2048


def area(p):
    return sum(p[i][0] * p[(i + 1) % len(p)][1] - p[(i + 1) % len(p)][0] * p[i][1] for i in range(len(p))) / 2


def cw(p):  # contorno externo: horário (área negativa)
    return p if area(p) < 0 else p[::-1]


def ccw(p):  # contorno interno (furo): anti-horário
    return p if area(p) > 0 else p[::-1]


def bar(x0, y0, x1, y1, w):
    """Traço de espessura horizontal w entre (x0,y0) e (x1,y1)."""
    h = w / 2
    return [(x0 - h, y0), (x0 + h, y0), (x1 + h, y1), (x1 - h, y1)]


def ellipse(cx, cy, rx, ry, tilt):
    """Contorno quadrático só de pontos fora da curva (octógono circunscrito)."""
    k = 1 / math.cos(math.pi / 8)
    c, s = math.cos(math.radians(tilt)), math.sin(math.radians(tilt))
    pts = []
    for i in range(8):
        a = math.pi / 8 + i * math.pi / 4
        x, y = rx * k * math.cos(a), ry * k * math.sin(a)
        pts.append((cx + x * c - y * s, cy + x * s + y * c))
    return pts


def glyph(contours, curves=()):
    pen = TTGlyphPen(None)
    for p in contours:
        pen.moveTo((round(p[0][0]), round(p[0][1])))
        for x, y in p[1:]:
            pen.lineTo((round(x), round(y)))
        pen.closePath()
    for p in curves:
        pts = [(round(x), round(y)) for x, y in cw(p)]
        pen.qCurveTo(*pts, None)
        pen.closePath()
    return pen.glyph(), 0


def triangle_outline():
    outer = [(100, 0), (1300, 0), (700, 1250)]
    a = math.dist(outer[1], outer[2]); b = math.dist(outer[0], outer[2]); c = math.dist(outer[0], outer[1])
    per = a + b + c
    ix = (a * outer[0][0] + b * outer[1][0] + c * outer[2][0]) / per
    iy = (a * outer[0][1] + b * outer[1][1] + c * outer[2][1]) / per
    inner = [(ix + (x - ix) * 0.58, iy + (y - iy) * 0.58) for x, y in outer]
    return [cw(outer), ccw(inner)]


def build(base, path):
    # nota musical (colcheia): cabeça elíptica inclinada, haste e bandeira
    stem = cw([(660, 260), (780, 260), (780, 1430), (660, 1430)])
    flag = cw([(780, 1430), (930, 1300), (1060, 1130), (1110, 920), (1050, 740), (960, 620),
               (940, 820), (880, 1000), (780, 1130)])
    note = glyph([stem, flag], [ellipse(400, 250, 340, 225, 22)])
    # ₩: W de quatro traços e duas barras horizontais
    won = glyph([cw(bar(130, 1400, 380, 0, 170)), cw(bar(380, 0, 650, 1050, 170)),
                 cw(bar(650, 1050, 920, 0, 170)), cw(bar(920, 0, 1170, 1400, 170)),
                 cw([(0, 520), (1300, 520), (1300, 660), (0, 660)]),
                 cw([(0, 820), (1300, 820), (1300, 960), (0, 960)])])
    tri = glyph(triangle_outline())
    sq = glyph([cw([(150, 0), (1350, 0), (1350, 1200), (150, 1200)])])
    dia = glyph([cw([(700, 0), (1300, 650), (700, 1300), (100, 650)])])
    notdef = glyph([cw([(100, 0), (1000, 0), (1000, 1400), (100, 1400)]),
                    ccw([(220, 120), (220, 1280), (880, 1280), (880, 120)])])

    names = {0x266A: ("uni266A", note[0], 1250), 0x20A9: ("uni20A9", won[0], 1400),
             0x25B3: ("uni25B3", tri[0], 1450), 0x25A0: ("uni25A0", sq[0], 1500),
             0x25C6: ("uni25C6", dia[0], 1400)}
    font = TTFont(base)
    glyf, hmtx, cmap = font["glyf"], font["hmtx"], font.getBestCmap()
    order = list(font.getGlyphOrder())
    for cp, (name, g, adv) in names.items():
        if cp in cmap or name in glyf.glyphs:
            raise SystemExit(f"U+{cp:04X} já existe na base")
        g.recalcBounds(glyf)
        glyf.glyphs[name] = g
        order.append(name)
        hmtx.metrics[name] = (adv, g.xMin)
        for t in font["cmap"].tables:
            if t.isUnicode() and (cp <= 0xFFFF or t.format == 12):
                t.cmap[cp] = name
    font.setGlyphOrder(order)
    glyf.setGlyphOrder(order)
    font["maxp"].numGlyphs = len(order)
    font["post"].extraNames = []
    font["post"].mapping = {}
    if "DSIG" in font:  # assinatura deixa de valer ao modificar a fonte
        del font["DSIG"]
    font.save(path)


if __name__ == "__main__":
    build(sys.argv[1], sys.argv[2])
