"""Count instruction -> flag-consumer pairs in the game's code, by operand width.
Output: pairs.json {"mnem/width -> consumer": count} for prioritising lifter bugs."""
import json, sys, collections, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_REG, X86_OP_MEM, X86_OP_IMM
xbe = open('/home/claude/pcj-native/game/default.xbe', 'rb').read()
base = struct.unpack_from('<I', xbe, 0x104)
nsec = struct.unpack_from('<I', xbe, 0x11C)[0]; shdr = struct.unpack_from('<I', xbe, 0x120)[0] - base[0]
secs = []
for i in range(nsec):
    fl, va, vs, raw, rs = struct.unpack_from('<IIIII', xbe, shdr + i * 56)
    secs.append((va, vs, raw, rs))
def read(va, n):
    for sva, vs, raw, rs in secs:
        if sva <= va < sva + rs:
            o = va - sva; return xbe[raw + o: raw + min(rs, o + n)]
    return b''
fns = json.load(open('/home/claude/pcj-native/work/disasm/functions.json'))
cs = Cs(CS_ARCH_X86, CS_MODE_32); cs.detail = True
FLAGCONS = lambda m: m.startswith('j') and m not in ('jmp', 'jecxz', 'jcxz') or m.startswith('set') or m.startswith('cmov') or m in ('adc', 'sbb', 'rcl', 'rcr', 'pushfd', 'lahf')
cnt = collections.Counter(); single = collections.Counter()
for f in fns:
    s, e = int(f['start'], 16), int(f['end'], 16)
    code = read(s, e - s)
    ins = list(cs.disasm(code, s))
    for a, b in zip(ins, ins[1:]):
        w = a.operands[0].size * 8 if a.operands else 0
        single[f'{a.mnemonic}/{w}/{len(a.operands)}'] += 1
        if FLAGCONS(b.mnemonic):
            cnt[f'{a.mnemonic}/{w} -> {b.mnemonic}'] += 1
json.dump({'pairs': cnt.most_common(), 'single': single.most_common()}, open('/home/claude/pcj-native/tools/difftriage/pairs.json', 'w'), indent=0)
for k, v in cnt.most_common(60): print(v, k)
