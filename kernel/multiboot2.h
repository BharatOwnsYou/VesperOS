#ifndef VESPEROS_MULTIBOOT2_H
#define VESPEROS_MULTIBOOT2_H
#include "../include/types.h"

typedef struct {
    u32 mem_lower_kb, mem_upper_kb; /* from the basic-mem tag if present. capped around the first memory hole, unreliable on big-RAM boxes */
    bool has_mem;

    u64 total_ram_bytes; /* sum of all type==1 regions from the full memory-map tag (type 6), right for any amount of RAM */
    bool has_mmap;

    u64 fb_addr;
    u32 fb_pitch, fb_width, fb_height;
    u8  fb_bpp, fb_type; /* fb_type: 0=indexed, 1=RGB, 2=EGA text */
    u8  fb_red_pos, fb_red_size, fb_green_pos, fb_green_size, fb_blue_pos, fb_blue_size; /* valid when fb_type==1 */
    bool has_fb;
} mb2_info_t;

/* mb_info_addr is the physical pointer GRUB left in ebx (passed through from boot.asm) */
void mb2_parse(u32 mb_info_addr, mb2_info_t *out);

#endif
