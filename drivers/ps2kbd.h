#ifndef VESPEROS_PS2KBD_H
#define VESPEROS_PS2KBD_H
#include "../include/types.h"

void ps2kbd_init(void);
char ps2kbd_poll(void);
int  ps2kbd_module_init(void);
void ps2kbd_module_exit(void);

#endif
