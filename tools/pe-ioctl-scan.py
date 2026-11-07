#!/usr/bin/env python3
"""pe-ioctl-scan.py —— 从 32 位 PE 的机器码里找"某个导入函数的调用点"和"调用前压进去的常量"。

为的是回答一个具体问题：Yistart.exe 打开 `\\\\.\\ProcessProtect` 之后，到底给驱动发了哪些
IOCTL 码、带了什么参数。dumpbin /disasm 不把导入名注进列表（实测 0 处），所以不能靠文本搜；
这里直接解析导入表拿到 IAT 槽的虚拟地址，再在 .text 里搜这个 4 字节地址的引用
（32 位 MSVC 的调用形态就是 `FF 15 <abs32>` = call dword ptr [abs32]），
然后把命中点往前几十字节里的 `push imm32` 全部解出来。

用法：
    python pe-ioctl-scan.py <exe> --import DeviceIoControl [--string ProcessProtect] [--context 48]

输出：每个调用点的 VA、原始的附近字节，以及它前面压栈的立即数（十六进制 + 十进制）。
"""
import argparse
import struct
import sys


def load_pe(path):
    data = open(path, 'rb').read()
    if data[:2] != b'MZ':
        raise SystemExit('不是 PE 文件')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        raise SystemExit('缺少 PE 签名')
    machine = struct.unpack_from('<H', data, pe + 4)[0]
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    optsz = struct.unpack_from('<H', data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    if magic != 0x10b:
        raise SystemExit('只支持 PE32（x86），当前 magic=0x%X' % magic)
    image_base = struct.unpack_from('<I', data, opt + 28)[0]
    import_rva = struct.unpack_from('<I', data, opt + 104)[0]
    sec_off = opt + optsz
    secs = []
    for i in range(nsec):
        off = sec_off + i * 40
        name = data[off:off + 8].rstrip(b'\0').decode('latin1')
        va = struct.unpack_from('<I', data, off + 12)[0]
        rawsize = struct.unpack_from('<I', data, off + 16)[0]
        rawptr = struct.unpack_from('<I', data, off + 20)[0]
        secs.append((name, va, rawsize, rawptr))
    return dict(data=data, image_base=image_base, import_rva=import_rva, secs=secs, machine=machine)


def rva_to_off(pe, rva):
    for name, va, rawsize, rawptr in pe['secs']:
        if va <= rva < va + max(rawsize, 1):
            return rawptr + (rva - va)
    return None


def iter_imports(pe):
    """产出 (dll, 函数名, IAT 槽 VA)。"""
    data = pe['data']
    base = pe['image_base']
    desc = rva_to_off(pe, pe['import_rva'])
    if desc is None:
        return
    while True:
        oft, ts, fc, name_rva, ft = struct.unpack_from('<IIIII', data, desc)
        if oft == 0 and ft == 0 and name_rva == 0:
            break
        dll_off = rva_to_off(pe, name_rva)
        dll = data[dll_off:data.index(b'\0', dll_off)].decode('latin1') if dll_off else '?'
        thunk_rva = oft if oft else ft
        i = 0
        while True:
            t_off = rva_to_off(pe, thunk_rva + i * 4)
            if t_off is None:
                break
            val = struct.unpack_from('<I', data, t_off)[0]
            if val == 0:
                break
            if val & 0x80000000:
                fname = '#%d' % (val & 0xffff)
            else:
                f_off = rva_to_off(pe, val)
                fname = data[f_off + 2:data.index(b'\0', f_off + 2)].decode('latin1')
            yield dll, fname, base + ft + i * 4
            i += 1
        desc += 20


def find_text(pe):
    for name, va, rawsize, rawptr in pe['secs']:
        if name == '.text':
            return rawptr, rawsize, va
    return None


def refs_of_va(pe, target_va):
    """在 .text 里找 target_va 的 4 字节小端出现位置（返回文件偏移列表）。"""
    data = pe['data']
    t = find_text(pe)
    if t is None:
        return []
    rawptr, rawsize, _ = t
    needle = struct.pack('<I', target_va)
    out = []
    pos = data.find(needle, rawptr, rawptr + rawsize)
    while pos >= 0:
        out.append(pos)
        pos = data.find(needle, pos + 1, rawptr + rawsize)
    return out


def pushes_before(pe, off, span=64):
    """从 off 往前扫 span 字节，收集 `68 imm32` 形式的压栈立即数。"""
    data = pe['data']
    start = max(0, off - span)
    found = []
    i = start
    while i < off - 4:
        if data[i] == 0x68:
            imm = struct.unpack_from('<I', data, i + 1)[0]
            found.append((off - i, imm))
        i += 1
    return found


def string_va(pe, text, wide=True):
    if wide:
        needle = text.encode('utf-16-le')
    else:
        needle = text.encode('ascii')
    data = pe['data']
    pos = data.find(needle)
    if pos < 0:
        return None
    for name, va, rawsize, rawptr in pe['secs']:
        if rawptr <= pos < rawptr + rawsize:
            return pe['image_base'] + va + (pos - rawptr)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe')
    ap.add_argument('--import', dest='imp', required=True, help='导入函数名，例如 DeviceIoControl')
    ap.add_argument('--string', dest='needle', help='同时看某个字符串的引用，例如 ProcessProtect')
    ap.add_argument('--context', type=int, default=48, help='往前扫多少字节找 push imm32')
    args = ap.parse_args()

    pe = load_pe(args.exe)
    print('文件 %s · 机器 0x%X · ImageBase 0x%08X' % (args.exe, pe['machine'], pe['image_base']))
    targets = {}
    for dll, fname, iat in iter_imports(pe):
        if fname == args.imp:
            targets[iat] = dll
    if not targets:
        print('没找到导入 %s（可能被 API set 转发或静态链接）' % args.imp)
        return 1

    for iat, dll in targets.items():
        print('\n== %s!%s 的 IAT 槽 VA=0x%08X（%s）==' % (dll, args.imp, iat, 'direct'))
        hits = refs_of_va(pe, iat)
        print('   引用点 %d 处' % len(hits))
        for off in hits:
            va = None
            for name, sva, rawsize, rawptr in pe['secs']:
                if rawptr <= off < rawptr + rawsize:
                    va = pe['image_base'] + sva + (off - rawptr)
                    break
            near = pe['data'][max(0, off - 8):off + 8].hex(' ')
            ps = pushes_before(pe, off, args.context)
            print('   @0x%08X  附近字节[%s]' % (va or 0, near))
            for dist, imm in ps:
                signed = imm - 0x100000000 if imm & 0x80000000 else imm
                print('        push 0x%08X (%d)  距调用 %d 字节' % (imm, signed, dist))

    if args.needle:
        sva = string_va(pe, args.needle)
        print('\n== 字符串 "%s" VA=%s ==' % (args.needle, ('0x%08X' % sva) if sva else '未找到'))
        if sva:
            hits2 = refs_of_va(pe, sva)
            print('   引用点 %d 处' % len(hits2))
            for off in hits2:
                near = pe['data'][max(0, off - 8):off + 8].hex(' ')
                print('   @文件偏移 0x%X  附近[%s]' % (off, near))
    return 0


if __name__ == '__main__':
    sys.exit(main())
