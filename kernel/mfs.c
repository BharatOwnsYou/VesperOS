#include "mfs.h"
#include "vfs.h"
#include "string.h"

#define MFS_MAX_RECORDS 96
#define MFS_DATA_MAX    8192
#define MFS_IMAGE_MAX   (sizeof(mfs_header_t) + (MFS_MAX_RECORDS * sizeof(mfs_record_t)) + MFS_DATA_MAX)

static u8  mfs_image[MFS_IMAGE_MAX];
static u32 mfs_image_len = 0;

static mfs_record_t tmp_records[MFS_MAX_RECORDS];
static u8  tmp_data[MFS_DATA_MAX];
static u32 tmp_record_count = 0;
static u32 tmp_data_used = 0;

/* recursively walks a VFS subtree, one record per dir/file (skips VFS_PROGRAM nodes, see mfs.h).
 * returns this node's record index, or -1 if the pool is full */
static s32 mfs_walk(vfs_node_t *node, s32 parent_idx) {
    if (node->type == VFS_PROGRAM) return -1;
    if (tmp_record_count >= MFS_MAX_RECORDS) return -1;

    s32 my_idx = (s32)tmp_record_count;
    mfs_record_t *r = &tmp_records[tmp_record_count++];
    k_memset(r, 0, sizeof(*r));
    k_strcpy(r->name, node->name);
    r->type = (node->type == VFS_DIR) ? 0 : 1;
    r->parent_idx = parent_idx;

    if (node->type == VFS_FILE && node->text) {
        u32 len = (u32)k_strlen(node->text);
        if (tmp_data_used + len <= MFS_DATA_MAX) {
            k_memcpy(tmp_data + tmp_data_used, node->text, len);
            r->data_offset = tmp_data_used;
            r->data_len = len;
            tmp_data_used += len;
        }
    }

    for (int i = 0; i < node->child_count; i++)
        mfs_walk(node->children[i], my_idx);

    return my_idx;
}

u32 mfs_save(void) {
    tmp_record_count = 0;
    tmp_data_used = 0;
    mfs_walk(vfs_root(), -1);

    mfs_header_t hdr;
    hdr.magic[0] = MFS_MAGIC0; hdr.magic[1] = MFS_MAGIC1;
    hdr.magic[2] = MFS_MAGIC2; hdr.magic[3] = MFS_MAGIC3;
    hdr.version = 1;
    hdr.record_count = tmp_record_count;
    hdr.data_size = tmp_data_used;

    u32 off = 0;
    k_memcpy(mfs_image + off, &hdr, sizeof(hdr));
    off += sizeof(hdr);
    k_memcpy(mfs_image + off, tmp_records, tmp_record_count * sizeof(mfs_record_t));
    off += tmp_record_count * sizeof(mfs_record_t);
    k_memcpy(mfs_image + off, tmp_data, tmp_data_used);
    off += tmp_data_used;

    mfs_image_len = off;
    return mfs_image_len;
}

u32 mfs_image_size(void)   { return mfs_image_len; }
u32 mfs_record_count(void) { return tmp_record_count; }
