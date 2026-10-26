"""Compare eagl_slots.txt (HackiOS, iOS 16.5) with this OpenGLES disassembly:
each gl* function jumps through [ctx + off]; its slot is off - 0x20."""
import re, sys
dis, slots = sys.argv[1:3]
funcs, cur = {}, None
for line in open(dis):
    m = re.match(r'^[0-9a-f]+ <(_\w+)>:', line)
    if m:
        cur = m.group(1)[1:]; funcs[cur] = []; continue
    if cur:
        funcs[cur].append(line.strip())
found = {}
for name, body in funcs.items():
    loads, ctx = {}, None
    for ins in body:
        m = re.search(r'ldr\s+(x\d+), \[(x\d+), #0xf0\]', ins)
        if m and ctx is None:
            ctx = m.group(1)
            continue
        m = re.search(r'ldr\s+(x\d+), \[(x\d+), #(0x[0-9a-f]+)\]', ins)
        if m and m.group(2) == ctx:
            loads[m.group(1)] = int(m.group(3), 16)
        m = re.search(r'\bb(?:l)?r\s+(x\d+)', ins)
        if m and m.group(1) in loads:
            found[name] = loads[m.group(1)] - 0x20
            break
want = {}
for line in open(slots):
    f = line.split('#')[0].split()
    if f:
        for n in f[2:]:
            want[n] = int(f[0], 16)
same = [n for n in want if found.get(n) == want[n]]
diff = [(n, want[n], found[n]) for n in want if n in found and found[n] != want[n]]
missing = [n for n in want if n not in found]
extra = sorted(n for n in found if n.startswith('gl') and n not in want)
print(f'slots file names: {len(want)}, found in OpenGLES: {len(found)}')
print(f'same offset: {len(same)}; different: {len(diff)}; not found here: {len(missing)}')
for n, w, f in diff: print(f'  DIFF {n}: 16.5 {w:#06x}, here {f:#06x}')
print('  not found:', ' '.join(missing[:40]))
print(f'gl functions here that the slots file does not name: {len(extra)}:', ' '.join(extra[:30]))
