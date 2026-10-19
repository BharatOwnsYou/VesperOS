#include "hid.h"
#include "ps2kbd.h"
#include "ps2mouse.h"
#include "../kernel/string.h"

#define MAX_HID_DEVICES 8
#define KBD_QUEUE_SIZE  32

static hid_device_t devices[MAX_HID_DEVICES];
static int device_count = 0;

static char kbd_queue[KBD_QUEUE_SIZE];
static int  kq_head = 0, kq_tail = 0;

static ps2mouse_state_t mouse_state;

void hid_core_init(void) {
    device_count = 0;
    kq_head = kq_tail = 0;
    k_memset(&mouse_state, 0, sizeof(mouse_state));
    mouse_state.x = 40; mouse_state.y = 12;
}

hid_device_t *hid_register(const char *name, hid_type_t type) {
    if (device_count >= MAX_HID_DEVICES) return NULL;
    hid_device_t *d = &devices[device_count++];
    k_strcpy(d->name, name);
    d->type = type;
    d->active = true;
    d->next = NULL;
    return d;
}

int hid_device_count(void) { return device_count; }

void hid_list(void (*printer)(const char *line)) {
    for (int i = 0; i < device_count; i++) {
        printer(devices[i].name);
    }
}

void hid_push_key(char c) {
    int next = (kq_tail + 1) % KBD_QUEUE_SIZE;
    if (next == kq_head) return;
    kbd_queue[kq_tail] = c;
    kq_tail = next;
}

void hid_push_mouse_packet(s8 dx, s8 dy, bool l, bool r, bool m) {
    mouse_state.dx = dx; mouse_state.dy = dy;
    mouse_state.left = l; mouse_state.right = r; mouse_state.middle = m;
    mouse_state.x += dx;
    mouse_state.y -= dy;
    if (mouse_state.x < 0) mouse_state.x = 0;
    if (mouse_state.y < 0) mouse_state.y = 0;
    if (mouse_state.x > 79) mouse_state.x = 79;
    if (mouse_state.y > 24) mouse_state.y = 24;
    mouse_state.updated = true;
}

void hid_poll_all(void) {
    char c = ps2kbd_poll();
    if (c) hid_push_key(c);

    ps2mouse_state_t pms;
    if (ps2mouse_poll(&pms)) {
        mouse_state = pms;
    }
}

char hid_getchar(void) {
    if (kq_head == kq_tail) return 0;
    char c = kbd_queue[kq_head];
    kq_head = (kq_head + 1) % KBD_QUEUE_SIZE;
    return c;
}

const void *hid_mouse_state(void) {
    return &mouse_state;
}
