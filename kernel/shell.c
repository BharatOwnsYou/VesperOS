#include "../include/version.h"
#include "../include/arch.h"
#include "shell.h"
#include "vga.h"
#include "vfs.h"
#include "vpkg.h"
#include "modules.h"
#include "services.h"
#include "string.h"
#include "athenalibc.h"
#include "rtc.h"
#include "../drivers/hid.h"
#include "../drivers/ps2mouse.h"
#include "../drivers/pci.h"
#include "../drivers/usb_uhci.h"
#include "../drivers/xhci.h"
#include "../drivers/diskdetect.h"
#include "../net/hespernetd.h"
#include "../net/netcmd.h"
#include "../net/netutil.h"
#include "../offline_packages/chronos.h"
#include "mfs.h"
#include "users.h"
#include "../include/io.h"

u32 g_uptime_ticks = 0;

#define LINE_MAX 160
#define ARGV_MAX 12

static vfs_node_t *cwd;

vfs_node_t *sysvenus_cwd(void) { return cwd; }

typedef struct { const char *name; const char *desc; bool enabled; } feature_t;
static feature_t features[] = {
    { "welcome_art", "show the star banner at login",        true  },
    { "verbose_boot", "print each subsystem as it comes up",  true  },
};
#define FEATURE_COUNT (int)(sizeof(features) / sizeof(features[0]))

static feature_t *find_feature(const char *name) {
    for (int i = 0; i < FEATURE_COUNT; i++)
        if (k_streq(features[i].name, name)) return &features[i];
    return NULL;
}

static void pump_input(void) {
    hid_poll_all();
    usb_uhci_poll();
    xhci_poll();
    hespernetd_poll();              /* network manager: RX, link, DHCP timers */
    netcmd_async_notice();          /* announce a DHCP lease that arrives after the boot wait */
    g_uptime_ticks++;
}

static char blocking_getchar(void) {
    char c;
    while ((c = hid_getchar()) == 0) pump_input();
    return c;
}

static int read_line(char *buf, int max) {
    int len = 0;
    while (1) {
        char c = blocking_getchar();
        if (c == '\n') { buf[len] = 0; vga_putc('\n'); return len; }
        if (c == '\b') {
            if (len > 0) { len--; vga_putc('\b'); vga_putc(' '); vga_putc('\b'); }
            continue;
        }
        if (c >= 32 && c < 127 && len < max - 1) {
            buf[len++] = c;
            vga_putc(c);
        }
    }
}

