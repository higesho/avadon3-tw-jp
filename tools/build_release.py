"""Assemble the release ZIP (drop-in layout: copy the contents into the game folder).

usage:
    python -I build_release.py ORIGINAL_ROOT GAME_ROOT OUT_DIR VERSION PAGE_URL

ORIGINAL_ROOT: a game-folder-shaped copy of the untouched files ("Avadon 3 Files/...").
GAME_ROOT:     a game-folder-shaped tree with the Japanese files built in.
The DLL, ini and exe-string table are taken from this mod folder.
Writes OUT_DIR/Avadon3_JP_<VERSION>.zip with
    dsound.dll, avadon3_jp.ini, avadon3_jp/jp_patch.bin, avadon3_jp/avadon3_jp_exe.tsv,
    avadon3_jp/README.txt
"""
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(HERE)
SRC = os.path.join(MOD, 'dist_src')


def main(argv):
    if len(argv) != 5:
        print(__doc__)
        return 2
    orig, game, out, version, page = argv
    name = 'Avadon3_JP_' + version
    stage = os.path.join(out, name)
    shutil.rmtree(stage, ignore_errors=True)
    data = os.path.join(stage, 'avadon3_jp')
    os.makedirs(data)
    subprocess.check_call([sys.executable, '-I', os.path.join(HERE, 'make_patch.py'), orig, game,
                           os.path.join(data, 'jp_patch.bin')])
    shutil.copy2(os.path.join(MOD, 'dsound.dll'), os.path.join(stage, 'dsound.dll'))
    shutil.copy2(os.path.join(SRC, 'avadon3_jp.ini'), os.path.join(stage, 'avadon3_jp.ini'))
    shutil.copy2(os.path.join(MOD, 'exe_strings.tsv'), os.path.join(data, 'avadon3_jp_exe.tsv'))
    t = open(os.path.join(SRC, 'README.txt'), encoding='utf-8').read()
    t = t.replace('__VERSION__', version).replace('__PAGE_URL__', page).replace('\r\n', '\n').replace('\n', '\r\n')
    with open(os.path.join(data, 'README.txt'), 'w', encoding='utf-8-sig', newline='') as fh:
        fh.write(t)

    zpath = os.path.join(out, name + '.zip')
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for dirpath, _, files in os.walk(stage):
            for fn in sorted(files):
                p = os.path.join(dirpath, fn)
                z.write(p, os.path.relpath(p, stage).replace('\\', '/'))   # contents at the ZIP root
    print('wrote', zpath, os.path.getsize(zpath), 'bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
