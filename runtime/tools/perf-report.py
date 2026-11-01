#!/usr/bin/env python3
"""P1: pós-processa a saída de tools/perf-preload.c (samples.bin, threads.csv, maps.txt).

Uso: tools/perf-report.py <dir> [--from S] [--to S] [--bin build-perf/nxrt] [--top N]
  --from/--to  janela em segundos desde o primeiro sample (para isolar a cena pesada)
Imprime: CPU por thread/grupo, CPU por categoria, topo de funções (self e inclusivo),
estados/syscalls das threads (espera) e uma tabela por categoria.
"""
import argparse, bisect, collections, os, re, struct, subprocess, sys


def load_maps(path):
    maps = []  # (lo, hi, off, file)
    for line in open(path):
        p = line.split(None, 5)
        if len(p) < 6 or 'x' not in p[1]:
            continue
        lo, hi = (int(x, 16) for x in p[0].split('-'))
        maps.append((lo, hi, int(p[2], 16), p[5].strip()))
    maps.sort()
    return maps


def load_bases(path):
    base = {}
    for line in open(path):
        p = line.split(None, 5)
        if len(p) < 6 or not p[5].strip().startswith('/'):
            continue
        lo = int(p[0].split('-')[0], 16)
        f = p[5].strip()
        if f not in base or lo < base[f]:
            base[f] = lo
    return base


def read_samples(path):
    data = open(path, 'rb').read()
    out, i = [], 0
    while i + 16 <= len(data):
        t, tid, n = struct.unpack_from('<QII', data, i)
        if n > 64 or i + 16 + 8 * n > len(data):
            break
        pcs = struct.unpack_from('<%dQ' % n, data, i + 16)
        out.append((t, tid, pcs))
        i += 16 + 8 * n
    return out


class Sym:
    def __init__(self, maps, bases, binpath):
        self.maps, self.bases, self.binpath = maps, bases, binpath
        self.los = [m[0] for m in maps]
        self.cache = {}
        self.pending = collections.defaultdict(set)

    def module(self, pc):
        i = bisect.bisect_right(self.los, pc) - 1
        if i >= 0 and self.maps[i][0] <= pc < self.maps[i][1]:
            m = self.maps[i]
            f = m[3]
            return f, pc - self.bases.get(f, m[0] - m[2])
        return None, pc

    def resolve_all(self, pcs):
        by_mod = collections.defaultdict(list)
        for pc in set(pcs):
            f, off = self.module(pc)
            if f and f.startswith('/') and os.path.exists(f):
                by_mod[f].append((pc, off))
            else:
                self.cache[pc] = ('?', f or '?')
        for f, items in by_mod.items():
            # PCs de retorno caem na instrução seguinte à chamada: -1 evita atribuir à função errada
            args = ['addr2line', '-f', '-C', '-e', f] + ['0x%x' % (off - 1) for _, off in items]
            try:
                r = subprocess.run(args, capture_output=True, text=True, timeout=600).stdout.splitlines()
            except Exception:
                r = []
            for k, (pc, off) in enumerate(items):
                name = r[2 * k] if 2 * k < len(r) else '??'
                src = r[2 * k + 1] if 2 * k + 1 < len(r) else '??:0'
                self.cache[pc] = (name, os.path.basename(f), src)
        return self.cache

    def name(self, pc):
        c = self.cache.get(pc)
        if not c:
            return '?', '?'
        return c[0], c[1]


CATS = [
    # (categoria, teste em (função, módulo, pilha_inteira_str)) — a primeira regra que casar na pilha vence
    ('driver/Vulkan (GPU+libs)', lambda f, m: re.search(r'nvidia|libvulkan|libGLX|libEGL|libX11|libxcb|libwayland|mesa|vk_', m) is not None),
    ('renderer (CPU)', lambda f, m: re.search(r'NvnRenderer|nxrt::render|nxrt::vk|VulkanContext|shader|Shader|Maxwell|Spirv|spirv', f) is not None),
    ('audio', lambda f, m: re.search(r'audio|Audio|audren|Audren|adpcm|SDL_Audio|pipewire|pulse', f + m) is not None),
    ('kernel/HLE (svc/ipc/services)', lambda f, m: re.search(r'nxrt::(kernel|hle|svc|ipc|services|Svc|Ipc)|Svc[A-Z]|HandleIpc|ipc::', f) is not None),
]


