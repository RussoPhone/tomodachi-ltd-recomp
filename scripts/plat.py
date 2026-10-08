"""Platform helpers shared by the installer scripts (Linux and Windows)."""
import os
from pathlib import Path
import sys

IS_WINDOWS = sys.platform == 'win32'
EXE = '.exe' if IS_WINDOWS else ''


def _memory_status():
    import ctypes

    class MEMORYSTATUSEX(ctypes.Structure):
        _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong),
                    ('ullTotalPhys', ctypes.c_ulonglong), ('ullAvailPhys', ctypes.c_ulonglong),
                    ('ullTotalPageFile', ctypes.c_ulonglong), ('ullAvailPageFile', ctypes.c_ulonglong),
                    ('ullTotalVirtual', ctypes.c_ulonglong), ('ullAvailVirtual', ctypes.c_ulonglong),
                    ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]

    status = MEMORYSTATUSEX()
    status.dwLength = ctypes.sizeof(status)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status))
    return status


def _meminfo(field):
    with open('/proc/meminfo') as f:
        for line in f:
            if line.startswith(field + ':'):
                return int(line.split()[1]) // 1024
    return 0


def total_ram_mb():
    return _memory_status().ullTotalPhys // 2**20 if IS_WINDOWS else _meminfo('MemTotal')


def available_ram_mb():
    return _memory_status().ullAvailPhys // 2**20 if IS_WINDOWS else _meminfo('MemAvailable')


def swap_mb():
    """Linux swap, or on Windows what the page file adds on top of RAM (the commit limit minus RAM)."""
    if IS_WINDOWS:
        status = _memory_status()
        return max(0, status.ullTotalPageFile - status.ullTotalPhys) // 2**20
    return _meminfo('SwapTotal')


def pid_alive(pid):
    if IS_WINDOWS:
        import ctypes
        handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, int(pid))  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return False
        code = ctypes.c_ulong()
        ctypes.windll.kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
        ctypes.windll.kernel32.CloseHandle(handle)
        return code.value == 259  # STILL_ACTIVE
    return os.path.exists(f'/proc/{pid}')


def _is_junction(path):
    """os.path.isjunction exists only from Python 3.12; read the reparse-point attribute instead."""
    import stat
    try:
        st = os.lstat(path)
    except OSError:
        return False
    return bool(getattr(st, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT) and not os.path.islink(path)


def link_dir(link, target):
    """Make `link` point at the directory `target` (a junction on Windows: no admin rights needed)."""
    link, target = Path(link), Path(target)
    target.mkdir(parents=True, exist_ok=True)
    junction = IS_WINDOWS and _is_junction(link)
    if link.is_symlink() or junction:
        if Path(os.path.realpath(link)) == target.resolve():
            return
        os.rmdir(link) if junction else os.unlink(link)
    elif link.exists():
        raise RuntimeError(f'{link} exists and is a real folder; move it away first')
    link.parent.mkdir(parents=True, exist_ok=True)
    if IS_WINDOWS:
        import _winapi
        _winapi.CreateJunction(str(target.resolve()), str(link))
    else:
        link.symlink_to(target.resolve(), target_is_directory=True)


def git_env(base=None):
    """Environment for git: LF line endings (the patches are LF) and long paths on Windows."""
    env = dict(base if base is not None else os.environ)
    settings = [('core.autocrlf', 'false')] + ([('core.longpaths', 'true')] if IS_WINDOWS else [])
    start = int(env.get('GIT_CONFIG_COUNT', '0') or 0)
    for i, (key, value) in enumerate(settings, start):
        env[f'GIT_CONFIG_KEY_{i}'], env[f'GIT_CONFIG_VALUE_{i}'] = key, value
    env['GIT_CONFIG_COUNT'] = str(start + len(settings))
    return env
