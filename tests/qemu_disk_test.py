#!/usr/bin/env python3
"""boots VesperOS in qemu against emulated SATA (AHCI) and NVMe drives, types shell commands through the
qemu monitor and checks the result both in the debug log and in the raw disk image on the host.

run via `make test-disk` (needs qemu-system-x86_64, grub-mkrescue and a built iso/). takes a few minutes:
every run waits for GRUB + boot. writes only ever touch throwaway images in a temp dir.
optional argument: i686 or x86_64 to test just that kernel."""
import os, shutil, socket, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOOT_WAIT = 9          # seconds: grub (1s timeout) + kernel + shell prompt
SEC = 512
KEY = {' ': 'spc', '-': 'minus', '.': 'dot', '\n': 'ret', '_': 'shift-minus', '/': 'slash'}

fails = 0
def check(name, ok):
    global fails
    print(("  PASS  " if ok else "  FAIL  ") + name)
    if not ok: fails += 1

def need(tool):
    if shutil.which(tool) is None:
        print("missing tool: " + tool + " (see 'make check')"); sys.exit(2)

def make_iso(tmp, entry):
    d = os.path.join(tmp, "iso%d" % entry)
    shutil.copytree(os.path.join(ROOT, "iso"), d)
    cfg = open(os.path.join(ROOT, "grub.cfg")).read()
    cfg = cfg.replace("set timeout=10", "set timeout=1").replace("set default=0", "set default=%d" % entry)
    open(os.path.join(d, "boot/grub/grub.cfg"), "w").write(cfg)
    out = os.path.join(tmp, "test%d.iso" % entry)
    subprocess.run(["grub-mkrescue", "-o", out, d], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return out

def make_image(path, sector):
    with open(path, "wb") as f: f.truncate(16 * 1024 * 1024)
    with open(path, "r+b") as f:
        f.seek(100 * sector); f.write(b"LBA100-MARKER")

def run_qemu(iso, log, drive_args, cmds):
    sock = log + ".sock"
    q = subprocess.Popen(["qemu-system-x86_64", "-cdrom", iso, "-boot", "d", "-m", "512M", "-display", "none",
                          "-serial", "null", "-debugcon", "file:" + log, "-no-reboot",
                          "-monitor", "unix:%s,server,nowait" % sock] + drive_args,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(50):
        if os.path.exists(sock): break
        time.sleep(0.1)
    m = socket.socket(socket.AF_UNIX); m.connect(sock); m.settimeout(2)
    def mon(c):
        m.sendall((c + "\n").encode()); time.sleep(0.04)
        try: m.recv(4096)
        except Exception: pass
    time.sleep(BOOT_WAIT)
    for c in cmds:
        for ch in c + "\n": mon("sendkey " + KEY.get(ch, ch))
        time.sleep(1.5)
    time.sleep(1)
    mon("quit")
    try: q.wait(timeout=5)
    except Exception: q.kill()
    return open(log, errors="replace").read()

def scenario(tmp, iso, arch, kind):
    img = os.path.join(tmp, "%s_%s.img" % (arch, kind))
    sector = 4096 if kind == "nvme4k" else 512
    make_image(img, sector)
    if kind == "ahci":
        dev, drive = "sata0", ["-drive", "id=d0,file=%s,if=none,format=raw" % img, "-device", "ahci,id=ahci",
                               "-device", "ide-hd,drive=d0,bus=ahci.0"]
        tag = "[ahci] sata0:"
    else:
        extra = ",logical_block_size=4096,physical_block_size=4096" if kind == "nvme4k" else ""
        dev, drive = "nvme0", ["-drive", "id=n0,file=%s,if=none,format=raw" % img, "-device",
                               "nvme,drive=n0,serial=vesper0" + extra]
        tag = "[nvme] nvme0:"
    n_big = 256 if sector == 512 else 32           # always more than one driver-level transfer
    cmds = ["disks", "diskread %s 100" % dev, "diskwrite %s 200 0xab" % dev, "diskwrites on",
            "diskwrite %s 200 0xab" % dev, "diskwrite %s 300 0x10 %d" % (dev, n_big),
            "diskwrite %s 99999999 1" % dev]
    print("[%s / %s]" % (arch, kind))
    log = run_qemu(iso, os.path.join(tmp, "%s_%s.log" % (arch, kind)), drive, cmds)
    check("drive detected at boot", tag in log)
    check("diskread returns the marker bytes (4c 42 41 31 30 30)", "0000: 4c 42 41 31 30 30" in log)
    check("write refused while locked", "writes are locked" in log)
    check("single sector write + verify", "1 sector(s) written and verified" in log)
    check("%d-sector write + verify (multi-transfer)" % n_big, "%d sector(s) written and verified" % n_big in log)
    check("out-of-range lba rejected", "out of range" in log)
    d = open(img, "rb").read()
    check("host image: sector 200 holds the written pattern", d[200*sector:201*sector] == bytes([0xab]) * sector)
    check("host image: sector 201 untouched", d[201*sector:202*sector] == bytes(sector))
    check("host image: %d sectors from 300 hold per-sector pattern, edges untouched" % n_big,
          all(d[(300+i)*sector:(301+i)*sector] == bytes([(0x10+i) & 0xff]) * sector for i in range(n_big))
          and d[299*sector:300*sector] == bytes(sector) and d[(300+n_big)*sector:(301+n_big)*sector] == bytes(sector))

def main():
    for t in ("qemu-system-x86_64", "grub-mkrescue"): need(t)
    if not os.path.exists(os.path.join(ROOT, "iso/boot/vesperos4-x86_64.bin")):
        print("no kernels in iso/boot, run 'make iso' first"); sys.exit(2)
    tmp = tempfile.mkdtemp(prefix="vesper-disk-test-")
    try:
        only = sys.argv[1] if len(sys.argv) > 1 else None
        for entry, arch in ((0, "i686"), (1, "x86_64")):
            if only and only != arch: continue
            iso = make_iso(tmp, entry)
            for kind in ("ahci", "nvme", "nvme4k"):
                scenario(tmp, iso, arch, kind)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\n%s" % ("all disk tests passed" if fails == 0 else "%d check(s) FAILED" % fails))
    sys.exit(1 if fails else 0)

main()
