#ifndef VESPEROS_PS2CTL_H
#define VESPEROS_PS2CTL_H
#include "../include/types.h"

#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64
#define PS2_CMD_PORT    0x64

void ps2ctl_init(void);

int  ps2ctl_output_full(void);
int  ps2ctl_is_mouse_byte(void);
int  ps2ctl_input_full(void);

u8   ps2ctl_read_data(void);
void ps2ctl_write_data(u8 val);
void ps2ctl_write_cmd(u8 cmd);
void ps2ctl_mouse_write(u8 val);

#endif
