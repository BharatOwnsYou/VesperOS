#include "ps2mouse.h"
#include "ps2ctl.h"
#include "../include/io.h"

static ps2mouse_state_t state;
static u8  packet[3];
static int packet_idx = 0;
static bool enabled = false;

int ps2mouse_module_init(void) {
    packet_idx = 0;
    state.x = 40; state.y = 12;
    state.dx = state.dy = 0;
    state.left = state.right = state.middle = false;
    state.updated = false;

    ps2ctl_mouse_write(0xFF);
    ps2ctl_mouse_write(0xF6);
    ps2ctl_mouse_write(0xF4);
    enabled = true;
    return 0;
}

void ps2mouse_module_exit(void) {
    if (enabled) ps2ctl_mouse_write(0xF5);
    enabled = false;
}

const ps2mouse_state_t *ps2mouse_get(void) {
    return &state;
}

int ps2mouse_poll(ps2mouse_state_t *out) {
    state.updated = false;
    if (!enabled) return 0;

    if (!ps2ctl_output_full()) return 0;
    if (!ps2ctl_is_mouse_byte()) return 0;

    u8 b = ps2ctl_read_data();

    if (packet_idx == 0 && !(b & 0x08)) return 0;

    packet[packet_idx++] = b;
    if (packet_idx < 3) return 0;
    packet_idx = 0;

    u8 flags = packet[0];
    s32 raw_dx = packet[1];
    s32 raw_dy = packet[2];

    if (flags & 0x10) raw_dx -= 256;
    if (flags & 0x20) raw_dy -= 256;
    if (flags & 0xC0) {
        if (out) *out = state;
        return 0;
    }

    state.dx = (s8)raw_dx;
    state.dy = (s8)raw_dy;
    state.left   = (flags & 0x01) != 0;
    state.right  = (flags & 0x02) != 0;
    state.middle = (flags & 0x04) != 0;

    state.x += raw_dx;
    state.y -= raw_dy;
    if (state.x < 0) state.x = 0;
    if (state.y < 0) state.y = 0;
    if (state.x > 79) state.x = 79;
    if (state.y > 24) state.y = 24;

    state.updated = true;
    if (out) *out = state;
    return 1;
}
