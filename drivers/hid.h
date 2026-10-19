#ifndef VESPEROS_HID_H
#define VESPEROS_HID_H
#include "../include/types.h"

typedef enum { HID_KEYBOARD, HID_MOUSE } hid_type_t;

typedef struct hid_device {
    char        name[32];
    hid_type_t  type;
    bool        active;
    struct hid_device *next;
} hid_device_t;

void hid_core_init(void);
hid_device_t *hid_register(const char *name, hid_type_t type);
void hid_poll_all(void);
char hid_getchar(void);
const void *hid_mouse_state(void);
int hid_device_count(void);
void hid_list(void (*printer)(const char *line));

void hid_push_key(char c);
void hid_push_mouse_packet(s8 dx, s8 dy, bool l, bool r, bool m);

#endif
