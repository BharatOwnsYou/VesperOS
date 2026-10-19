#include "modules.h"
#include "string.h"
#include "../drivers/ps2ctl.h"
#include "../drivers/ps2kbd.h"
#include "../drivers/ps2mouse.h"
#include "../drivers/usb_uhci.h"
#include "../drivers/xhci.h"

#define MAX_MODULES 16
static kmodule_t mods[MAX_MODULES];
static int mod_count = 0;

static int noop_init(void) { return 0; }
static void noop_exit(void) {}

static void reg(const char *name, const char *desc, int (*init)(void), void (*exit)(void), bool builtin) {
    if (mod_count >= MAX_MODULES) return;
    kmodule_t *m = &mods[mod_count++];
    k_strcpy(m->name, name);
    k_strcpy(m->desc, desc);
    m->init = init ? init : noop_init;
    m->exit = exit ? exit : noop_exit;
    m->loaded = false;
    m->builtin = builtin;
}

void modules_init(void) {
    mod_count = 0;

    ps2ctl_init(); /* bring up the 8042 controller: flush stale buffer, enable both ports */

    reg("ps2kbd",   "PS/2 keyboard (8042, polled)",         ps2kbd_module_init,   ps2kbd_module_exit,   true);
    reg("ps2mouse", "PS/2 mouse, 3-byte packet protocol",    ps2mouse_module_init, ps2mouse_module_exit, true);
    reg("usb_uhci", "UHCI USB1.1 host controller + boot HID", usb_uhci_module_init, usb_uhci_module_exit, false);
    reg("xhci", "xHCI USB2/3 host controller + boot HID (real hardware USB)", xhci_module_init, xhci_module_exit, false);

    for (int i = 0; i < mod_count; i++) {
        if (mods[i].builtin) {
            mods[i].init();
            mods[i].loaded = true;
        }
    }
}

static kmodule_t *find(const char *name) {
    for (int i = 0; i < mod_count; i++)
        if (k_streq(mods[i].name, name)) return &mods[i];
    return NULL;
}

int module_enable(const char *name) {
    kmodule_t *m = find(name);
    if (!m) return -1;
    if (m->loaded) return 0;
    m->init();
    m->loaded = true;
    return 1;
}

int module_disable(const char *name) {
    kmodule_t *m = find(name);
    if (!m) return -1;
    if (m->builtin) return -2;
    if (!m->loaded) return 0;
    m->exit();
    m->loaded = false;
    return 1;
}

void module_list(void (*printer)(const char *name, const char *desc, bool loaded)) {
    for (int i = 0; i < mod_count; i++)
        printer(mods[i].name, mods[i].desc, mods[i].loaded);
}

int module_count(void) { return mod_count; }
