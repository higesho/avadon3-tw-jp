"""Show which translation jobs are done and which can start next.

usage:
    python -I job_status.py TRANS_DIR [RUNNING_JOB ...]

A job is done when every chunk has TRANS_DIR/ja/<chunk>.json. A job that continues a
file (its first chunk is not __01) is ready only when the job before it is done.
Jobs given as RUNNING_JOB are listed as running, not ready.
"""
import json
import os
import sys


def main(argv):
    tdir, running = argv[0], set(argv[1:])
    jobs = json.load(open(os.path.join(tdir, 'jobs.json'), encoding='utf-8'))
    done = [j['name'] for j in jobs if all(os.path.exists(os.path.join(tdir, 'ja', c + '.json')) for c in j['chunks'])]
    ready = []
    for i, j in enumerate(jobs):
        if j['name'] in done or j['name'] in running:
            continue
        if not j['chunks'][0].endswith('__01') and i > 0 and jobs[i - 1]['name'] not in done:
            continue
        ready.append(j['name'])
    chars = sum(j['chars'] for j in jobs)
    dchars = sum(j['chars'] for j in jobs if j['name'] in done)
    print(f'done {len(done)}/{len(jobs)} jobs ({dchars * 100 // chars}% of text)')
    print('running:', ' '.join(sorted(running)))
    print('ready  :', ' '.join(ready))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
