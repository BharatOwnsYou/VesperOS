#include "ps2ctl.h"
#include "../include/io.h"

int ps2ctl_output_full(void) {
    return inb(PS2_STATUS_PORT) & 0x01;
}

int ps2ctl_input_full(void) {
    return inb(PS2_STATUS_PORT) & 0x02;
}

int ps2ctl_is_mouse_byte(void) {
    return (inb(PS2_STATUS_PORT) & 0x20) != 0;
}

static void ps2ctl_wait_input_clear(void) {
    int timeout = 100000;
    while (ps2ctl_input_full() && timeout--) io_wait();
}

static void ps2ctl_wait_output_full(void) {
    int timeout = 100000;
    while (!ps2ctl_output_full() && timeout--) io_wait();
}

u8 ps2ctl_read_data(void) {
    return inb(PS2_DATA_PORT);
}

void ps2ctl_write_data(u8 val) {
    ps2ctl_wait_input_clear();
    outb(PS2_DATA_PORT, val);
}

void ps2ctl_write_cmd(u8 cmd) {
    ps2ctl_wait_input_clear();
    outb(PS2_CMD_PORT, cmd);
}

void ps2ctl_mouse_write(u8 val) {
    ps2ctl_write_cmd(0xD4);
    ps2ctl_write_data(val);
    ps2ctl_wait_output_full();
    (void)ps2ctl_read_data();
}

void ps2ctl_init(void) {
    ps2ctl_write_cmd(0xAD);
    ps2ctl_write_cmd(0xA7);

    /* flush stale bytes from the output buffer. one flush isn't always enough, firmware doing SMM
     * usb->ps2 emulation can leave several queued and then the first keypress is garbage.
     * bounded so a stuck controller can't hang boot */
    for (int i = 0; i < 32 && ps2ctl_output_full(); i++) ps2ctl_read_data();

    ps2ctl_write_cmd(0x20);
    ps2ctl_wait_output_full();
    u8 cfg = ps2ctl_read_data();
    cfg |= 0x03;
    cfg &= ~0x30;
    ps2ctl_write_cmd(0x60);
    ps2ctl_write_data(cfg);

    ps2ctl_write_cmd(0xAE);
    ps2ctl_write_cmd(0xA8);
}
