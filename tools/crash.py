"""x64 minidump 读取器（EDF5 崩溃排查用）。

用法：
    python tools\\crash.py                 # 读 CrashDumps 里最新的 EDF5 dump
    python tools\\crash.py <dump路径>      # 读指定 dump

输出：
  1. 我们 DLL（部署版 vs 本地构建）的节表与 .text 布局是否一致
  2. 模块列表（EDF5 / 我们的 DLL / dxgi / d3d / dcomp / havok 相关）
  3. 异常记录：线程 id、异常码、异常地址、参数、寄存器
  4. **崩溃线程整条栈**的全部返回地址 → 模块+RVA（EDF5EnemyHealthBar 的额外给 .map 符号名）
  5. 所有线程里出现我们 DLL 地址的情况（找"是不是我们"）
  6. 我们 DLL 是否真的被加载（模块列表里查）
"""
import glob
import os
import re
import struct
import sys

ROOT = r"F:\deepseek-harness\default-workspace\projects\edf5-enemy-healthbar"
DLL_LOCAL = os.path.join(ROOT, "build", "EDF5EnemyHealthBar.dll")
DLL_DEP = r"E:\SteamLibrary\steamapps\common\EARTH DEFENSE FORCE 5\Mods\Plugins\EDF5EnemyHealthBar.dll"
MAP = os.path.join(ROOT, "build", "EDF5EnemyHealthBar.map")
CRASH_DIR = os.path.expandvars(r"%LOCALAPPDATA%\CrashDumps")


def newest_dump():
    cands = glob.glob(os.path.join(CRASH_DIR, "EDF5*.dmp"))
    if not cands:
        return None
    return max(cands, key=os.path.getmtime)


def pe_sections(path):
    d = open(path, "rb").read()
    e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, e_lfanew + 6)[0]
    opt = e_lfanew + 24
    size_opt = struct.unpack_from("<H", d, e_lfanew + 20)[0]
    base = struct.unpack_from("<Q", d, opt + 24)[0]
    size_img = struct.unpack_from("<I", d, opt + 56)[0]
    secs = []
    off = opt + size_opt
    for _ in range(nsec):
        name = d[off:off + 8].rstrip(b"\0").decode("latin1")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", d, off + 8)
        secs.append((name, vaddr, vsize, rawptr, rawsize))
        off += 40
    return base, size_img, secs


def load_map(path, text_rva):
    syms = []
    if not os.path.exists(path):
        return syms
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\s+([0-9a-fA-F]{4}):([0-9a-fA-F]{8})\s+(\S+)", line)
        if m:
            syms.append((int(m.group(2), 16) + text_rva, m.group(3)))
    syms.sort()
    return syms


def sym_at(syms, rva):
    lo, hi, best = 0, len(syms) - 1, None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= rva:
            best, lo = syms[mid], mid + 1
        else:
            hi = mid - 1
    return best


