"""Describe how Avadon 3.exe assembles messages from string fragments (for the translators).

usage:
    python -I exe_fragctx.py "Avadon 3.exe" EXE_STRINGS_TSV OUT_JSON

The message helpers 0x4884c0 / 0x4884f0 (cdecl a, b, c, d, n, e, ...) print
    a + b + c + d + (n >= 0 ? number(n) : "") + e
(sprintf "%s%s%s%s%d%s"). For every call, the pushed arguments are resolved to string
literals where possible (also through a register loaded just before the push), giving e.g.
"You get " + <x> + " coins." For other calls (sprintf etc.) the string literals pushed
before the call are listed. Writes {va: ["description", ...]} for every string va in the
tsv that takes part.
"""
import json
import re
import sys

import capstone
import pefile

sys.stdout.reconfigure(encoding='utf-8')
CONCAT = {0x4884c0: 'message', 0x4884f0: 'message'}
REGS32 = {'ecx', 'edx', 'esi', 'ebx', 'eax', 'edi'}


def main(argv):
    exe, tsv, out = argv
    rows = [l.split('\t') for l in open(tsv, encoding='utf-8').read().split('\n')[1:] if l.strip()]
    strings = {int(r[0], 16): r[3] for r in rows}
    pe = pefile.PE(exe, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
    t0 = base + text.VirtualAddress
    code = img[text.VirtualAddress:text.VirtualAddress + text.Misc_VirtualSize]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    insns, off = [], 0
    while off < len(code):
        got = False
        for ins in md.disasm(code[off:off + 32], t0 + off, 1):
            insns.append(ins)
            off += ins.size
            got = True
        if not got:
            off += 1

    def lit(v):
        if v in strings:
            return "'%s'" % strings[v]
        if v is not None and 0x5c8920 <= v < 0x5d569c:  # format strings and other game texts not in the tsv
            o = v - base
            b = img[o:img.find(b'\0', o, o + 200)]
            if b and all(32 <= c < 127 for c in b):
                return "'%s'" % b.decode('ascii')
            if not b:
                return "''"
        return None

    ctx = {}

    def add(va, desc):
        lst = ctx.setdefault('0x%06x' % va, [])
        if desc not in lst and len(lst) < 6:
            lst.append(desc)

    for i, ins in enumerate(insns):
        if ins.mnemonic != 'call' or ins.operands[0].type != capstone.x86.X86_OP_IMM:
            continue
        tgt = ins.operands[0].imm & 0xffffffff
        start = i
        for j in range(i - 1, max(i - 40, 0), -1):
            c = insns[j]
            if c.mnemonic in ('call', 'ret', 'jmp') or c.mnemonic.startswith('j'):
                break
            start = j
        regs, pushes, used = {}, [], []
        for c in insns[start:i]:
            ops = c.operands
            if c.mnemonic == 'push':
                if ops[0].type == capstone.x86.X86_OP_IMM:
                    v = ops[0].imm & 0xffffffff
                    if v in strings:
                        used.append(v)
                    pushes.append(lit(v) or ('<-1>' if v == 0xffffffff else '<%s>' % c.op_str))
                elif ops[0].type == capstone.x86.X86_OP_REG:
                    r = c.reg_name(ops[0].reg)
                    val = regs.get(r, '<%s>' % r)
                    if isinstance(val, tuple):
                        used.append(val[1])
                        val = val[0]
                    pushes.append(val)
                else:
                    pushes.append('<%s>' % c.op_str)
            elif c.mnemonic in ('mov', 'movsx', 'movzx', 'lea') and len(ops) == 2 and ops[0].type == capstone.x86.X86_OP_REG:
                r = c.reg_name(ops[0].reg)
                if c.mnemonic == 'mov' and ops[1].type == capstone.x86.X86_OP_IMM:
                    v = ops[1].imm & 0xffffffff
                    regs[r] = (lit(v), v) if v in strings else (lit(v) or '<%s>' % c.op_str.split(', ', 1)[1])
                elif c.mnemonic == 'lea':
                    regs[r] = '<buffer>'
                else:
                    regs[r] = '<%s>' % c.op_str.split(', ', 1)[1]
            elif len(ops) >= 1 and ops[0].type == capstone.x86.X86_OP_REG:
                regs[c.reg_name(ops[0].reg)] = '<%s %s>' % (c.mnemonic, c.op_str)
        args = list(reversed(pushes))
        if not used:
            continue
        if tgt in CONCAT and len(args) >= 6:
            parts = args[:4]
            if args[4] != '<-1>':
                parts.append('<number>')
            parts.append(args[5])
            parts = [p for p in parts if p not in ("''",)]
            desc = 'message: ' + ' + '.join(parts)
        else:
            desc = 'call %06x(%s)' % (tgt, ', '.join(args))
        for v in used:
            add(v, desc)
    json.dump(ctx, open(out, 'w', encoding='utf-8'), ensure_ascii=False, indent=0)
    print(len(ctx), 'strings with context')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
