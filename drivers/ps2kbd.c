#include "ps2kbd.h"
#include "ps2ctl.h"
#include "../include/io.h"

static bool shift_held = false;
static bool caps_lock  = false;

/* which scancodes are currently down. fixes duplicate keypresses: some controllers / firmware
 * (SMM usb emulation) redeliver the same make-code with no break in between and we typed the char again.
 * a make is now only accepted if the scancode isn't already held, cleared on break.
 * side effect: no typematic auto-repeat. fine for now */
static bool key_down[128] = { false };

static const char sc_ascii[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0,' ', 0,
};

static const char sc_ascii_shift[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0,' ', 0,
};

void ps2kbd_init(void) {
    shift_held = false;
    caps_lock  = false;
    for (int i = 0; i < 128; i++) key_down[i] = false;
}

char ps2kbd_poll(void) {
    if (!ps2ctl_output_full()) return 0;
    if (ps2ctl_is_mouse_byte()) return 0;

    u8 sc = ps2ctl_read_data();
    bool release = (sc & 0x80) != 0;
    sc &= 0x7F;
    if (sc >= 128) return 0;

    if (sc == 0x2A || sc == 0x36) {
        shift_held = !release;
        return 0;
    }
    if (sc == 0x3A && !release) {
        caps_lock = !caps_lock;
        return 0;
    }

    if (release) {
        key_down[sc] = false;
        return 0;
    }

    if (key_down[sc]) return 0;
    key_down[sc] = true;

    char c = shift_held ? sc_ascii_shift[sc] : sc_ascii[sc];
    if (caps_lock && c >= 'a' && c <= 'z' && !shift_held) c -= 32;
    else if (caps_lock && c >= 'A' && c <= 'Z' && shift_held) c += 32;
    return c;
}

static void kbd_send_and_drain(u8 cmd, int drain_count) {
    ps2ctl_write_data(cmd);
    for (int i = 0; i < drain_count; i++) {
        int timeout = 2000000;
        while (!ps2ctl_output_full() && timeout--) io_wait();
        if (!ps2ctl_output_full()) return;
        if (ps2ctl_is_mouse_byte()) return;
        ps2ctl_read_data();
    }
}

int ps2kbd_module_init(void) {
    ps2kbd_init();
    kbd_send_and_drain(0xFF, 2);
    kbd_send_and_drain(0xF4, 1);
    return 0;
}
void ps2kbd_module_exit(void) {}
