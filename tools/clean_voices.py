"""Drop voice notes keyed by generic roles (class names, races, unnamed speakers) from TRANS_DIR/voices.json.

usage:
    python -I clean_voices.py TRANS_DIR

prep_job.py matches the English part of a key against the chunk text, so a key such as
"機工術師（Tinkermage）" would be handed to every job that mentions tinkermages.
"""
import json
import os
import re
import sys

sys.stdout.reconfigure(encoding='utf-8')
GENERIC = {'Tinkermage', 'Shaman', 'Sorcerer', 'Blademaster', 'Shadowwalker', 'Overseer', 'Speaker', 'Chief',
           'Hand', 'Eye', 'Keeper', 'Wretch', 'Wretches', 'Ogre', 'Titan', 'Titans', 'Warborn', 'Infernal',
           'Infernals', 'Tawon', 'Dharam', 'Svorgaldi', 'Horror', 'Shade', 'Guard', 'Guards', 'Soldier',
           'Soldiers', 'Villagers', 'Envoy', 'Ghost', 'Drake'}


def main(argv):
    p = os.path.join(argv[0], 'voices.json')
    v = json.load(open(p, encoding='utf-8'))
    drop = []
    for k in list(v):
        m = re.search(r'[（(]([^）)]*)[）)]', k)
        en = (m.group(1) if m else k).split('、')[0].strip()
        if (en in GENERIC or not re.match(r'^[A-Z]', en)
                or en.lower().startswith(('the ', 'a ', 'an ', 'unnamed', 'nameless'))):
            drop.append(k)
            del v[k]
    json.dump(v, open(p, 'w', encoding='utf-8'), ensure_ascii=False, indent=0)
    print(f'{len(v)} voices kept, {len(drop)} generic dropped')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
