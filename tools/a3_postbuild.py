"""Fix-ups applied to the built Japanese scripts after build_scripts.py (Avadon 3).

usage:
    python -I a3_postbuild.py BUILT_SCRIPTS_DIR

z2vanareachdlg.txt: one original string contains a raw line break
("...this tower. <CR><TAB>When you get close, Trupo says, _..._"). The game reads it
as a single literal, but extract.py ends the literal at the line break, so the part
after it was never translated. Replace that English tail (with the line break) by the
Japanese text.
"""
import os
import sys

FIXES = [
    ('z2vanareachdlg.txt',
     b"\r\tWhen you get close, Trupo says, _I wouldn't dream of telling you what to do, Hand, "
     b"but I'd appreciate it if you left our quarters alone._",
     '近づくと、トルポが言う。「あなたに指図する気はないが、ハンド、我々の居室には立ち入らないでもらえるとありがたい。」'),
]


def main(argv):
    if len(argv) != 1:
        print(__doc__)
        return 2
    for fn, old, new in FIXES:
        p = os.path.join(argv[0], fn)
        d = open(p, 'rb').read()
        if d.count(old) != 1:
            print(f'{fn}: expected text not found once ({d.count(old)}), skipped')
            return 1
        d = d.replace(old, new.encode('euc_jp'))
        open(p, 'wb').write(d)
        print(f'{fn}: fixed')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
