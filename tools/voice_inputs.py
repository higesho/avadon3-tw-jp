"""Collect sample quotes per speaking character for the voice table (trans/voices_in/*.json)."""
import collections
import glob
import json
import os
import re

cands = json.load(open('trans/glossary_candidates.json', encoding='utf-8'))
names = {c['en'] for c in cands if re.match(r"^[A-Z][a-z'\-]+$", c['en'])}
skip = {'Avadon', 'Hand', 'Hands', 'Keeper', 'Pact', 'Eye', 'Tawon', 'Kellem', 'Goldcrag', 'Warborn', 'Holklanda',
        'Wyldrylm', 'Farlanders', 'God', 'Refuge', 'Khemerian', 'Stead', 'Dharam', 'Hall', 'Dharamite', 'Chief', 'Mine',
        'Woodhome', 'Corruption', 'Dharamites', 'Spiral', 'Holklandan', 'Bloodwave'}
cnt = collections.Counter()
samples = collections.defaultdict(list)
files = collections.defaultdict(set)
for f in sorted(glob.glob('trans/units/*.json')):
    fn = os.path.basename(f)[:-5]
    for u in json.load(open(f, encoding='utf-8')):
        if u['kind'] not in ('text', 'lib'):
            continue
        s = u['en']
        for m in re.finditer(r'_([^_]{8,})_', s):
            pre = s[max(0, m.start() - 200):m.start()]
            ws = [w for w in re.findall(r"\b[A-Z][a-z'\-]+\b", pre) if w in names and w not in skip]
            if ws:
                n = ws[-1]
                cnt[n] += 1
                files[n].add(fn)
                if len(samples[n]) < 12:
                    samples[n].append({'before': pre, 'quote': m.group(1)[:260]})
a1 = json.load(open('ref_av1/voices_av1.json', encoding='utf-8'))


def ek(k):
    m = re.search(r'[（(]([^）)]*)[）)]', k)
    return (m.group(1) if m else k).split('、')[0].strip()


a1e = {}
for k, v in a1.items():
    for w in re.findall(r"[A-Z][a-z'\-]+", ek(k)):
        a1e.setdefault(w, (k, v))
items = []
for n, c in cnt.most_common():
    if c < 2:
        continue
    d = {'en': n, 'quotes': c, 'files': sorted(files[n])[:8], 'samples': samples[n]}
    if n in a1e:
        d['av1_voice'] = {a1e[n][0]: a1e[n][1]}
    items.append(d)
for k in range(2):
    json.dump(items[k::2], open('trans/voices_in/part%d.json' % (k + 1), 'w', encoding='utf-8'), ensure_ascii=False, indent=0)
print(len(items), [i['en'] for i in items if 'av1_voice' in i])
