#ifndef VESPEROS_PS2MOUSE_H
#define VESPEROS_PS2MOUSE_H
#include "../include/types.h"

typedef struct {
    s32 x, y;
    s8  dx, dy;
    bool left, right, middle;
    bool updated;
} ps2mouse_state_t;

int ps2mouse_module_init(void);
void ps2mouse_module_exit(void);
int ps2mouse_poll(ps2mouse_state_t *out);
const ps2mouse_state_t *ps2mouse_get(void);

#endif