def categorize(frames):
    """frames: lista (função, módulo) da folha para a raiz."""
    for cat, test in CATS:
        for f, m in frames:
            if test(f, m):
                return cat
    for f, m in frames:
        if re.match(r'(main|sub_|func_|FUN_|nso_|guest)', f) or 'main+0x' in f:
            return 'guest (AOT)'
    return 'outros/host (libc, sync)'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--from', dest='t0', type=float, default=0.0)
    ap.add_argument('--to', dest='t1', type=float, default=1e9)
    ap.add_argument('--bin', default=None)
    ap.add_argument('--top', type=int, default=25)
    a = ap.parse_args()
    d = a.dir
    maps = load_maps(os.path.join(d, 'maps.txt'))
    bases = load_bases(os.path.join(d, 'maps.txt'))
    samples = read_samples(os.path.join(d, 'samples.bin'))
    if not samples:
        sys.exit('sem amostras')
    t_first = samples[0][0]
    samples = [s for s in samples if a.t0 <= (s[0] - t_first) / 1e9 <= a.t1]
    print('amostras: %d, janela %.1f–%.1f s' % (len(samples), a.t0, min(a.t1, (samples[-1][0] - t_first) / 1e9)))

    comm = {}
    for line in open(os.path.join(d, 'threads.csv')):
        p = line.rstrip('\n').split(',')
        if len(p) >= 3 and p[1].isdigit():
            comm.setdefault(int(p[1]), p[2])

    sym = Sym(maps, bases, a.bin)
    allpcs = [pc for _, _, pcs in samples for pc in pcs]
    sym.resolve_all(allpcs)

    def group(c):
        return 'guest-*' if c.startswith('guest-') else c

    per_thread = collections.Counter()
    per_cat = collections.Counter()
    cat_thread = collections.Counter()
    self_fn = collections.Counter()
    incl_fn = collections.Counter()
    leaf_mod = collections.Counter()
    guest_thr = collections.Counter()
    cat_fn = collections.defaultdict(collections.Counter)
    for t, tid, pcs in samples:
        c = comm.get(tid, 'tid%d' % tid)
        frames = [sym.name(pc) for pc in pcs]
        if not frames:
            continue
        cat = categorize(frames)
        per_thread[group(c)] += 1
        if c.startswith('guest-'):
            guest_thr[c] += 1
        per_cat[cat] += 1
        cat_thread[(cat, group(c))] += 1
        self_fn[frames[0]] += 1
        leaf_mod[frames[0][1]] += 1
        cat_fn[cat][frames[0]] += 1
        for fn in set(frames):
            incl_fn[fn] += 1

    n = len(samples)
    print('\n== CPU por thread (grupo) ==')
    for k, v in per_thread.most_common(15):
        print('%6.1f%%  %6d  %s' % (100.0 * v / n, v, k))
    print('\n== guest-* individuais ==')
    for k, v in guest_thr.most_common(8):
        print('%6.1f%%  %6d  %s' % (100.0 * v / n, v, k))
    print('\n== CPU por categoria (folha→raiz, 1ª regra que casa) ==')
    for k, v in per_cat.most_common():
        print('%6.1f%%  %6d  %s' % (100.0 * v / n, v, k))
    print('\n== categoria × thread ==')
    for (cat, th), v in cat_thread.most_common(20):
        print('%6.1f%%  %-34s %s' % (100.0 * v / n, cat, th))
    print('\n== módulo da folha ==')
    for k, v in leaf_mod.most_common(10):
        print('%6.1f%%  %s' % (100.0 * v / n, k))
    print('\n== topo self ==')
    for (f, m), v in self_fn.most_common(a.top):
        print('%6.1f%%  %s  [%s]' % (100.0 * v / n, f[:110], m))
    print('\n== topo inclusivo ==')
    for (f, m), v in incl_fn.most_common(a.top):
        print('%6.1f%%  %s  [%s]' % (100.0 * v / n, f[:110], m))
    print('\n== topo self por categoria ==')
    for cat, ctr in cat_fn.items():
        print('-- %s' % cat)
        for (f, m), v in ctr.most_common(6):
            print('   %6.1f%%  %s  [%s]' % (100.0 * v / n, f[:100], m))

    # espera: estados/syscalls das threads (amostragem de 100 ms)
    st = collections.Counter()
    tot = collections.Counter()
    t_csv0 = None
    for line in open(os.path.join(d, 'threads.csv')):
        p = line.rstrip('\n').split(',')
        if len(p) < 7 or not p[0].isdigit():
            continue
        tn = int(p[0])
        if t_csv0 is None:
            t_csv0 = tn
        s = (tn - t_csv0) / 1e9
        if not (a.t0 <= s <= a.t1):
            continue
        g = group(p[2])
        tot[g] += 1
        st[(g, p[3], p[6])] += 1
    print('\n== estados das threads (amostras de 100 ms; S=dormindo, R=executando) ==')
    for (g, s, sc), v in sorted(st.items(), key=lambda kv: -kv[1])[:30]:
        if g == 'guest-*':
            continue
        print('%6.1f%%  %-18s %s syscall=%s' % (100.0 * v / max(1, tot[g]), g, s, sc))


if __name__ == '__main__':
    main()
