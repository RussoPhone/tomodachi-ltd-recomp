#!/usr/bin/env python3
"""Triagem de log do nxrt: linha de tempo de serviços, primeira falha e
intervalo entre IDatabaseService/applet e a queda.

Uso: tools/triagem-log.py LOG [--top N]
"""
import argparse
import re
import sys
from collections import Counter, OrderedDict

LINE = re.compile(r"^\[\s*(\d+(?:\.\d+)?)\]\s+(\w+)\s+(.*)$")
# "Iface::Method", "appletOE::Open...", ou "sm:::GetServiceHandle"
CALL = re.compile(r"^([A-Za-z_][\w]*?):{2,3}([A-Za-z_]\w*)")
FAULT_KEYS = ("guest read fault", "guest write fault", "guest fetch fault",
              "backtrace", "memory fault")
FAULT_ADDR = re.compile(r"fault at (0x[0-9a-fA-F]+)")
WATCH = re.compile(r"IDatabaseService|applet", re.I)


def parse(path):
    for n, raw in enumerate(open(path, errors="replace"), 1):
        m = LINE.match(raw.rstrip("\n"))
        if m:
            yield n, float(m.group(1)), m.group(2), m.group(3)


def is_failure(level, msg):
    low = msg.lower()
    return level == "error" or any(k in low for k in FAULT_KEYS)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log")
    ap.add_argument("--top", type=int, default=4,
                    help="chamadas mais frequentes mostradas por segundo")
    a = ap.parse_args()

    calls = []          # (t, nome)
    first_fail = None   # (n, t, msg)
    fail_ctx = []       # linhas de falha seguintes à primeira
    addr = None
    t_end = 0.0
    total = 0
    for n, t, lvl, msg in parse(a.log):
        total += 1
        t_end = t
        if first_fail is None:
            m = CALL.match(msg)
            if m and lvl == "debug":
                calls.append((t, f"{m.group(1)}::{m.group(2)}"))
            if is_failure(lvl, msg):
                first_fail = (n, t, msg)
        else:
            if is_failure(lvl, msg):
                fail_ctx.append((n, t, msg))
    if total == 0:
        sys.exit("nenhuma linha no formato '[ tempo] nível  mensagem'")

    for _, _, msg in ([first_fail] if first_fail else []) + fail_ctx:
        m = FAULT_ADDR.search(msg)
        if m:
            addr = m.group(1)
            break

    print(f"== {a.log}: {total} linhas, {t_end:.3f} s ==\n")
    print("(1) Chamadas de serviço por segundo"
          + (" (até a primeira falha)" if first_fail else ""))
    buckets = OrderedDict()
    for t, name in calls:
        buckets.setdefault(int(t), Counter())[name] += 1
    for s, c in buckets.items():
        top = ", ".join(f"{k} x{v}" if v > 1 else k
                        for k, v in c.most_common(a.top))
        more = len(c) - a.top
        print(f"  {s:4d}s  {sum(c.values()):5d} chamadas  {top}"
              + (f" (+{more} outras)" if more > 0 else ""))
    if not calls:
        print("  (nenhuma)")

    print("\n(2) Primeira falha")
    if first_fail:
        n, t, msg = first_fail
        print(f"  linha {n}, t={t:.3f} s: {msg}")
        print(f"  endereço: {addr or 'não informado'}")
        for _, _, m in fail_ctx:
            if m.lstrip().startswith("backtrace"):
                print(f"  {m.strip()}")
                break
        for _, _, m in fail_ctx:
            if "fault at" in m and m != msg:
                print(f"  {m.strip()}")
                break
    else:
        print("  nenhuma falha encontrada")

    print("\n(3) Intervalo até a queda (IDatabaseService / applet*)")
    if not first_fail:
        print("  sem queda no log")
        return
    t_fail = first_fail[1]
    groups = OrderedDict()
    for t, name in calls:
        if WATCH.search(name):
            groups.setdefault(name, []).append(t)
    if not groups:
        print("  nenhuma chamada IDatabaseService/applet antes da queda")
    for name, ts in groups.items():
        print(f"  {name}: x{len(ts)}  primeira t={ts[0]:.3f} "
              f"(queda em +{t_fail - ts[0]:.3f} s)  última t={ts[-1]:.3f} "
              f"(queda em +{t_fail - ts[-1]:.3f} s)")
    last = max((ts[-1], nm) for nm, ts in groups.items()) if groups else None
    if last:
        print(f"  última chamada monitorada: {last[1]} a "
              f"{t_fail - last[0]:.3f} s da queda")


if __name__ == "__main__":
    main()
