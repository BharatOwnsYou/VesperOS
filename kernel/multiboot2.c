#include "multiboot2.h"
#include "string.h"

/* multiboot2 info tags are packed, 8-byte aligned, walked until type==0 */
void mb2_parse(u32 mb_info_addr, mb2_info_t *out) {
    k_memset(out, 0, sizeof(*out));
    if (!mb_info_addr) return;

    const u8 *base = (const u8 *)(uptr)mb_info_addr;   /* GRUB keeps the info block below 4 GiB (identity-mapped) */
    u32 total_size = *(const u32*)base;
    u32 off = 8; /* skip total_size + reserved */

    while (off + 8 <= total_size) {
        const u8 *tag = base + off;
        u32 type = *(const u32*)(tag + 0);
        u32 size = *(const u32*)(tag + 4);
        if (type == 0) break;

        if (type == 4 && size >= 16) { /* basic memory info */
            out->mem_lower_kb = *(const u32*)(tag + 8);
            out->mem_upper_kb = *(const u32*)(tag + 12);
            out->has_mem = true;
        } else if (type == 6 && size >= 16) {
            /* full memory map: entry_size(u32), entry_version(u32), then entries of entry_size bytes:
             * base(u64), length(u64), type(u32), reserved(u32). type 1 = available RAM.
             * this one reports right on boxes with lots of RAM (tens of GB), the basic-mem tag above is a legacy
             * summary that caps out around the first memory hole */
            u32 entry_size = *(const u32*)(tag + 8);
            u32 mmap_off = 16; /* past type/size/entry_size/entry_version */
            out->total_ram_bytes = 0;
            while (entry_size > 0 && mmap_off + entry_size <= size) {
                const u8 *entry = tag + mmap_off;
                u64 region_len = *(const u64*)(entry + 8);
                u32 region_type = *(const u32*)(entry + 16);
                if (region_type == 1) out->total_ram_bytes += region_len;
                mmap_off += entry_size;
            }
            out->has_mmap = true;
        } else if (type == 8 && size >= 31) { /* framebuffer info */
            out->fb_addr   = *(const u64*)(tag + 8);
            out->fb_pitch  = *(const u32*)(tag + 16);
            out->fb_width  = *(const u32*)(tag + 20);
            out->fb_height = *(const u32*)(tag + 24);
            out->fb_bpp    = *(const u8*) (tag + 28);
            out->fb_type   = *(const u8*) (tag + 29);
            out->has_fb    = true;
            if (out->fb_type == 1 && size >= 37) { /* RGB - color channel layout follows */
                out->fb_red_pos     = *(const u8*)(tag + 31);
                out->fb_red_size    = *(const u8*)(tag + 32);
                out->fb_green_pos   = *(const u8*)(tag + 33);
                out->fb_green_size  = *(const u8*)(tag + 34);
                out->fb_blue_pos    = *(const u8*)(tag + 35);
                out->fb_blue_size   = *(const u8*)(tag + 36);
            }
        }

        off += (size + 7u) & ~7u; /* advance to next 8-byte aligned tag */
    }
}
