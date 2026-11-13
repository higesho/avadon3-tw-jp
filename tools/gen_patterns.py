"""Make the byte patterns the DLL uses to find its hook points in Avadon.exe.

usage:
    python -I gen_patterns.py Avadon.exe

For each code range below, absolute addresses and call/jump targets are replaced by
wildcards (they differ between builds); the rest must match. The script checks that
every pattern matches exactly where expected in this exe (first match after its
anchor for the "near" ones, unique in .text for the others), and prints the C table
for avadon_jp.c. Requires pefile and capstone.
"""
import re
import sys

import capstone
import pefile

# name, start VA, end VA, anchor (None = unique in .text, else name of the pattern it
# follows within 0x400 bytes)
RANGES = [
    ('draw_text', 0x488780, 0x488849, None),
    ('font_select', 0x48894a, 0x488977, 'draw_text'),
    ('draw_ascii', 0x48c070, 0x48c0a4, None),
    ('charmap', 0x48c121, 0x48c129, 'draw_ascii'),
    ('ascii_blit', 0x48c2c1, 0x48c316, 'draw_ascii'),
    ('bind_tex', 0x49cd40, 0x49cdad, None),
    ('str_width', 0x488600, 0x4886d2, None),
    ('named_str', 0x483dc8, 0x483ded, None),
    ('tok_class', 0x4727a5, 0x4727f6, None),
]


def main(argv):
    pe = pefile.PE(argv[0])
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
    t0 = text.VirtualAddress
    tdata = img[t0:t0 + text.Misc_VirtualSize]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    found = {}
    out = []
    for name, a, b, anchor in RANGES:
        code = img[a - base:b - base]
        mask = [True] * len(code)
        n = 0
        for ins in md.disasm(code, a):
            off = ins.address - a
            n = off + ins.size
            if ins.disp_size == 4 and ins.disp_offset and base <= (ins.disp & 0xffffffff) < base + 0x1000000:
                for k in range(4):
                    mask[off + ins.disp_offset + k] = False
            if ins.imm_size == 4 and ins.imm_offset:
                imm = ins.operands[-1].imm & 0xffffffff if ins.operands else 0
                is_branch = ins.group(capstone.CS_GRP_CALL) or ins.group(capstone.CS_GRP_JUMP)
                if is_branch or base <= imm < base + 0x1000000:
                    for k in range(4):
                        mask[off + ins.imm_offset + k] = False
        assert n == len(code), (name, n, len(code))
        rx = re.compile(b''.join(re.escape(bytes([c])) if m else b'.' for c, m in zip(code, mask)), re.S)
        if anchor:
            start = found[anchor] - base - t0
            m = rx.search(tdata, start, start + 0x400)
            hits = [m.start()] if m else []
        else:
            hits = [m.start() for m in rx.finditer(tdata)]
        where = [h + t0 + base for h in hits]
        ok = where == [a]
        print('%-12s %06x len %3d hits %s %s' % (name, a, len(code), ' '.join('%06x' % w for w in where), 'OK' if ok else 'BAD'))
        if not ok:
            return 1
        found[name] = a
        pat = ' '.join('%02X' % c if m else '??' for c, m in zip(code, mask))
        out.append('    {"%s", %s, 0x%06x,\n     "%s"},' % (name, ('"%s"' % anchor) if anchor else 'NULL', a, pat))
    print()
    print('\n'.join(out))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
