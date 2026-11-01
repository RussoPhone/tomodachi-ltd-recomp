#!/usr/bin/env python3
"""Cobertura de codepoints da fonte font_extended_chinese_simplified.ttf (H8).

Standalone, sem fontTools: parser mínimo da tabela cmap (formatos 4 e 12).
Uso: test_font_coverage.py [caminho.ttf]   (sai 0 se tudo presente, 1 se faltar algo)
"""
import os
import struct
import sys

DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                       "third_party", "suyu", "fonts",
                       "font_extended_chinese_simplified.ttf")

SYMBOLS = {0x266A: "♪", 0x20A9: "₩", 0x25B3: "△", 0x25A0: "■", 0x25C6: "◆"}
BASIC_LATIN = range(0x20, 0x7F)


def table(data, tag):
    n = struct.unpack_from(">H", data, 4)[0]
    for i in range(n):
        t, _, off, length = struct.unpack_from(">4sIII", data, 12 + 16 * i)
        if t == tag:
            return off, length
    raise ValueError("tabela %s ausente" % tag.decode())


def fmt4(data, o):
    segx2 = struct.unpack_from(">H", data, o + 6)[0]
    seg = segx2 // 2
    end = struct.unpack_from(">%dH" % seg, data, o + 14)
    start = struct.unpack_from(">%dH" % seg, data, o + 16 + segx2)
    delta = struct.unpack_from(">%dh" % seg, data, o + 16 + 2 * segx2)
    ro_pos = o + 16 + 3 * segx2
    ro = struct.unpack_from(">%dH" % seg, data, ro_pos)
    out = {}
    for i in range(seg):
        for c in range(start[i], end[i] + 1):
            if c == 0xFFFF:
                continue
            if ro[i] == 0:
                g = (c + delta[i]) & 0xFFFF
            else:
                p = ro_pos + 2 * i + ro[i] + 2 * (c - start[i])
                g = struct.unpack_from(">H", data, p)[0]
                if g:
                    g = (g + delta[i]) & 0xFFFF
            if g:
                out[c] = g
    return out


def fmt12(data, o):
    n = struct.unpack_from(">I", data, o + 12)[0]
    out = {}
    for i in range(n):
        s, e, g = struct.unpack_from(">III", data, o + 16 + 12 * i)
        for c in range(s, e + 1):
            if g + c - s:
                out[c] = g + c - s
    return out


def read_cmap(path):
    with open(path, "rb") as f:
        data = f.read()
    off, _ = table(data, b"cmap")
    n = struct.unpack_from(">H", data, off + 2)[0]
    cmap = {}
    for i in range(n):
        pid, eid, so = struct.unpack_from(">HHI", data, off + 4 + 8 * i)
        sub = off + so
        fmt = struct.unpack_from(">H", data, sub)[0]
        if fmt == 4:
            cmap.update(fmt4(data, sub))
        elif fmt == 12:
            cmap.update(fmt12(data, sub))
    return cmap


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    cmap = read_cmap(path)
    print("fonte: %s (%d codepoints mapeados)" % (os.path.normpath(path), len(cmap)))
    fails = 0
    for cp, ch in SYMBOLS.items():
        ok = cp in cmap
        fails += not ok
        print("%s U+%04X %s%s" % ("ok  " if ok else "FALTA", cp, ch,
                                  " glyph %d" % cmap[cp] if ok else ""))
    miss = [c for c in BASIC_LATIN if c not in cmap]
    fails += len(miss)
    print("%s latinos básicos U+0020-U+007E: %d/%d%s" % (
        "ok  " if not miss else "FALTA", len(BASIC_LATIN) - len(miss), len(BASIC_LATIN),
        "" if not miss else " ausentes: " + " ".join("U+%04X" % c for c in miss)))
    print("RESULTADO: %s" % ("PASSOU" if not fails else "FALHOU (%d ausências)" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
