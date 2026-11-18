"""Print the exception and a heuristic call stack from a 32-bit Windows minidump.
usage: python dumpstack.py <names.txt> <file.dmp>
"""
import struct, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from flame import Symbolizer


def streams(d):
    sig, ver, n, rva = struct.unpack_from('<4sIII', d, 0)
    assert sig == b'MDMP'
    out = {}
    for i in range(n):
        t, size, r = struct.unpack_from('<III', d, rva + 12 * i)
        out.setdefault(t, []).append((size, r))
    return out


def mdstring(d, rva):
    n = struct.unpack_from('<I', d, rva)[0]
    return d[rva + 4: rva + 4 + n].decode('utf-16-le', 'replace')


def main():
    names, path = sys.argv[1], sys.argv[2]
    d = open(path, 'rb').read()
    st = streams(d)
    mods = []
    for size, r in st.get(4, []):
        n = struct.unpack_from('<I', d, r)[0]
        for i in range(n):
            o = r + 4 + i * 108
            base = struct.unpack_from('<Q', d, o)[0]; sz = struct.unpack_from('<I', d, o + 8)[0]
            nm = struct.unpack_from('<I', d, o + 20)[0]
            mods.append((base, sz, mdstring(d, nm)))
    mem = []
    for size, r in st.get(5, []):
        n = struct.unpack_from('<I', d, r)[0]
        for i in range(n):
            start, dsz, drva = struct.unpack_from('<QII', d, r + 4 + i * 16)
            mem.append((start, dsz, drva))
    for size, r in st.get(9, []):
        n, base_rva = struct.unpack_from('<QQ', d, r)
        cur = base_rva
        for i in range(n):
            start, dsz = struct.unpack_from('<QQ', d, r + 16 + i * 16)
            mem.append((start, dsz, cur)); cur += dsz

    def read(addr, n):
        for s, sz, rv in mem:
            if s <= addr and addr + n <= s + sz:
                o = rv + addr - s
                return d[o:o + n]
        return None

    sym = Symbolizer(names, [(b, s, p) for b, s, p in mods])
    size, r = st[6][0]
    tid, _, code, flags, rec, addr = struct.unpack_from('<IIIIQQ', d, r)
    ctx_size, ctx_rva = struct.unpack_from('<II', d, r + 8 + 152)
    ebp, eip = struct.unpack_from('<II', d, ctx_rva + 0xB4)
    esp = struct.unpack_from('<I', d, ctx_rva + 0xC4)[0]
    regs = dict(zip(['edi', 'esi', 'ebx', 'edx', 'ecx', 'eax'], struct.unpack_from('<6I', d, ctx_rva + 0x9C)))
    print('thread %d  exception %08X at %08X  %s' % (tid, code, addr, sym.resolve(addr)[1]))
    print('eip=%08X esp=%08X ebp=%08X  ' % (eip, esp, ebp) + ' '.join('%s=%08X' % kv for kv in regs.items()))
    stack = b''
    for s0, sz, rv in mem:
        if s0 <= esp < s0 + sz:
            n = min(s0 + sz - esp, 0x8000); stack = d[rv + esp - s0: rv + esp - s0 + n]
    print('stack bytes captured: %d' % len(stack))
    print('--- candidate return addresses (stack scan)')
    shown = 0
    for off in range(0, len(stack) - 3, 4):
        v = struct.unpack_from('<I', stack, off)[0]
        b, p = sym.module(v)
        if b is None or not (0x400000 <= v < 0x7D0000 or 'ZFASTSAVE' in p.upper() or 'UNION' in p.upper()):
            continue
        code5 = read(v - 7, 7)
        if not code5:
            print('  [esp+%04X] %08X  %s   (unverified)' % (off, v, sym.resolve(v)[1])); shown += 1
            if shown > 60: break
            continue
        direct = code5[2] == 0xE8
        indirect = any(code5[7 - k] == 0xFF and (code5[7 - k + 1] & 0x38) == 0x10 for k in (2, 3, 6) if 7 - k + 1 < 7)
        if not (direct or indirect):
            continue
        tgt = ''
        if direct:
            t = (v + struct.unpack_from('<i', code5, 3)[0]) & 0xFFFFFFFF
            tgt = '  -> ' + sym.resolve(t)[1]
        print('  [esp+%04X] %08X  %s%s' % (off, v, sym.resolve(v)[1], tgt))
        shown += 1
        if shown > 45:
            break


if __name__ == '__main__':
    main()
