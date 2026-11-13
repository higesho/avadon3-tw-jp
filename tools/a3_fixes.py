"""One-off consistency fixes found while reviewing the translators' reports (Avadon 3).

usage:
    python -I a3_fixes.py TRANS_DIR [--dry-run]

Each rule applies to units whose English matches `en` (regex, or None = every unit)
and replaces `old` with `new` in the Japanese. Run before polish.py / unify_dupes.py.
"""
import json
import os
import re
import sys

sys.stdout.reconfigure(encoding='utf-8')

RULES = [
    # (English regex or None, old, new)
    (None, '四つの輪', '四つの環'),
    (None, 'アンティティアの遺跡', 'アンティティア遺跡'),
    (None, '『フォーク』', '『民』'),
    (None, 'よそ者', '余所者'),
    (None, '指揮官の広間', '司令官の広間'),
    (None, 'ネバネバ虫', 'ネバリ虫'),
    (None, '手首射出器', '腕投げ器'),
    (None, '製作長', '工匠長'),
    (None, 'ホルクランドの', 'ホルクランダの'),
    (None, '番人のプロタス（代理）', '番人代理のプロタス'),
    (r'\bSlow\b', '「減速」', '「鈍足」'),
    (r'Haste is necessary', '加速が必要だ', '急がねばならない'),
    (r'[Pp]rotector', '守護者', '守護官'),
    (r'[Aa]colyte', '侍者', '侍祭'),
    (r'[Aa]colyte', '見習い', '侍祭'),
    (r'[Ss]entinel', '番人', '番兵'),
    (r'[Ii]mperator', '帝国の高官', '統領'),
]


def main(argv):
    dry = '--dry-run' in argv
    tdir = [a for a in argv if not a.startswith('--')][0]
    total = 0
    for fn in sorted(os.listdir(os.path.join(tdir, 'ja'))):
        if not fn.endswith('.json'):
            continue
        name = fn[:-5]
        p = os.path.join(tdir, 'ja', fn)
        out = json.load(open(p, encoding='utf-8'))
        src = {str(u['id']): u['en'] for u in
               json.load(open(os.path.join(tdir, 'chunks', name + '.json'), encoding='utf-8'))['units']}
        changed = False
        for k, en in src.items():
            ja = out.get(k)
            if not isinstance(ja, str):
                continue
            new = ja
            for pat, old, rep in RULES:
                if (pat is None or re.search(pat, en)) and old in new:
                    new = new.replace(old, rep)
            if new != ja:
                print(f'{name} {k}: {ja[:50]} -> {new[:50]}')
                out[k] = new
                changed = True
                total += 1
        if changed and not dry:
            json.dump(out, open(p, 'w', encoding='utf-8'), ensure_ascii=False, indent=0)
    print(f'{total} units changed' + (' (dry run)' if dry else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