static void print_prompt(void) {
    char path[128];
    vfs_path(cwd, path, sizeof(path));
    u8 user_color = user_current_is_root() ? VGA_LIGHT_RED : VGA_LIGHT_GREEN;
    vga_puts_col(user_current(), user_color, VGA_BLACK);
    vga_puts_col("@" VESPER_HOSTNAME, VGA_LIGHT_GREEN, VGA_BLACK);
    vga_puts_col(path, VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts_col(user_current_is_root() ? " # " : " $ ", VGA_WHITE, VGA_BLACK);
}

static void print_module_line(const char *name, const char *desc, bool loaded) {
    vga_puts_col(loaded ? "[on ] " : "[off] ", loaded ? VGA_LIGHT_GREEN : VGA_LIGHT_RED, VGA_BLACK);
    vga_puts(name);
    vga_puts(" - ");
    vga_puts(desc);
    vga_putc('\n');
}

static void print_service_line(const char *name, const char *desc, bool started) {
    vga_puts_col(started ? "[on ] " : "[off] ", started ? VGA_LIGHT_GREEN : VGA_LIGHT_RED, VGA_BLACK);
    vga_puts(name);
    vga_puts(" - ");
    vga_puts(desc);
    vga_putc('\n');
}

static void print_pkg_line(const char *name, const char *desc, int installed) {
    vga_puts_col(installed ? "[installed] " : "[available] ", installed ? VGA_LIGHT_GREEN : VGA_LIGHT_GREY, VGA_BLACK);
    vga_puts(name);
    vga_puts(" - ");
    vga_puts(desc);
    vga_putc('\n');
}

static void print_user_line(const char *name, bool is_root, bool is_current) {
    vga_puts_col(is_current ? "* " : "  ", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts_col(name, is_root ? VGA_LIGHT_RED : VGA_LIGHT_GREEN, VGA_BLACK);
    if (is_root) vga_puts(" (root)");
    vga_putc('\n');
}

static void cmd_help(void) {
    vga_puts(
        "Commands:\n"
        "  help                       this text\n"
        "  clear                      clear the screen\n"
        "  echo <text>                print text\n"
        "  pwd                        print working directory\n"
        "  ls [dir]                   list a directory\n"
        "  cd <dir>                   change directory ( .. and / work)\n"
        "  cat <file>                 print a file's contents\n"
        "  mkdir <name>               create a directory\n"
        "  uname                      kernel/version info\n"
        "  mouse                      show current PS/2 / USB HID mouse state\n"
        "  whoami / adduser / su / users   user bookkeeping (no real auth)\n"
        "  lsdisk                     list detected storage controllers\n"
        "  venus list                 list modules, features, services\n"
        "  venus enable <name>        enable a module or feature\n"
        "  venus disable <name>       disable a module or feature\n"
        "  venus smite <service>      start a service (OpenRC-style)\n"
        "  venus stop <service>       stop a service\n"
        "  vpkg list                  list offline packages\n"
        "  vpkg install <name>        install a package into /bin\n"
        "  vpkg remove <name>         remove a package from /bin\n"
        "  install-vesper             install all packages + build an MFS image\n"
        "  start_wm                   launch Solar Display Manager (SDM)\n"
        "  <program name>             run an installed program from /bin\n"
        "  reboot / halt\n"
        "  crashtest yes              deliberately fault the kernel (shows the x86_64 crash dump)\n"
    );
    vga_puts("Networking:\n");
    vga_puts(NETCMD_HELP);
}

static void cmd_ls(int argc, char **argv) {
    vfs_node_t *dir = cwd;
    if (argc > 1) {
        dir = vfs_find_child(cwd, argv[1]);
        if (!dir || dir->type != VFS_DIR) { vga_puts("ls: no such directory\n"); return; }
    }
    for (int i = 0; i < dir->child_count; i++) {
        vfs_node_t *c = dir->children[i];
        u8 color = (c->type == VFS_DIR) ? VGA_LIGHT_BLUE : (c->type == VFS_PROGRAM) ? VGA_LIGHT_GREEN : VGA_WHITE;
        vga_puts_col(c->name, color, VGA_BLACK);
        vga_puts(c->type == VFS_DIR ? "/  " : "  ");
    }
    vga_putc('\n');
}

static void cmd_cd(int argc, char **argv) {
    if (argc < 2) return;
    if (k_streq(argv[1], "/")) { cwd = vfs_root(); return; }
    if (k_streq(argv[1], "..")) { if (cwd->parent) cwd = cwd->parent; return; }
    vfs_node_t *n = vfs_find_child(cwd, argv[1]);
    if (!n || n->type != VFS_DIR) { vga_puts("cd: no such directory\n"); return; }
    cwd = n;
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) { vga_puts("usage: cat <file>\n"); return; }
    vfs_node_t *n = vfs_find_child(cwd, argv[1]);
    if (!n || n->type != VFS_FILE) { vga_puts("cat: no such file\n"); return; }
    vga_puts(n->text ? n->text : "");
}

static void cmd_venus(int argc, char **argv) {
    if (argc < 2) { vga_puts("usage: venus <list|enable|disable|smite|stop> [name]\n"); return; }

    if (k_streq(argv[1], "list")) {
        vga_puts_col("-- kernel modules --\n", VGA_LIGHT_CYAN, VGA_BLACK);
        module_list(print_module_line);
        vga_puts_col("-- features --\n", VGA_LIGHT_CYAN, VGA_BLACK);
        for (int i = 0; i < FEATURE_COUNT; i++)
            print_module_line(features[i].name, features[i].desc, features[i].enabled);
        vga_puts_col("-- services --\n", VGA_LIGHT_CYAN, VGA_BLACK);
        services_list(print_service_line);
        return;
    }

    if (k_streq(argv[1], "smite")) {
        if (argc < 3) { vga_puts("usage: venus smite <service>\n"); return; }
        const char *name = argv[2];
        int r = service_start(name);
        if (r == 1) { vga_puts("service "); vga_puts(name); vga_puts(" started\n"); return; }
        if (r == 0) { vga_puts("service already running\n"); return; }
        if (r == -2) { vga_puts("venus smite: "); vga_puts(name); vga_puts(" failed to start (see 'hespernetctl log' / 'lsnet' for network)\n"); return; }
        vga_puts("venus smite: no such service (try 'venus list')\n");
        return;
    }

    if (k_streq(argv[1], "stop")) {
        if (argc < 3) { vga_puts("usage: venus stop <service>\n"); return; }
        int r = service_stop(argv[2]);
        if (r == 1) { vga_puts("service "); vga_puts(argv[2]); vga_puts(" stopped\n"); return; }
        if (r == 0) { vga_puts("service not running\n"); return; }
        vga_puts("venus stop: no such service (try 'venus list')\n");
        return;
    }

    if (argc < 3) { vga_puts("usage: venus enable|disable <name>\n"); return; }
    const char *name = argv[2];
    bool enabling = k_streq(argv[1], "enable");
    bool disabling = k_streq(argv[1], "disable");
    if (!enabling && !disabling) { vga_puts("venus: expected enable or disable\n"); return; }

    int mr = enabling ? module_enable(name) : module_disable(name);
    if (mr == 1) { vga_puts("module "); vga_puts(name); vga_puts(enabling ? " enabled\n" : " disabled\n"); return; }
    if (mr == 0) { vga_puts("module already in that state\n"); return; }
    if (mr == -2) { vga_puts("that module is builtin and can't be disabled\n"); return; }

    feature_t *f = find_feature(name);
    if (f) { f->enabled = enabling; vga_puts("feature "); vga_puts(name); vga_puts(enabling ? " enabled\n" : " disabled\n"); return; }

    vga_puts("venus: no such module or feature (try 'venus list')\n");
}

static void cmd_vpkg(int argc, char **argv) {
    if (argc < 2) { vga_puts("usage: vpkg <list|install|remove> [name]\n"); return; }
    if (k_streq(argv[1], "list")) { vpkg_list(print_pkg_line); return; }
    if (argc < 3) { vga_puts("usage: vpkg install|remove <name>\n"); return; }

    if (k_streq(argv[1], "install")) {
        int r = vpkg_install(argv[2]);
        if (r == 1) { vga_puts("installed "); vga_puts(argv[2]); vga_puts(" -> /bin\n"); }
        else if (r == 0) vga_puts("already installed\n");
        else vga_puts("vpkg: no such package in /offline_packages\n");
        return;
    }
    if (k_streq(argv[1], "remove")) {
        int r = vpkg_remove(argv[2]);
        if (r == 1) vga_puts("removed\n");
        else vga_puts("vpkg: not installed or no such package\n");
        return;
    }
    vga_puts("usage: vpkg <list|install|remove> [name]\n");
}

static void cmd_mouse(void) {
    const ps2mouse_state_t *m = (const ps2mouse_state_t*)hid_mouse_state();
    vga_puts("x="); vga_put_dec((u32)m->x);
    vga_puts(" y="); vga_put_dec((u32)m->y);
    vga_puts(" left="); vga_puts(m->left ? "1" : "0");
    vga_puts(" right="); vga_puts(m->right ? "1" : "0");
    vga_puts(" middle="); vga_puts(m->middle ? "1" : "0");
    vga_putc('\n');
}

static void run_program(vfs_node_t *prog, int argc, char **argv) {
    prog->run(argc, argv);
}

static void dispatch(char *line) {
    char *argv[ARGV_MAX];
    int argc = k_split_ws(line, argv, ARGV_MAX);
    if (argc == 0) return;

    chronos_log(line);

    if (k_streq(argv[0], "help"))  { cmd_help(); return; }
    if (k_streq(argv[0], "clear")) { vga_clear(); return; }
    if (k_streq(argv[0], "pwd"))   { char p[128]; vfs_path(cwd, p, sizeof(p)); vga_puts(p); vga_putc('\n'); return; }
    if (k_streq(argv[0], "echo")) {
        for (int i = 1; i < argc; i++) { vga_puts(argv[i]); if (i < argc - 1) vga_putc(' '); }
        vga_putc('\n'); return;
    }
    if (k_streq(argv[0], "ls"))    { cmd_ls(argc, argv); return; }
    if (k_streq(argv[0], "cd"))    { cmd_cd(argc, argv); return; }
    if (k_streq(argv[0], "mkdir")) {
        if (argc < 2) { vga_puts("usage: mkdir <name>\n"); return; }
        if (vfs_find_child(cwd, argv[1])) { vga_puts("mkdir: already exists\n"); return; }
        if (!vfs_mkdir(cwd, argv[1])) vga_puts("mkdir: failed (VFS node pool full)\n");
        return;
    }
    if (k_streq(argv[0], "cat"))   { cmd_cat(argc, argv); return; }
    if (k_streq(argv[0], "uname")) { vga_puts(VESPER_OS_FULLNAME ", kernel " VESPER_KERNEL_TAG " (" VESPER_CODENAME "), " VESPER_ARCH "\n"); return; }
    if (k_streq(argv[0], "mouse")) { cmd_mouse(); return; }
    if (k_streq(argv[0], "venus")) { cmd_venus(argc, argv); return; }
    if (k_streq(argv[0], "vpkg"))  { cmd_vpkg(argc, argv); return; }
    if (k_streq(argv[0], "whoami")) {
        vga_puts(user_current());
        vga_putc('\n');
        return;
    }
    if (k_streq(argv[0], "adduser")) {
        if (argc < 2) { vga_puts("usage: adduser <name>\n"); return; }
        int r = user_add(argv[1]);
        if (r == 1) { vga_puts("added user "); vga_puts(argv[1]); vga_putc('\n'); }
        else if (r == 0) vga_puts("adduser: that user already exists\n");
        else vga_puts("adduser: user table full\n");
        return;
    }
    if (k_streq(argv[0], "su")) {
        if (argc < 2) { vga_puts("usage: su <name>\n"); return; }
        if (user_switch(argv[1]) == 1) {
            vga_puts("switched to "); vga_puts(argv[1]); vga_putc('\n');
        } else {
            vga_puts("su: no such user (try 'users' or 'adduser <name>' first)\n");
        }
        return;
    }
    if (k_streq(argv[0], "users")) {
        users_list(print_user_line);
        return;
    }
    if (k_streq(argv[0], "lsdisk")) {
        disk_detect_scan();
        int n = disk_detect_count();
        if (n == 0) {
            vga_puts("lsdisk: no storage controllers found on the PCI bus\n");
            return;
        }
        vga_puts_col("bus:slot.func  vendor:device  kind\n", VGA_LIGHT_CYAN, VGA_BLACK);
        for (int i = 0; i < n; i++) {
            const disk_ctrl_t *c = disk_detect_at(i);
            char line[80];
            k_snprintf(line, sizeof(line), "%x:%x.%x        %x:%x       %s\n",
                       c->bus, c->slot, c->func, c->vendor_id, c->device_id,
                       disk_ctrl_kind_name(c->kind));
            vga_puts(line);
        }
        vga_puts_col(
            "note: detection only - no block driver exists yet, so nothing\n"
            "here can actually be read from or written to.\n",
            VGA_DARK_GREY, VGA_BLACK);
        return;
    }
    if (k_streq(argv[0], "reboot")) {
        u8 good = 0x02;
        while (good & 0x02) good = inb(0x64);
        outb(0x64, 0xFE);
        while (1) __asm__ volatile ("hlt");
    }
    if (k_streq(argv[0], "crashtest")) {
        if (argc < 2 || !k_streq(argv[1], "yes")) {
            vga_puts("crashtest deliberately crashes the kernel to prove the exception handler prints a register dump.\n"
                     "Run 'crashtest yes' to proceed. The machine will halt; reboot afterwards.\n");
            return;
        }
        arch_crash_test();
        vga_puts("crashtest: this build (" VESPER_ARCH ") has no exception handler.\n");
        return;
    }
    if (k_streq(argv[0], "halt")) {
        vga_puts("system halted.\n");
        while (1) __asm__ volatile ("cli; hlt");
    }
    if (k_streq(argv[0], "start_wm")) {
        vfs_node_t *sdm = vfs_find_child(vfs_dir_bin, "sdm");
        if (sdm && sdm->type == VFS_PROGRAM) {
            sdm->run(argc, argv);
        } else {
            vga_puts("start_wm: SDM not installed. try: vpkg install sdm\n");
        }
        return;
    }
    if (k_streq(argv[0], "install-vesper")) {
        vga_puts_col("=== " VESPER_OS_FULLNAME " Installer ===\n\n", VGA_LIGHT_MAGENTA, VGA_BLACK);

        vga_puts("Installing all offline packages into /bin...\n");
        vpkg_install_all();
        vga_puts_col("[ok] ", VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts("packages installed\n");

        if (!vfs_find_child(vfs_dir_home, "projects")) vfs_mkdir(vfs_dir_home, "projects");
        vga_puts_col("[ok] ", VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts("/home/projects created\n");

        u32 sz = mfs_save();
        vga_puts_col("[ok] ", VGA_LIGHT_GREEN, VGA_BLACK);
        vga_puts("MFS image built: ");
        vga_put_dec(sz);
        vga_puts(" bytes, ");
        vga_put_dec(mfs_record_count());
        vga_puts(" records\n\n");

        vga_puts_col("Note: ", VGA_YELLOW, VGA_BLACK);
        vga_puts(
            "this image lives in RAM only - there is no disk driver yet,\n"
            "so it will not survive a reboot. Once a real NVMe/AHCI block\n"
            "driver exists, this is exactly the image it would write to\n"
            "your drive and read back at the next boot.\n");
        return;
    }

    if (netcmd_dispatch(argc, argv)) return;     /* ifconfig, lsnet, ping, nslookup, hespernetctl */

    vfs_node_t *prog = vfs_find_child(vfs_dir_bin, argv[0]);
    if (prog && prog->type == VFS_PROGRAM) { run_program(prog, argc, argv); return; }

    vga_puts(argv[0]);
    vga_puts(": command not found (try 'help' or 'vpkg list')\n");
}

static void print_boot_line(const char *msg, int ok) {
    if (!features[1].enabled) return;
    vga_puts_col(ok ? "[  ok  ] " : "[ warn ] ", ok ? VGA_LIGHT_GREEN : VGA_YELLOW, VGA_BLACK);
    vga_puts(msg);
    vga_putc('\n');
}

void sysvenus_main(void) {
    vga_init();
    vga_puts_col("SysVenus", VGA_LIGHT_MAGENTA, VGA_BLACK);
    vga_puts(" init starting " VESPER_OS_FULLNAME " (kernel " VESPER_KERNEL_TAG ", OpenRC-style)...\n\n");

    services_init();
    hid_core_init();
    vfs_init();
    users_init();
    cwd = vfs_root();

    modules_init();
    print_boot_line("ps2kbd module loaded", 1);
    print_boot_line("ps2mouse module loaded", 1);
    print_boot_line(arch_describe(), 1);

    pci_scan();
    print_boot_line("PCI bus scanned", 1);

    disk_detect_scan();
    {
        int dcount = disk_detect_count();
        if (dcount > 0) {
            char msg[64];
            k_snprintf(msg, sizeof(msg), "%d storage controller(s) detected (see 'lsdisk')", dcount);
            print_boot_line(msg, 1);
        } else {
            print_boot_line("no storage controllers detected on PCI bus", 0);
        }
    }

    if (module_enable("usb_uhci") == 1 && usb_uhci_present()) {
        print_boot_line("UHCI USB controller found and reset", 1);
        usb_uhci_enumerate();
        if (usb_uhci_hid_count() > 0) print_boot_line("USB HID device(s) enumerated", 1);
        else print_boot_line("no USB HID devices found on root ports", 1);
    } else {
        print_boot_line("no UHCI USB controller present (ok if using PS/2 only)", 0);
    }

    if (module_enable("xhci") == 1 && xhci_present()) {
        print_boot_line("xHCI USB controller found and reset", 1);
        xhci_enumerate();
        if (xhci_hid_count() > 0) {
            print_boot_line("USB HID device(s) enumerated via xHCI", 1);
        } else {
            /* printed unconditionally: with no working keyboard there is no other way to see what went wrong */
            print_boot_line("xHCI controller is up but NO USB HID device was enumerated", 0);
            char dl[130];
            for (int di = 0; di < 2; di++) { xhci_diag_line(di, dl, sizeof dl); vga_puts("        "); vga_puts(dl); vga_puts("\n"); }
        }
    } else {
        print_boot_line("no xHCI USB controller present", 0);
    }

    /* hespernetd: network manager. registered with auto_start so it probes the NIC drivers, brings up every
         * interface and starts DHCP right here, then keeps running from pump_input() */
    services_register("hespernetd", "HesperNet network manager (NIC drivers, DHCP, ARP, ICMP, UDP, DNS)",
                      hespernetd_service_start, hespernetd_service_stop, true);
    if (hespernetd_running()) {
        print_boot_line("hespernetd started (network manager)", 1);
        u32 net_t0 = net_time_ms();
        while (!hespernetd_boot_ready(net_t0)) pump_input();     /* up to 8 s for link + DHCP */
    }
    netcmd_boot_report(features[1].enabled);
    netcmd_notice_sync();

    vpkg_init_builtin_packages();
    print_boot_line("offline package repo indexed (/offline_packages)", 1);

    vga_putc('\n');
    if (features[0].enabled) {
        vga_puts_col("   *  " VESPER_OS_FULLNAME "  *\n", VGA_LIGHT_MAGENTA, VGA_BLACK);
    }
    vfs_node_t *motd = vfs_find_child(vfs_dir_etc, "motd");
    if (motd) vga_puts(motd->text);
    vga_putc('\n');

    char line[LINE_MAX];
    while (1) {
        print_prompt();
        read_line(line, LINE_MAX);
        dispatch(line);
    }
}
