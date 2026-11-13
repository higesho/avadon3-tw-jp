"""Group translation chunks into jobs of about JOB_CHARS and schedule them into batches.

usage:
    python -I make_jobs.py TRANS_DIR [--job-chars 30000] [--batch 8] [--first JOB ...]

Reads TRANS_DIR/chunks/*.json (made by make_chunks.py), writes TRANS_DIR/jobs.json:
    [{"name": "j00", "chunks": [...], "files": [...], "chars": n, "batch": b}, ...]
Chunks are taken in file order; a job may end inside a big file (at a chunk boundary).
Batches run in parallel, so the continuation of a file always goes to a later batch
than the job before it (voice notes and terms are handed on between batches).
"""
import json
import os
import re
import sys


def main(argv):
    job_chars, batch_size = 30000, 8
    if '--job-chars' in argv:
        job_chars = int(argv[argv.index('--job-chars') + 1])
    if '--batch' in argv:
        batch_size = int(argv[argv.index('--batch') + 1])
    tdir = argv[0]
    names = sorted(f[:-5] for f in os.listdir(os.path.join(tdir, 'chunks')) if f.endswith('.json'))
    names.sort(key=lambda n: (n.rsplit('__', 1)[0], int(n.rsplit('__', 1)[1])))
    sizes = {}
    for n in names:
        c = json.load(open(os.path.join(tdir, 'chunks', n + '.json'), encoding='utf-8'))
        sizes[n] = sum(len(u['en']) for u in c['units'])
    fsize = {}
    for n, k in sizes.items():
        f = n.rsplit('__', 1)[0]
        fsize[f] = fsize.get(f, 0) + k
    jobs, cur, size = [], [], 0
    for n in names:
        k = sizes[n]
        # a big file starts a new job, so that its jobs only depend on each other
        big_start = n.endswith('__01') and fsize[n.rsplit('__', 1)[0]] >= 8000
        if cur and (size + k > job_chars * 1.15 or (big_start and size > job_chars * 0.1)):
            jobs.append((cur, size))
            cur, size = [], 0
        cur.append(n)
        size += k
    if cur:
        jobs.append((cur, size))
    out = []
    for i, (ch, sz) in enumerate(jobs):
        files = list(dict.fromkeys(c.rsplit('__', 1)[0] for c in ch))
        out.append({'name': 'j%02d' % i, 'chunks': ch, 'files': files, 'chars': sz, 'batch': None})
    # A job "continues" the previous job when its first chunk is not part 01 of its file.
    def cont(j):
        return not j['chunks'][0].endswith('__01')
    batch = 0
    left = list(range(len(out)))
    while left:
        taken = []
        for i in list(left):
            if len(taken) >= batch_size:
                break
            if cont(out[i]) and i - 1 in left:
                continue  # its predecessor has not been scheduled yet
            if cont(out[i]) and i - 1 in taken:
                continue  # same batch as its predecessor: wait
            taken.append(i)
        for i in taken:
            out[i]['batch'] = batch
            left.remove(i)
        batch += 1
    with open(os.path.join(tdir, 'jobs.json'), 'w', encoding='utf-8') as f:
        json.dump(out, f, ensure_ascii=False, indent=0)
    for b in range(batch):
        js = [j for j in out if j['batch'] == b]
        print('batch %d: %s' % (b, ', '.join('%s(%dk %s)' % (j['name'], j['chars'] // 1000, j['files'][0]) for j in js)))
    print(f'{len(out)} jobs, {batch} batches')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
