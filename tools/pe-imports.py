# pe-imports.py —— 直接解析 PE 导入表，不依赖 dumpbin
# 原因：dumpbin /imports 在厂商文件上会间歇性只输出 Summary 头（退出码仍 0）。
# 用法：python pe-imports.py <文件或目录> [--capture-only] [--grep 正则] [--verbose]

import struct, sys, os, argparse

CAP = ('BitBlt','StretchBlt','GetDIBits','GetDC','GetWindowDC','PrintWindow','CreateDIBSection','CreateCompatibleDC')

def imports(path):
    b = open(path, 'rb').read()
    if b[:2] != b'MZ': return None
    e = struct.unpack_from('<I', b, 0x3c)[0]
    if b[e:e+4] != b'PE\0\0': return None
    nsec = struct.unpack_from('<H', b, e+6)[0]
    optsz = struct.unpack_from('<H', b, e+20)[0]
    opt = e+24
    pe32p = (struct.unpack_from('<H', b, opt)[0] == 0x20b)
    dd = opt + (112 if pe32p else 96)
    imp_rva = struct.unpack_from('<I', b, dd+8)[0]
    secs = []
    so = opt+optsz
    for i in range(nsec):
        s = so+i*40
        va = struct.unpack_from('<I', b, s+12)[0]
        vs = struct.unpack_from('<I', b, s+8)[0]
        raw = struct.unpack_from('<I', b, s+20)[0]
        secs.append((va, vs, raw))
    def r2o(rva):
        for va, vs, raw in secs:
            if va <= rva < va+max(vs, 1): return raw+(rva-va)
        return None
    out = {}
    if not imp_rva: return out
    o = r2o(imp_rva)
    if o is None: return out
    while True:
        name_rva = struct.unpack_from('<I', b, o+12)[0]
        oft = struct.unpack_from('<I', b, o)[0]
        if name_rva == 0 and oft == 0: break
        no = r2o(name_rva)
        dll = ''
        if no is not None:
            end = b.index(b'\0', no); dll = b[no:end].decode('latin-1')
        funcs = []
        t = r2o(oft) if oft else None
        if t is not None:
            step = 8 if pe32p else 4
            while True:
                v = struct.unpack_from('<Q' if pe32p else '<I', b, t)[0]
                if v == 0: break
                flag = (1 << 63) if pe32p else (1 << 31)
                if not (v & flag):
                    fo = r2o(v & 0x7fffffff)
                    if fo is not None:
                        end = b.index(b'\0', fo+2); funcs.append(b[fo+2:end].decode('latin-1'))
                t += step
        out[dll] = funcs
        o += 20
    return out

def walk(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _, files in os.walk(p):
                for fn in files:
                    if fn.lower().endswith(('.exe', '.dll', '.ads', '.adr', '.ocx', '.moe', '.sys')):
                        yield os.path.join(root, fn)
        else:
            yield p

def main():
    ap = argparse.ArgumentParser(description='直接解析 PE 导入表（不依赖 dumpbin）')
    ap.add_argument('paths', nargs='+', help='文件或目录')
    ap.add_argument('--capture-only', action='store_true', help='只打印带抓屏 API 的文件')
    ap.add_argument('--grep', default=None, help='只打印导入名匹配该正则的文件')
    ap.add_argument('--verbose', action='store_true', help='打印全部导入函数')
    a = ap.parse_args()
    import re
    hits = 0
    for p in walk(a.paths):
        try:
            t = imports(p)
        except Exception as ex:
            print('  %-40s 解析异常 %s' % (os.path.basename(p), ex)); continue
        if t is None: continue
        dlls = sorted(t.keys())
        allf = [f for v in t.values() for f in v]
        cap = sorted({f for f in allf if f.startswith(CAP)})
        gfx = [d for d in dlls if any(k in d.upper() for k in ('D3D11', 'DXGI', 'D3D9', 'GDI32', 'DWMAPI'))]
        if a.capture_only and not cap: continue
        if a.grep and not re.search(a.grep, ' '.join(allf) + ' ' + ' '.join(dlls)): continue
        hits += 1
        rel = os.path.basename(p)
        print('  %-34s DLL %2d · 函数 %3d · 图形DLL: %-22s 抓屏API: %s'
              % (rel, len(dlls), len(allf), ', '.join(gfx) if gfx else '无', ', '.join(cap) if cap else '无'))
        if a.verbose:
            print('        导入DLL: %s' % ', '.join(dlls))
            print('        导入函数: %s' % ', '.join(sorted(allf)))
    print('  ---- 命中 %d 个文件 ----' % hits)

if __name__ == '__main__':
    main()