"""Symbolize zFastSaveLoad sampler dumps and render flamegraphs.

usage: python flame.py <names.txt> <out_dir> <dump.txt> [dump.txt ...]
Dumps with the same label prefix (LOAD / SAVE / STARTUP / CHANGELEVEL) are merged.
Writes <out_dir>/<group>.folded, <group>.html and prints a top-N self/inclusive table.
"""
import bisect, collections, html, json, os, re, struct, sys

CALLCONV = re.compile(r'(?:__thiscall|__cdecl|__stdcall|__fastcall)\s+(.+?)\(')


def load_names(path):
    addrs, names = [], []
    for line in open(path, encoding='latin-1'):
        if not line.startswith('0x'):
            continue
        a, _, decl = line.rstrip('\n').partition(' ')
        m = CALLCONV.search(decl)
        if not m:
            continue
        name = m.group(1).strip()
        name = name.replace('`vector deleting destructor\'', '~vdtor').replace('`scalar deleting destructor\'', '~sdtor')
        addrs.append(int(a, 16)); names.append(name)
    order = sorted(range(len(addrs)), key=addrs.__getitem__)
    return [addrs[i] for i in order], [names[i] for i in order]


def pe_exports(path):
    try:
        data = open(path, 'rb').read()
    except OSError:
        return [], []
    try:
        pe = struct.unpack_from('<I', data, 0x3C)[0]
        nsec = struct.unpack_from('<H', data, pe + 6)[0]
        optsz = struct.unpack_from('<H', data, pe + 20)[0]
        opt = pe + 24
        exp_rva = struct.unpack_from('<I', data, opt + 96)[0]
        secs = []
        for i in range(nsec):
            o = opt + optsz + i * 40
            va, vs, raw = struct.unpack_from('<III', data, o + 12)[0], struct.unpack_from('<I', data, o + 8)[0], struct.unpack_from('<I', data, o + 20)[0]
            secs.append((va, vs, raw))

        def off(rva):
            for va, vs, raw in secs:
                if va <= rva < va + max(vs, 1):
                    return rva - va + raw
            raise ValueError
        if not exp_rva:
            return [], []
        e = off(exp_rva)
        nfun, nnames, afun, aname, aord = struct.unpack_from('<IIIII', data, e + 20)
        funs = [struct.unpack_from('<I', data, off(afun) + 4 * i)[0] for i in range(nfun)]
        res = {}
        for i in range(nnames):
            nrva = struct.unpack_from('<I', data, off(aname) + 4 * i)[0]
            ordi = struct.unpack_from('<H', data, off(aord) + 2 * i)[0]
            n = data[off(nrva):data.index(b'\0', off(nrva))].decode('latin-1')
            res[funs[ordi]] = n
        items = sorted(res.items())
        return [a for a, _ in items], [n for _, n in items]
    except Exception:
        return [], []


class Symbolizer:
    def __init__(self, names_path, modules):
        self.ex_addrs, self.ex_names = load_names(names_path)
        self.mods = sorted(modules)  # (base, size, path)
        self.exp = {}

    @staticmethod
    def fix_path(path):
        # 32-bit process: System32 paths are really SysWOW64 (we run as a 64-bit Python)
        low = path.lower()
        if '\\windows\\system32\\' in low:
            i = low.index('\\windows\\system32\\') + len('\\windows\\')
            path = path[:i] + 'SysWOW64' + path[i + len('system32'):]
        return path

    def module(self, a):
        for base, size, path in self.mods:
            if base <= a < base + size:
                return base, path
        return None, None

    def resolve(self, a):
        """returns (function start or None, display name)"""
        base, path = self.module(a)
        if base is None:
            return None, '?%08X' % a
        mod = os.path.basename(path)
        if base == 0x400000:
            i = bisect.bisect_right(self.ex_addrs, a) - 1
            if i >= 0 and a - self.ex_addrs[i] < 0x8000:
                d = a - self.ex_addrs[i]
                nxt = self.ex_addrs[i + 1] - self.ex_addrs[i] if i + 1 < len(self.ex_addrs) else 0
                # far past a small known symbol: probably an unnamed internal function after it
                if d > 0x600 and (nxt == 0 or nxt > d):
                    return None, '?after ' + self.ex_names[i]
                return self.ex_addrs[i], self.ex_names[i]
            return None, '%s+%X' % (mod, a - base)
        if path not in self.exp:
            self.exp[path] = pe_exports(self.fix_path(path))
        ea, en = self.exp[path]
        rva = a - base
        i = bisect.bisect_right(ea, rva) - 1
        if i >= 0 and rva - ea[i] < 0x4000:
            return base + ea[i], '%s!%s' % (mod.split('.')[0], en[i])
        return None, mod.split('.')[0]


