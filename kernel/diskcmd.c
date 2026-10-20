#include "diskcmd.h"
#include "blockdev.h"
#include "vga.h"
#include "debug.h"
#include "string.h"
#include "athenalibc.h"

/* everything goes to the screen AND the debug port, so it's still readable on a headless box / over serial */
static void say(const char *s) { vga_puts(s); dbg_puts(s); }

/* decimal or 0x-prefixed hex. shifts instead of multiplies, no 64-bit division on i686 */
static int parse_num(const char *s, u64 *out) {
    int hex = 0;
    u64 v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { hex = 1; s += 2; }
    if (!*s) return -1;
    for (; *s; s++) {
        u32 dgt;
        if (*s >= '0' && *s <= '9') dgt = (u32)(*s - '0');
        else if (hex && *s >= 'a' && *s <= 'f') dgt = (u32)(*s - 'a' + 10);
        else if (hex && *s >= 'A' && *s <= 'F') dgt = (u32)(*s - 'A' + 10);
        else return -1;
        if (v >> 59) return -1;
        v = hex ? ((v << 4) | dgt) : ((v << 3) + (v << 1) + dgt);
    }
    *out = v;
    return 0;
}

static const char *blk_err(int r) {
    switch (r) {
        case BLK_EIO:    return "device error or timeout";
        case BLK_ERANGE: return "lba/length out of range";
        case BLK_EPERM:  return "writes are locked (run 'diskwrites on' first)";
        default:         return "unknown error";
    }
}

/* 128K each, enough for 256 sectors of 512 bytes: more than one AHCI command's worth (128), so a
 * multi-sector diskwrite also exercises the drivers' transfer splitting */
#define DBUF_BYTES (128 * 1024)
static u8 sbuf[DBUF_BYTES] __attribute__((aligned(4096)));
static u8 vbuf[DBUF_BYTES] __attribute__((aligned(4096)));

static void cmd_disks(void) {
    int n = blk_count();
    if (n == 0) {
        say("disks: no block devices (no AHCI/NVMe drive was brought up, see 'lsdisk')\n");
        return;
    }
    vga_puts_col("name    size        sector  model\n", VGA_LIGHT_CYAN, VGA_BLACK);
    for (int i = 0; i < n; i++) {
        blkdev_t *d = blk_get(i);
        u64 mib = (d->sectors * d->sector_size) >> 20;
        char line[96];
        k_snprintf(line, sizeof line, "%s   %u MiB   %u    %s\n", d->name, (u32)mib, d->sector_size, d->model);
        say(line);
    }
    say(blk_writes_enabled() ? "writes: UNLOCKED\n" : "writes: locked\n");
}

static void hexline(u32 off, const u8 *p, int n, char *out) {
    static const char H[] = "0123456789abcdef";
    int k = 0;
    for (int s = 12; s >= 0; s -= 4) out[k++] = H[(off >> s) & 0xF];
    out[k++] = ':';
    for (int i = 0; i < n; i++) { out[k++] = ' '; out[k++] = H[p[i] >> 4]; out[k++] = H[p[i] & 0xF]; }
    out[k++] = '\n';
    out[k] = 0;
}

static void cmd_diskread(int argc, char **argv) {
    if (argc < 3) { say("usage: diskread <dev> <lba>\n"); return; }
    blkdev_t *d = blk_find(argv[1]);
    if (!d) { say("diskread: no such device (see 'disks')\n"); return; }
    u64 lba;
    if (parse_num(argv[2], &lba)) { say("diskread: bad lba\n"); return; }
    int r = blk_read(d, lba, 1, sbuf);
    if (r) { say("diskread: "); say(blk_err(r)); say("\n"); return; }
    for (u32 off = 0; off < 128; off += 16) {
        char line[64];
        hexline(off, sbuf + off, 16, line);
        say(line);
    }
    if (lba == 0) {
        int sig = sbuf[510] == 0x55 && sbuf[511] == 0xAA;
        say(sig ? "boot signature 55AA: present\n" : "boot signature 55AA: absent\n");
    }
}

static void cmd_diskwrites(int argc, char **argv) {
    if (argc < 2) { say(blk_writes_enabled() ? "writes: UNLOCKED\n" : "writes: locked\n"); return; }
    if (k_streq(argv[1], "on")) {
        blk_set_writes_enabled(1);
        vga_puts_col("block writes UNLOCKED. disk commands can now overwrite data, use 'diskwrites off' to lock.\n",
                     VGA_YELLOW, VGA_BLACK);
        dbg_puts("block writes UNLOCKED\n");
    } else if (k_streq(argv[1], "off")) {
        blk_set_writes_enabled(0);
        say("block writes locked\n");
    } else {
        say("usage: diskwrites [on|off]\n");
    }
}

/* writes <count> sectors (default 1), sector i filled with (<byte> + i) & 0xFF so a reordered or dropped
 * sector shows up, then reads them back and compares. raw test command, destroys those sectors */
static void cmd_diskwrite(int argc, char **argv) {
    if (argc < 4) { say("usage: diskwrite <dev> <lba> <hexbyte> [count]   (overwrites sectors)\n"); return; }
    blkdev_t *d = blk_find(argv[1]);
    if (!d) { say("diskwrite: no such device (see 'disks')\n"); return; }
    u64 lba, val, count = 1;
    if (parse_num(argv[2], &lba)) { say("diskwrite: bad lba\n"); return; }
    if (parse_num(argv[3], &val) || val > 0xFF) { say("diskwrite: bad byte value (0-255, or 0x00-0xFF)\n"); return; }
    if (argc > 4 && (parse_num(argv[4], &count) || count == 0)) { say("diskwrite: bad count\n"); return; }
    if (count > DBUF_BYTES / d->sector_size) { say("diskwrite: count too big for the 128K test buffer\n"); return; }

    u32 n = (u32)count;
    for (u32 i = 0; i < n; i++) k_memset(sbuf + (u64)i * d->sector_size, (int)((val + i) & 0xFF), d->sector_size);
    int r = blk_write(d, lba, n, sbuf);
    if (r) { say("diskwrite: "); say(blk_err(r)); say("\n"); return; }
    k_memset(vbuf, 0, (size_t)n * d->sector_size);
    r = blk_read(d, lba, n, vbuf);
    if (r) { say("diskwrite: wrote, but read-back failed: "); say(blk_err(r)); say("\n"); return; }
    for (u32 i = 0; i < n * d->sector_size; i++) {
        if (vbuf[i] != sbuf[i]) { say("diskwrite: VERIFY FAILED (read-back differs)\n"); return; }
    }
    char msg[64];
    k_snprintf(msg, sizeof msg, "diskwrite: ok, %u sector(s) written and verified\n", n);
    say(msg);
}

int disk_shell_cmd(int argc, char **argv) {
    if (argc < 1) return 0;
    if (k_streq(argv[0], "disks"))      { cmd_disks(); return 1; }
    if (k_streq(argv[0], "diskread"))   { cmd_diskread(argc, argv); return 1; }
    if (k_streq(argv[0], "diskwrites")) { cmd_diskwrites(argc, argv); return 1; }
    if (k_streq(argv[0], "diskwrite"))  { cmd_diskwrite(argc, argv); return 1; }
    return 0;
}
