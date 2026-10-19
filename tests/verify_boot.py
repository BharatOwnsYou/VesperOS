#!/usr/bin/env python3
"""Mechanical checks on the built kernel images. Used because the long-mode switch cannot be exercised without a
VM or real hardware: this proves the *artifacts* satisfy everything GRUB and the CPU require of them.

usage: verify_boot.py <kernel.elf> [--arch x86_64|i686]
Exit status 0 = every check passed."""
import struct, subprocess, sys

ok_n = fail_n = 0
def check(cond, what):
    global ok_n, fail_n
    if cond: ok_n += 1; print(f"  ok    {what}")
    else:    fail_n += 1; print(f"  FAIL  {what}")

def main():
    path = sys.argv[1]
    arch = "x86_64"
    if "--arch" in sys.argv: arch = sys.argv[sys.argv.index("--arch") + 1]
    d = open(path, "rb").read()
    is64 = arch == "x86_64"
    print(f"== {path} ({arch})")

    check(d[:4] == b"\x7fELF", "ELF magic")
    check(d[4] == (2 if is64 else 1), f"ELF class is {'ELF64' if is64 else 'ELF32'}")
    if is64:
        e_type, e_machine, _, e_entry, e_phoff = struct.unpack_from("<HHIQQ", d, 16)
        e_phentsize, e_phnum = struct.unpack_from("<HH", d, 54)
        check(e_machine == 62, "machine = x86-64")
    else:
        e_type, e_machine, _, e_entry, e_phoff = struct.unpack_from("<HHIII", d, 16)
        e_phentsize, e_phnum = struct.unpack_from("<HH", d, 42)
        check(e_machine == 3, "machine = i386")

    segs = []
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        if is64:
            p_type, p_flags, p_off, p_va, p_pa, p_fsz, p_msz, p_al = struct.unpack_from("<IIQQQQQQ", d, o)
        else:
            p_type, p_off, p_va, p_pa, p_fsz, p_msz, p_flags, p_al = struct.unpack_from("<IIIIIIII", d, o)
        if p_type == 1: segs.append(dict(off=p_off, va=p_va, pa=p_pa, fsz=p_fsz, msz=p_msz, flags=p_flags))

    check(len(segs) >= 1, "has PT_LOAD segments")
    check(all(s["va"] == s["pa"] for s in segs), "virtual == physical for every segment (identity layout)")
    check(all(s["va"] >= 0x100000 for s in segs), "every loadable segment is at or above 1 MiB (nothing in the legacy BIOS/VGA hole)")
    check(len(segs) <= 4, f"{len(segs)} loadable segments (no extra segment for the ELF headers)")
    check(all(s["va"] + s["msz"] < 0x80000000 for s in segs), "image ends below 2 GiB (required by -mcmodel=small)")
    entry_seg = [s for s in segs if s["va"] <= e_entry < s["va"] + s["msz"]]
    check(bool(entry_seg) and bool(entry_seg[0]["flags"] & 1), "entry point lies in an executable loaded segment")
    check(e_entry < 0x100000000, "entry point below 4 GiB (GRUB jumps to it in 32-bit mode)")

    # ---- multiboot2 header: 8-byte aligned, entirely inside the first 32 KiB of the file
    mb = None
    for off in range(0, min(32768, len(d) - 16), 8):
        if struct.unpack_from("<I", d, off)[0] == 0xE85250D6: mb = off; break
    check(mb is not None, "multiboot2 magic found in first 32 KiB")
    if mb is not None:
        magic, ma, mlen, chk = struct.unpack_from("<IIII", d, mb)
        check(ma == 0, "multiboot2 architecture = 0 (i386 protected-mode entry)")
        check(((magic + ma + mlen + chk) & 0xFFFFFFFF) == 0, "multiboot2 header checksum is correct")
        check(mb + mlen <= 32768, "whole header inside the 32 KiB window")
        # walk tags
        t = mb + 16; end_ok = False
        while t < mb + mlen:
            typ, flg, sz = struct.unpack_from("<HHI", d, t)
            if typ == 0 and sz == 8: end_ok = True; break
            t += (sz + 7) & ~7
        check(end_ok, "multiboot2 tag list is terminated by the end tag within the declared length")

    # ---- symbols
    shoff, shentsize, shnum, shstrndx = (struct.unpack_from("<QHHH", d, 40)[0], *struct.unpack_from("<HHH", d, 58)) if is64 \
        else (struct.unpack_from("<I", d, 32)[0], *struct.unpack_from("<HHH", d, 46))
    secs = []
    for i in range(shnum):
        o = shoff + i * shentsize
        if is64: nm, ty, fl, addr, off, sz, link, info, al, es = struct.unpack_from("<IIQQQQIIQQ", d, o)
        else:    nm, ty, fl, addr, off, sz, link, info, al, es = struct.unpack_from("<IIIIIIIIII", d, o)
        secs.append(dict(type=ty, addr=addr, off=off, size=sz, link=link, entsize=es))
    syms = {}
    sym_size = {}
    for s in secs:
        if s["type"] == 2:                                   # SHT_SYMTAB
            strtab = secs[s["link"]]
            for j in range(s["size"] // s["entsize"]):
                o = s["off"] + j * s["entsize"]
                if is64: st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", d, o)
                else:    st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from("<IIIBBH", d, o)
                a = strtab["off"] + st_name
                name = d[a:d.index(b"\0", a)].decode()
                syms[name] = st_value
                sym_size[name] = st_size
    def file_off(va):
        for s in segs:
            if s["va"] <= va < s["va"] + s["fsz"]: return s["off"] + (va - s["va"])
        return None
    def q(va, n=8):
        o = file_off(va); return int.from_bytes(d[o:o + n], "little") if o is not None else None

    for need in ["_start", "kernel_main", "mmio_map", "arch_early_init"]:
        check(need in syms, f"symbol {need} present")
    check(syms.get("_start") == e_entry, "ELF entry == _start")

    # ---- xHCI DMA structures (spec Table 6-1). Where the compiler puts an unaligned static differs per build; a controller
    # that sees a context crossing a page, or a ring crossing a 64 KiB boundary, can lose events (enumeration works, key
    # presses never arrive). Checked on the real image, so it also validates a clang/lld build.
    xh = {"dcbaa": 4096, "input_ctx": 4096, "device_ctx": 4096, "scratchpad_array": 4096,
          "cmd_ring": 65536, "evt_ring": 65536, "erst": 65536, "ep0_ring": 65536, "hid_devs": 65536}
    present = [n for n in xh if n in syms and sym_size.get(n, 0) > 0]
    check(len(present) == len(xh), f"all {len(xh)} xHCI DMA symbols are present in the image")
    for n in present:
        a, sz, bound = syms[n], sym_size[n], xh[n]
        check(a % 64 == 0, f"{n} is 64-byte aligned ({a:#x})")
        check(a // bound == (a + sz - 1) // bound,
              f"{n} ({a:#x}, {sz:#x} bytes) does not cross a {bound // 1024} KiB boundary")
    if "scratchpad_bufs" in syms:
        check(syms["scratchpad_bufs"] % 4096 == 0, "scratchpad buffers are page aligned")

    if is64:
        for need in ["exception_dispatch", "isr_stub_table", "boot_pml4", "boot_pdpt", "boot_stack_top", "boot_gdt"]:
            check(need in syms, f"symbol {need} present")
        pml4, pdpt = syms["boot_pml4"], syms["boot_pdpt"]
        check(pml4 % 4096 == 0 and pdpt % 4096 == 0, "PML4 and PDPT are 4 KiB aligned")
        check(syms["boot_stack_top"] % 16 == 0, "boot stack top is 16-byte aligned (SysV ABI)")
        check(pml4 < 0x100000000 and pdpt < 0x100000000, "page tables live below 4 GiB (CR3 is loaded from 32-bit code)")
        check(q(pml4) == pdpt + 3, "PML4[0] -> PDPT (present|rw)")
        check(all(q(pml4 + 8 * i) == 0 for i in range(1, 512)), "PML4[1..511] empty")
        pds = []
        for g in range(4):
            e = q(pdpt + 8 * g)
            check(e is not None and (e & 3) == 3 and (e & ~0xFFF) % 4096 == 0, f"PDPT[{g}] -> page directory (present|rw, aligned)")
            pds.append(e & ~0xFFF)
        check(all(q(pdpt + 8 * i) == 0 for i in range(4, 512)), "PDPT[4..511] empty (filled on demand by mmio_map)")
        bad = [(g, i) for g in range(4) for i in range(512)
               if not (g == 0 and i == 0) and q(pds[g] + 8 * i) != ((g << 30) | (i << 21) | 0x83)]
        check(not bad, "page-directory entries 1..2047 are exactly (n * 2 MiB) | present|rw|2MiB-page (identity map of 2 MiB..4 GiB)")
        pd0e = q(pds[0])
        check(pd0e is not None and (pd0e & 0x83) == 0x03 and (pd0e & ~0xFFF) % 4096 == 0,
              "PD[0][0] points at a 4 KiB page table (not a large page): the first 2 MiB spans mixed MTRR types")
        pt = pd0e & ~0xFFF
        badpt = [i for i in range(512) if q(pt + 8 * i) != ((i << 12) | (0x1B if 0xA0 <= i < 0x100 else 0x03))]
        check(not badpt, "first 2 MiB: identity-mapped 4 KiB pages, 0xA0000-0xFFFFF uncached (PWT|PCD), everything else write-back")
        kend = syms.get("__kernel_end", 0)
        check(kend and kend < 0x4000000, f"kernel image ends at {kend:#x} (sane size, well inside the 4 GiB boot identity map)")

        gdt = syms["boot_gdt"]
        check(q(gdt) == 0, "GDT[0] is the null descriptor")
        code = q(gdt + 8)
        check(code == 0x00AF9A000000FFFF and (code >> 53) & 1 == 1 and (code >> 54) & 1 == 0, "GDT[1] is a 64-bit code segment (L=1, D=0)")
        check(q(gdt + 16) == 0x00CF92000000FFFF, "GDT[2] is a writable data segment")

        # ---- instruction ordering of the mode switch, read from the raw opcode bytes of _start
        # (pure Python: works whether the toolchain's objdump is GNU or LLVM, and for either assembler's encodings)
        code = d[file_off(e_entry):file_off(e_entry) + 0xC0]
        pats = [("cpuid", b"\x0f\xa2"), ("mov %eax,%cr4 (PAE)", b"\x0f\x22\xe0"), ("mov %eax,%cr3", b"\x0f\x22\xd8"),
                ("wrmsr (EFER.LME)", b"\x0f\x30"), ("lgdt", b"\x0f\x01\x15"), ("mov %eax,%cr0 (PG)", b"\x0f\x22\xc0"),
                ("far jump", b"\xea")]
        pos = [code.find(b) for _, b in pats]
        # cpuid appears more than once; the *first* occurrence is the feature check and must precede everything else
        check(all(p >= 0 for p in pos) and pos == sorted(pos),
              "mode switch in the architecturally required order: " + " -> ".join(n for n, _ in pats))
        check(code.find(b"\x0f\x22\xe0") < code.find(b"\x0f\x22\xd8"), "CR4.PAE is set before CR3 is loaded")
        check(code.count(b"\x0f\x22\xc0") == 1, "CR0 written exactly once (enabling paging)")

        # ---- no SSE/x87 anywhere: CR4.OSFXSR is never set, so the first such instruction would fault.
        # Needs *an* objdump (GNU or llvm-objdump); skipped loudly rather than silently if neither exists.
        full = None
        for tool in ("objdump", "llvm-objdump"):
            try:
                full = subprocess.run([tool, "-d", path], capture_output=True, text=True, check=True).stdout.lower(); break
            except (OSError, subprocess.CalledProcessError): pass
        if full is None:
            print("  SKIP  no objdump/llvm-objdump found: could not scan for SSE instructions")
        else:
            bad = [w for w in ("xmm", "ymm", "mmx", "%st(", "movaps", "movups", "movdq", "fldt", "fstp") if w in full]
            check(len(full) > 1000 and not bad, "no SSE/AVX/x87 instructions in the whole image" + (f" (found: {bad})" if bad else ""))

    print(f"== {ok_n} passed, {fail_n} failed")
    return 1 if fail_n else 0

sys.exit(main())