def parse_dump(path):
    meta, mods, samples = {}, [], []
    threads = []
    for line in open(path, encoding='latin-1'):
        line = line.rstrip('\n')
        if line.startswith('# thread'):
            threads.append(line[2:])
        elif line.startswith('# '):
            k, _, v = line[2:].partition(' ')
            meta[k] = v
        elif line.startswith('module '):
            _, b, s, p = line.split(' ', 3)
            mods.append((int(b, 16), int(s, 16), p))
        elif line:
            parts = line.split(' ')
            frames = []
            for f in parts[1:]:
                r, _, t = f.partition('>')
                frames.append((int(r, 16), int(t, 16) if t else 0))
            samples.append((int(parts[0]), frames))
    return meta, threads, mods, samples


def unwind(sym, frames):
    """frames: leaf first. Keep frames whose CALL target matches the function of the frame above."""
    leaf = frames[0][0]
    cur_start, cur_name = sym.resolve(leaf)
    out = [cur_name]
    seen_exe = sym.module(leaf)[0] == 0x400000
    for ret, tgt in frames[1:]:
        if tgt:
            t_start, t_name = sym.resolve(tgt)
            if cur_start is not None:
                # direct call must target the function we are in (allow import thunks / unknown targets)
                if t_start is not None and t_start != cur_start and tgt != cur_start:
                    tb, _ = sym.module(tgt)
                    cb, _ = sym.module(cur_start)
                    if tb == cb:
                        continue
            elif t_start is not None:
                # we are in an unnamed function: accept only if the target is also unnamed / other module
                tb, _ = sym.module(tgt)
                cb, _ = sym.module(leaf)
                if tb == cb and tgt > leaf:
                    continue
        s, n = sym.resolve(ret)
        rb, _ = sym.module(ret)
        if rb != 0x400000 and seen_exe:
            if not (tgt and cur_start is not None and (tgt == cur_start or sym.resolve(tgt)[0] == cur_start)):
                continue
        if rb == 0x400000:
            seen_exe = True
        out.append(n)
        cur_start, leaf = s, ret
    out.reverse()
    # collapse direct recursion
    col = []
    for n in out:
        if not col or col[-1] != n:
            col.append(n)
    return col


def render_html(title, folded, total, info):
    tree = {'n': 'all', 'v': 0, 'c': {}}
    for stack, cnt in folded.items():
        node = tree; node['v'] += cnt
        for fr in stack:
            node = node['c'].setdefault(fr, {'n': fr, 'v': 0, 'c': {}})
            node['v'] += cnt

    def conv(n):
        return {'n': n['n'], 'v': n['v'], 'c': [conv(c) for c in sorted(n['c'].values(), key=lambda x: -x['v'])]}
    data = json.dumps(conv(tree))
    return HTML_TEMPLATE.replace('%TITLE%', html.escape(title)).replace('%INFO%', html.escape(info)).replace('%DATA%', data)