def main():
    dump = sys.argv[1] if len(sys.argv) > 1 else newest_dump()
    if not dump or not os.path.exists(dump):
        print("找不到 dump 文件，请传路径")
        return 1
    print("== dump ==")
    print("  %s  (%s, %.1f MB)" % (dump, __import__("time").strftime(
        "%Y-%m-%d %H:%M:%S", __import__("time").localtime(os.path.getmtime(dump))),
        os.path.getsize(dump) / 1048576))

    # --- 我们 DLL 的节表：部署版 vs 本地构建 ---
    text_local = text_dep = None
    if os.path.exists(DLL_LOCAL):
        _, _, s = pe_sections(DLL_LOCAL)
        t = [x for x in s if x[0] == ".text"]
        text_local = (t[0][1], t[0][2]) if t else None
        print("\n== 本地构建 DLL ==  .text rva=%#x size=%#x  mtime=%s" % (
            text_local[0], text_local[1],
            __import__("time").strftime("%Y-%m-%d %H:%M",
                                        __import__("time").localtime(os.path.getmtime(DLL_LOCAL)))))
    if os.path.exists(DLL_DEP):
        _, _, s = pe_sections(DLL_DEP)
        t = [x for x in s if x[0] == ".text"]
        text_dep = (t[0][1], t[0][2]) if t else None
        print("== 已部署 DLL ==  .text rva=%#x size=%#x  mtime=%s" % (
            text_dep[0], text_dep[1],
            __import__("time").strftime("%Y-%m-%d %H:%M",
                                        __import__("time").localtime(os.path.getmtime(DLL_DEP)))))
    if text_local and text_dep:
        print("   布局一致=%s（不一致说明 .map 符号名对不上部署版）" % (text_local == text_dep))

    syms = load_map(MAP, text_local[0]) if text_local else []
    if not syms:
        print("   (没用上 .map 符号表：%s)" % MAP)

    data = open(dump, "rb").read()
    sig, ver, nstreams, dirrva = struct.unpack_from("<IIII", data, 0)
    if sig != 0x504D444D:
        print("不是 minidump: %#x" % sig)
        return 1
    streams = {}
    for i in range(nstreams):
        st, sz, rva = struct.unpack_from("<III", data, dirrva + i * 12)
        streams.setdefault(st, (sz, rva))

    # --- 模块 ---
    mods = []
    if 4 in streams:
        _, rva = streams[4]
        n = struct.unpack_from("<I", data, rva)[0]
        for i in range(n):
            m = rva + 4 + i * 108
            base = struct.unpack_from("<Q", data, m)[0]
            size = struct.unpack_from("<I", data, m + 8)[0]
            namerva = struct.unpack_from("<I", data, m + 20)[0]
            ln = struct.unpack_from("<I", data, namerva)[0]
            name = data[namerva + 4:namerva + 4 + ln].decode("utf-16-le", "replace")
            mods.append((base, size, name))
    mods.sort()
    print("\n== 模块 %d 个（只列关键的）==" % len(mods))
    ours = 0
    for b, s, n in mods:
        low = n.lower()
        if any(k in low for k in ("edf5", "enemyhealthbar", "dxgi", "d3d11", "dcomp",
                                  "dwmapi", "havok", "vcruntime", "winmm", "dxcore")):
            tag = ""
            if "enemyhealthbar" in low:
                tag = "   <<< 我们的 DLL"
                ours += 1
            print("  %016X +%08X  %s%s" % (b, s, n, tag))
    if ours == 0:
        print("  ** 模块列表里没有 EDF5EnemyHealthBar.dll —— 这次崩溃时 MOD 未加载 **")

    def mod_for(addr):
        for b, s, n in mods:
            if b <= addr < b + s:
                return b, s, n
        return None

    def describe(addr):
        mm = mod_for(addr)
        if not mm:
            return None
        name = os.path.basename(mm[2])
        off = addr - mm[0]
        s = "  %s+%#x" % (name, off)
        if "EDF5EnemyHealthBar" in name and syms:
            sym = sym_at(syms, off)
            if sym:
                s += "  [%s +%#x]" % (sym[1], off - sym[0])
        return s

    # --- 线程列表 ---
    threads = []
    if 3 in streams:
        _, rva = streams[3]
        n = struct.unpack_from("<I", data, rva)[0]
        for i in range(n):
            m = rva + 4 + i * 48
            tid, susp, prio_c, prio = struct.unpack_from("<IIII", data, m)
            teb = struct.unpack_from("<Q", data, m + 16)[0]
            st_start, st_size, st_rva = struct.unpack_from("<QII", data, m + 24)
            cx_size, cx_rva = struct.unpack_from("<II", data, m + 40)
            threads.append(dict(tid=tid, teb=teb, stack=(st_start, st_size, st_rva),
                                ctx_rva=cx_rva, ctx_size=cx_size))
    print("\n== 线程 %d 个 ==" % len(threads))

    # --- 内存 ---
    mem = []
    if 9 in streams:
        _, rva = streams[9]
        nranges, baserva = struct.unpack_from("<QQ", data, rva)
        off, cur = rva + 16, baserva
        for _ in range(nranges):
            start, size = struct.unpack_from("<QQ", data, off)
            mem.append((start, size, cur))
            cur += size
            off += 16
    if 5 in streams:
        _, rva = streams[5]
        n = struct.unpack_from("<I", data, rva)[0]
        for i in range(n):
            m = rva + 4 + i * 16
            start, size, mrva = struct.unpack_from("<QII", data, m)
            mem.append((start, size, mrva))

    def mem_read(addr, size):
        for start, sz, rva in mem:
            if start <= addr < start + sz:
                return data[rva + (addr - start):rva + min(sz, size + (addr - start))]
        return None

    # --- 异常记录 ---
    eaddr = None
    fault_tid = None
    if 6 in streams:
        _, rva = streams[6]
        tid, _al = struct.unpack_from("<II", data, rva)
        ecode, eflags, _erec, eaddr, nparam = struct.unpack_from("<IIQQI", data, rva + 8)
        params = struct.unpack_from("<15Q", data, rva + 36)
        print("\n== 异常 ==")
        print("  线程 id     = %#x (%d)" % (tid, tid))
        print("  异常码      = %#010x" % ecode)
        print("  异常地址    = %#018x%s" % (eaddr, describe(eaddr) or "  (不在任何模块内)"))
        print("  参数        = %s" % [hex(p) for p in params[:nparam]])
        if nparam >= 2:
            print("                param0(读/写?) = %#x  param1(地址) = %#018x%s"
                  % (params[0], params[1], describe(params[1]) or ""))
        fault_tid = tid

    # --- 崩溃线程上下文（优先取线程自己的 CONTEXT，比 stream 6 全）---
    def read_ctx(ctx_rva, ctx_size):
        # x64 CONTEXT: Rip @0xF8, Rsp @0x98, Rbp @0xA0, Rax @0x78, Rcx @0x80, Rdx @0x88, R8 @0xB8
        if ctx_size < 0x120:
            return None
        c = ctx_rva
        return dict(
            rip=struct.unpack_from("<Q", data, c + 0xF8)[0],
            rsp=struct.unpack_from("<Q", data, c + 0x98)[0],
            rbp=struct.unpack_from("<Q", data, c + 0xA0)[0],
            rax=struct.unpack_from("<Q", data, c + 0x78)[0],
            rcx=struct.unpack_from("<Q", data, c + 0x80)[0],
            rdx=struct.unpack_from("<Q", data, c + 0x88)[0],
            rbx=struct.unpack_from("<Q", data + 0, c + 0x90)[0] if False else struct.unpack_from("<Q", data, c + 0x90)[0],
            rsi=struct.unpack_from("<Q", data, c + 0xA8)[0],
            rdi=struct.unpack_from("<Q", data, c + 0xB0)[0],
            r8=struct.unpack_from("<Q", data, c + 0xB8)[0],
            r9=struct.unpack_from("<Q", data, c + 0xC0)[0],
            r10=struct.unpack_from("<Q", data, c + 0xC8)[0],
            r11=struct.unpack_from("<Q", data, c + 0xD0)[0],
            r12=struct.unpack_from("<Q", data, c + 0xD8)[0],
            r13=struct.unpack_from("<Q", data, c + 0xE0)[0],
            r14=struct.unpack_from("<Q", data, c + 0xE8)[0],
            r15=struct.unpack_from("<Q", data, c + 0xF0)[0],
        )

    fth = next((t for t in threads if t["tid"] == fault_tid), None)
    ctx = None
    if fth and fth["ctx_rva"]:
        ctx = read_ctx(fth["ctx_rva"], fth["ctx_size"])
    if 6 in streams and ctx is None:
        _, rva = streams[6]
        csz, crva = struct.unpack_from("<II", data, rva + 8 + 152)
        ctx = read_ctx(crva, csz)

    if ctx:
        print("\n== 崩溃线程寄存器 ==")
        for k in ("rip", "rsp", "rbp", "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
                  "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"):
            print("  %-4s = %#018x%s" % (k, ctx[k], describe(ctx[k]) or ""))
        # 解引用一些可疑寄存器（找 null+偏移 之类的经典 AV）
        print("\n== 可疑寄存器解引用 ==")
        for k in ("rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r12", "r13", "r14", "r15"):
            v = ctx[k]
            if v == 0:
                print("  %-4s = NULL（解引用即 AV）" % k)
                continue
            if v < 0x10000:
                print("  %-4s = %#x（小整数/野指针）" % (k, v))
                continue
            chunk = mem_read(v, 16)
            if chunk and len(chunk) >= 16:
                q0, q1 = struct.unpack_from("<QQ", chunk, 0)
                print("  %-4s -> [%#x] = %#018x%s | %#018x%s" % (
                    k, v, q0, describe(q0) or "", q1, describe(q1) or ""))

        # --- 整条栈扫描 ---
        print("\n== 崩溃线程栈（从 rsp 向下扫指针，映射到模块）==")
        st = fth["stack"] if fth else None
        lo = hi = None
        if st and st[0] <= ctx["rsp"] < st[0] + st[1]:
            lo, hi = ctx["rsp"], st[0] + st[1]
            chunk = mem_read(lo, hi - lo)
        else:
            chunk = mem_read(ctx["rsp"], 0x8000)
            lo = ctx["rsp"]
        if not chunk:
            print("  栈内存不在 dump 里")
        else:
            n = 0
            for i in range(0, len(chunk) - 8, 8):
                v = struct.unpack_from("<Q", chunk, i)[0]
                d = describe(v)
                if d:
                    print("  [rsp+%#06x] %016X%s" % (i, v, d))
                    n += 1
                    if n > 120:
                        print("  ... (截断)")
                        break
            if n == 0:
                print("  栈上没有指向任何模块的返回地址（栈未包含在 dump 里？）")

    # --- 所有线程里是否出现我们的 DLL ---
    print("\n== 其他线程中的我们 DLL 地址 ==")
    anyhit = False
    for t in threads:
        if t["tid"] == fault_tid:
            continue
        c = read_ctx(t["ctx_rva"], t["ctx_size"]) if t["ctx_rva"] else None
        if not c:
            continue
        hits = [k for k in ("rip", "rsp", "rbp", "rax", "rbx", "rcx", "rdx",
                            "rsi", "rdi", "r8", "r9", "r12", "r13", "r14", "r15")
                if "EDF5EnemyHealthBar" in (describe(c[k]) or "")]
        st = t["stack"]
        if st[2] and st[1]:
            chunk = mem_read(st[0], st[1])
            if chunk:
                for i in range(0, len(chunk) - 8, 8):
                    v = struct.unpack_from("<Q", chunk, i)[0]
                    d = describe(v)
                    if d and "EDF5EnemyHealthBar" in d:
                        hits.append("stack+%#x" % i)
        if hits:
            anyhit = True
            print("  线程 %#x: %s" % (t["tid"], ", ".join(hits[:12])))
    if not anyhit:
        print("  （无）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
