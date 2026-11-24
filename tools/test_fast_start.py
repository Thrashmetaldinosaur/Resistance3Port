#!/usr/bin/env python3
"""Source-only regression guards for the opt-in R3 frontend startup modes.

These checks do not run the game and are not a substitute for an in-game test.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = (ROOT / 'project/r3_quickresume.inc').read_text(encoding='utf-8')
EXTRA = (ROOT / 'project/r3_pcextra.cpp').read_text(encoding='utf-8')
LAUNCHER = (ROOT / 'project/launcher.cpp').read_text(encoding='utf-8')
CATS = (ROOT / 'project/launcher_cats.inc').read_text(encoding='utf-8')


def test_both_modes_explicitly_opt_in():
    assert 'envi("FAST_START", 0)' in SRC
    assert 'envi("QUICK_RESUME", 0)' in SRC
    assert 'R3_FAST_START' in LAUNCHER and 'R3_QUICK_RESUME' in LAUNCHER
    assert 'faststart quickresume' in CATS
    assert 'Off  (normal title screen)' in LAUNCHER
    assert 'Off  (normal startup)' in LAUNCHER


def test_no_save_does_not_force_continue():
    assert 'if (want_resume && r3_quickresume_has_checkpoint()) mode = 2' in SRC
    assert 'else if (want_menu) mode = 1' in SRC
    assert 'if (want_resume) LOG("quick resume: no checkpoint; normal startup")' in SRC
    assert 'GAME.SAV' in SRC and 'PARAM.SFO' in SRC
    assert 'GetFileAttributesExA' in SRC
    assert 'vm_write' not in SRC and 'cellSaveData' not in SRC


def test_campaign_menu_disarms_before_continue():
    marker = 'if (mode == 1) {'
    assert SRC.count(marker) == 1
    tail = SRC.split(marker, 1)[1]
    stop = tail.split('r3_quickresume_inject(0x4000u, "continue saved campaign"', 1)[0]
    assert 'state = 5;' in stop and 'return;' in stop


def test_bounded_startup_and_excludes_benchmark():
    assert 'if (s_bench) { state = -1; return; }' in SRC
    assert 't >= 22.0' in SRC and 'boots < 25' in SRC
    assert 't - last >= 2.0' in SRC
    assert 'if (level == "lobby")' in SRC
    assert 'r3_quickresume_tick(hwnd);' in EXTRA
    assert '#include "r3_quickresume.inc"' in EXTRA


if __name__ == '__main__':
    for name, fn in sorted(globals().copy().items()):
        if name.startswith('test_') and callable(fn):
            fn()
            print(f'PASS {name}')