HTML_TEMPLATE = r"""<!doctype html><meta charset=utf-8><title>%TITLE%</title>
<style>
body{font:12px system-ui,sans-serif;margin:16px;background:#fff;color:#222}
#fg{position:relative;width:100%;overflow:hidden}
.f{position:absolute;height:17px;line-height:17px;font-size:11px;overflow:hidden;white-space:nowrap;
 border-right:1px solid #fff;box-sizing:border-box;padding-left:3px;cursor:pointer;color:#111}
#tip{position:fixed;pointer-events:none;background:#222;color:#fff;padding:4px 6px;border-radius:3px;display:none;max-width:700px}
pre{white-space:pre-wrap;color:#555}
</style>
<h3>%TITLE%</h3><pre>%INFO%</pre><p>Kliknij ramkę, by przybliżyć; kliknij "all", by wrócić.</p>
<div id=fg></div><div id=tip></div>
<script>
const root=%DATA%;const fg=document.getElementById('fg'),tip=document.getElementById('tip');
function col(n){let h=0;for(const c of n)h=(h*31+c.charCodeAt(0))>>>0;
 if(/^(kernel|ntdll|KERNEL|KERNELBASE|win32u|user32)/i.test(n))return 'hsl(200,40%,'+(70+h%15)+'%)';
 if(/^(vdfs|Vdfs|shw32|Shw32|SHW32)/.test(n))return 'hsl(120,45%,'+(60+h%20)+'%)';
 if(/!/.test(n))return 'hsl(270,40%,'+(70+h%15)+'%)';
 return 'hsl('+(10+h%45)+',85%,'+(55+h%20)+'%)';}
let focus=root;
function draw(){fg.innerHTML='';const W=fg.clientWidth;let maxd=0;
 const rows=[];(function walk(n,x,d,w){if(w<0.5)return;rows.push([n,x,d,w]);maxd=Math.max(maxd,d);let cx=x;
  for(const c of n.c){const cw=w*c.v/n.v;walk(c,cx,d+1,cw);cx+=cw;}})(focus,0,0,W);
 fg.style.height=(maxd+1)*18+'px';
 for(const [n,x,d,w] of rows){const e=document.createElement('div');e.className='f';
  e.style.left=x+'px';e.style.width=w+'px';e.style.bottom=(d*18)+'px';e.style.background=col(n.n);
  e.textContent=w>30?n.n:'';e.onclick=()=>{focus=(n===focus?root:n);draw();};
  e.onmousemove=ev=>{tip.style.display='block';tip.style.left=(ev.clientX+12)+'px';tip.style.top=(ev.clientY+12)+'px';
   tip.textContent=n.n+'  —  '+n.v+' próbek ('+(100*n.v/root.v).toFixed(1)+'% całości)';};
  e.onmouseleave=()=>tip.style.display='none';fg.appendChild(e);}}
window.onresize=draw;draw();
</script>"""


def main():
    names, out_dir, dumps = sys.argv[1], sys.argv[2], sys.argv[3:]
    os.makedirs(out_dir, exist_ok=True)
    groups = collections.defaultdict(list)
    for d in dumps:
        label = os.path.basename(d).split('_', 1)[1].rsplit('.', 1)[0]
        groups[label.split('_')[0]].append(d)
    for g, files in groups.items():
        folded = collections.Counter()
        total = 0; info = []
        for f in files:
            meta, threads, mods, samples = parse_dump(f)
            sym = Symbolizer(names, mods)
            info.append('%s: wall %s ms, %s; %s' % (meta.get('label'), meta.get('wall_ms'), meta.get('samples'), ' | '.join(threads)))
            info.append('   io: ' + meta.get('io', ''))
            for cnt, frames in samples:
                folded[tuple(unwind(sym, frames))] += cnt
                total += cnt
        with open(os.path.join(out_dir, g + '.folded'), 'w') as fo:
            for st, c in folded.most_common():
                fo.write(';'.join(st) + ' %d\n' % c)
        with open(os.path.join(out_dir, g + '.html'), 'w', encoding='utf-8') as fo:
            fo.write(render_html('zFastSaveLoad – ' + g, folded, total, '\n'.join(info)))
        selfc, incl = collections.Counter(), collections.Counter()
        for st, c in folded.items():
            selfc[st[-1]] += c
            for n in set(st):
                incl[n] += c
        print('=== %s: %d samples from %d dumps' % (g, total, len(files)))
        print('\n'.join(info))
        print('--- top self')
        for n, c in selfc.most_common(25):
            print('%6.1f%%  %s' % (100 * c / total, n))
        print('--- top inclusive')
        for n, c in incl.most_common(45):
            print('%6.1f%%  %s' % (100 * c / total, n))


if __name__ == '__main__':
    main()
