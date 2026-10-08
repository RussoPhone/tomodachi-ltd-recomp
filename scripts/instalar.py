#!/usr/bin/env python3
"""Instalador do Tomodachi Life: Living the Dream (versão nativa para PC).

Faz tudo em ordem, mostrando cada passo, e continua de onde parou se for interrompido:
  1. confere o computador         6. extrai o código do seu jogo
  2. recebe seu jogo e suas chaves 7. traduz o código para C
  3. baixa o suyu/mk8-recomp       8. compila a versão nativa
  4. aplica as correções           9. extrai os dados do jogo
  5. compila as ferramentas       10. monta a pasta para jogar

Nada do jogo vem neste projeto: tudo é gerado no seu computador a partir da sua cópia.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
STATE = ROOT / 'local/estado'
UPSTREAM = ROOT / 'upstream/mk8-recomp'
SUYU = UPSTREAM / 'third_party/suyu'
TARGET = 'tomodachi'
GREEN, YELLOW, RED, BOLD, RESET = '\033[32m', '\033[33m', '\033[31m', '\033[1m', '\033[0m'


def say(text=''):
    print(text, flush=True)


def fail(text):
    say(f'\n{RED}{BOLD}✗ {text}{RESET}')
    say('Corrija o problema acima e rode ./instalar.sh de novo: ele continua de onde parou.')
    sys.exit(1)


def run(cmd, log, env=None, cwd=ROOT):
    log = ROOT / 'local/logs' / log
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w') as out:
        rc = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT).returncode
    if rc:
        tail = log.read_text(errors='replace').splitlines()[-15:]
        say('\n'.join('   ' + l for l in tail))
        fail(f'Este passo falhou. O registro completo está em {log.relative_to(ROOT)}')


def ram_gb():
    for line in open('/proc/meminfo'):
        if line.startswith('MemTotal'):
            return int(line.split()[1]) / 2**20
    return 8


def swap_gb():
    for line in open('/proc/meminfo'):
        if line.startswith('SwapTotal'):
            return int(line.split()[1]) / 2**20
    return 0


def pick(kind, title):
    """Ask for a file or folder with a graphical dialog when available, otherwise in the terminal."""
    if shutil.which('zenity') and (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')):
        args = ['zenity', '--file-selection', f'--title={title}'] + (['--directory'] if kind == 'dir' else [])
        res = subprocess.run(args, capture_output=True, text=True)
        if res.returncode == 0 and res.stdout.strip():
            return res.stdout.strip()
    return input(f'   {title}\n   Cole o caminho aqui e aperte Enter: ').strip().strip('"\'')


STEPS = []


def step(title):
    def deco(fn):
        STEPS.append((fn.__name__, title, fn))
        return fn
    return deco


@step('Conferir o computador')
def check():
    missing = [t for t in ('git', 'cmake', 'ninja', 'clang', 'g++', 'glslangValidator') if not shutil.which(t)]
    try:
        import cryptography  # noqa: F401
    except ImportError:
        missing.append('python-cryptography')
    if missing:
        fail('Faltam programas: ' + ', '.join(missing) + '.\n  Instale com o comando da seção "1. Preparar o computador" do README.')
    if sys.platform != 'linux':
        fail('Por enquanto o instalador funciona só no Linux.')
    mem, swap = ram_gb(), swap_gb()
    free = shutil.disk_usage(ROOT).free / 2**30
    say(f'   Memória: {mem:.0f} GB (+ {swap:.0f} GB de swap) · Espaço livre: {free:.0f} GB')
    if mem + swap < 24:
        fail('São necessários pelo menos 16 GB de RAM e 8 GB de swap (24 GB somados) para compilar o jogo.')
    if free < 35:
        fail(f'São necessários uns 35 GB livres no disco (há {free:.0f} GB).')


@step('Escolher seu jogo e suas chaves')
def choose():
    dump = os.environ.get('TOMODACHI_NSP') or pick('file', 'Escolha o arquivo do Tomodachi Life: Living the Dream (.nsp)')
    keys = os.environ.get('TOMODACHI_KEYS') or pick('dir', 'Escolha a pasta onde está o seu prod.keys')
    dump, keys = Path(dump).expanduser().resolve(), Path(keys).expanduser().resolve()
    if not dump.is_file():
        fail(f'Não encontrei o arquivo do jogo: {dump}')
    with open(dump, 'rb') as f:
        if f.read(4) != b'PFS0':
            fail('Esse arquivo não parece ser um .nsp do Switch.')
    if not (keys / 'prod.keys').is_file():
        fail(f'Não encontrei prod.keys dentro de {keys}')
    user_keys = ROOT / 'local/runtime/user/keys'
    user_keys.mkdir(parents=True, exist_ok=True)
    os.chmod(user_keys, 0o700)
    for name in ('prod.keys', 'title.keys'):
        if (keys / name).is_file():
            shutil.copyfile(keys / name, user_keys / name)
            os.chmod(user_keys / name, 0o600)
    say('   Calculando a identidade do arquivo (leva alguns segundos)...')
    digest = hashlib.sha256()
    with open(dump, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 24), b''):
            digest.update(chunk)
    (ROOT / 'local/runtime.json').write_text(json.dumps({'dump_path': str(dump), 'target': TARGET}, indent=2))
    (ROOT / 'artifacts').mkdir(exist_ok=True)
    (ROOT / 'artifacts/input-identity.json').write_text(json.dumps(
        {'schema_version': 1, 'sha256': digest.hexdigest(), 'size_bytes': dump.stat().st_size}, indent=2))
    say(f'   Jogo: {dump.name}\n   Chaves: copiadas para a pasta do instalador (nunca são enviadas a lugar nenhum)')


@step('Baixar o suyu/mk8-recomp (código aberto, GPL)')
def fetch():
    lock = json.loads((ROOT / 'upstream.lock.json').read_text())
    if not (UPSTREAM / '.git').exists():
        UPSTREAM.parent.mkdir(exist_ok=True)
        run(['git', 'clone', lock['repository'], UPSTREAM], 'git-clone.log')
    run(['git', '-C', UPSTREAM, 'checkout', '-q', lock['commit']], 'git-checkout.log')
    say('   Baixando os submódulos (pode levar alguns minutos)...')
    run(['git', '-C', UPSTREAM, 'submodule', 'update', '--init', '--recursive'], 'git-submodules.log')
    run([sys.executable, 'scripts/verify-upstream.py'], 'verify-upstream.log')


@step('Aplicar as correções deste projeto')
def patches():
    for patch in sorted((ROOT / 'patches').glob('*.patch')):
        if subprocess.run(['git', '-C', SUYU, 'apply', '--reverse', '--check', patch], capture_output=True).returncode == 0:
            say(f'   já aplicada: {patch.name}')
            continue
        run(['git', '-C', SUYU, 'apply', patch], f'patch-{patch.stem}.log')
        say(f'   aplicada: {patch.name}')


def jobs():
    return max(1, min(os.cpu_count() or 1, int((ram_gb() - 3) / 1.5)))


@step('Compilar as ferramentas (suyu) — uns 20 a 40 minutos')
def backend():
    env = {**os.environ, 'BUILD_JOBS': str(jobs())}
    run(['bash', 'scripts/build-backend.sh'], 'build-backend.log', env=env)


@step('Extrair o código do seu jogo')
def dump():
    run([sys.executable, 'scripts/dump-exefs.py'], 'dump-exefs.log')
    import importlib.util
    spec = importlib.util.spec_from_file_location('nso', UPSTREAM / 'scripts/nso.py')
    nso = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(nso)
    adapter = json.loads((ROOT / f'adapters/{TARGET}/target.json').read_text())
    exefs = ROOT / f"local/runtime/user/dump/{adapter['expected_title_id']}/exefs"
    for module, expected in adapter['expected_modules'].items():
        build_id = nso.Nso(exefs / module).build_id
        if build_id != expected['build_id']:
            fail(f'Seu jogo tem uma versão diferente da suportada ({module}). Este projeto suporta: '
                 f"{adapter['supported_version']}.")
    say(f"   Versão conferida: {adapter['supported_version']}")


@step('Traduzir o código do jogo para C — alguns segundos (uma janela vai abrir e fechar sozinha)')
def export():
    if not (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')):
        fail('Este passo precisa da área de trabalho (rode o instalador numa janela de terminal, não por SSH).')
    run([sys.executable, 'scripts/export-aot.py', '--backend', 'hybrid', '--unit-insns', '30000'], 'export-aot.log')


@step('Compilar a versão nativa do jogo — a parte demorada (1 a 3 horas)')
def package():
    budget = int((ram_gb() - 3) * 1024)
    say(f'   Usando até {budget // 1024} GB de RAM para compilar. Pode deixar o computador trabalhando.')
    run([sys.executable, 'scripts/package-standalone.py', '--jobs', str(jobs()), '--recomp-jobs', '12',
         '--mem-budget-mb', str(budget)], 'package-standalone.log')


@step('Extrair os dados do jogo (texturas, sons, textos) — uns 5 minutos')
def install():
    run([sys.executable, 'scripts/install-native.py'], 'install-native.log')


@step('Montar a pasta para jogar')
def bundle():
    run([sys.executable, 'scripts/assemble-package.py', '--name', 'tomodachi-native'], 'assemble-package.log')
    desktop = ROOT / 'local/package/tomodachi/tomodachi-native.desktop'
    apps = Path.home() / '.local/share/applications'
    if desktop.exists() and sys.stdin.isatty():
        answer = input('   Criar um atalho no menu de aplicativos? [S/n] ').strip().lower()
        if answer in ('', 's', 'sim', 'y', 'yes'):
            apps.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(desktop, apps / 'tomodachi-native.desktop')
            say('   Atalho criado: procure "Tomodachi" no menu.')


def main():
    STATE.mkdir(parents=True, exist_ok=True)
    say(f'{BOLD}Tomodachi Life: Living the Dream — instalador da versão nativa{RESET}')
    say('Projeto educacional e experimental. Use apenas com a sua própria cópia do jogo.\n')
    started = time.time()
    for i, (key, title, fn) in enumerate(STEPS, 1):
        marker = STATE / f'{i:02d}-{key}.ok'
        if marker.exists():
            say(f'{GREEN}✓{RESET} {i}/{len(STEPS)} {title} (já feito)')
            continue
        say(f'{YELLOW}▶{RESET} {BOLD}{i}/{len(STEPS)} {title}{RESET}')
        t0 = time.time()
        fn()
        marker.write_text(time.strftime('%Y-%m-%d %H:%M:%S'))
        say(f'{GREEN}✓{RESET} pronto ({(time.time() - t0) / 60:.0f} min)\n')
    say(f'{GREEN}{BOLD}Tudo pronto!{RESET} ({(time.time() - started) / 60:.0f} min nesta execução)')
    say('Para jogar:  ./jogar.sh          (tela cheia: ./jogar.sh --fullscreen)')


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        say('\nInterrompido. Rode ./instalar.sh de novo para continuar de onde parou.')
        sys.exit(130)
