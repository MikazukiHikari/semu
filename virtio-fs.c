#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>


#include "common.h"
#include "device.h"
#include "fuse.h"
#include "riscv.h"
#include "riscv_private.h"
#include "virtio.h"

#define VFS_DEV_CNT_MAX 1

#define VFS_FEATURES_0 0
#define VFS_FEATURES_1 1 /* VIRTIO_F_VERSION_1 */
#define VFS_QUEUE_NUM_MAX 1024
#define VFS_QUEUE (vfs->queues[vfs->QueueSel])
#define NUM_REQUEST_QUEUES_ADDR 0x49

#define PRIV(x) ((struct virtio_fs_config *) x->priv)

PACKED(struct virtio_fs_config {
    char tag[36];
    uint32_t num_request_queues;
    uint32_t notify_buf_size;  // ignored
});

typedef struct {
    DIR *dir;
    char path[4096];
} dir_handle_t;



static struct virtio_fs_config vfs_configs[VFS_DEV_CNT_MAX];
static int vfs_dev_cnt = 0;

static void virtio_fs_set_fail(virtio_fs_state_t *vfs)
{
    vfs->Status |= VIRTIO_STATUS__DEVICE_NEEDS_RESET;
    if (vfs->Status & VIRTIO_STATUS__DRIVER_OK)
        vfs->InterruptStatus |= VIRTIO_INT__CONF_CHANGE;
}

static inline uint32_t vfs_preprocess(virtio_fs_state_t *vfs, uint32_t addr)
{
    if ((addr >= RAM_SIZE) || (addr & 0b11))
        return virtio_fs_set_fail(vfs), 0;

    return addr >> 2;
}

static void virtio_fs_update_status(virtio_fs_state_t *vfs, uint32_t status)
{
    vfs->Status |= status;
    if (status)
        return;

    /* Reset */
    uint32_t *ram = vfs->ram;
    void *priv = vfs->priv;
    char *mount_tag = vfs->mount_tag;
    char shared_dir[4096];
    snprintf(shared_dir, sizeof(shared_dir), "%s", vfs->shared_dir);
    inode_map_entry *inode_map = vfs->inode_map;
    memset(vfs, 0, sizeof(*vfs));
    vfs->ram = ram;
    vfs->priv = priv;
    vfs->mount_tag = mount_tag;
    snprintf(vfs->shared_dir, sizeof(vfs->shared_dir), "%s", shared_dir);
    vfs->inode_map = inode_map;
}

static void virtio_fs_init_handler(virtio_fs_state_t *vfs,
                                   struct virtq_desc vq_desc[4],
                                   uint32_t *plen)
{
    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    struct fuse_init_out *init_out =
        (struct fuse_init_out *) ((uintptr_t) vfs->ram + vq_desc[3].addr);

    header_resp->out.len =
        sizeof(struct fuse_out_header) + sizeof(struct fuse_init_out);
    header_resp->out.error = 0;

    // Fill init_out with capabilities
    init_out->major = 7;
    init_out->minor = 41;
    init_out->max_readahead = 0x10000;
    // init_out->flags = FUSE_ASYNC_READ | FUSE_EXPORT_SUPPORT | FUSE_BIG_WRITES
    // | FUSE_DO_READDIRPLUS | FUSE_READDIRPLUS_AUTO | FUSE_WRITEBACK_CACHE;
    init_out->flags = FUSE_ASYNC_READ | FUSE_BIG_WRITES | FUSE_DO_READDIRPLUS;
    init_out->max_background = 64;
    init_out->congestion_threshold = 32;
    init_out->max_write = 0x131072;
    init_out->time_gran = 1;

    *plen = header_resp->out.len;
}

static void virtio_fs_getattr_handler(virtio_fs_state_t *vfs,
                                      struct virtq_desc vq_desc[4],
                                      uint32_t *plen)
{
    const struct fuse_in_header *in_header =
        (struct fuse_in_header *) ((uintptr_t) vfs->ram + vq_desc[0].addr);
    uint64_t inode = in_header->nodeid;

    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    struct fuse_attr_out *outattr =
        (struct fuse_attr_out *) ((uintptr_t) vfs->ram + vq_desc[3].addr);

    header_resp->out.len =
        sizeof(struct fuse_out_header) + sizeof(struct fuse_attr_out);
    header_resp->out.error = 0;

    const char *target_path = NULL;
    struct stat st;

    // root entry (inode=1)
    if (inode == 1) {
        target_path = vfs->shared_dir;
    } else {
        inode_map_entry *entry = NULL;
        HASH_FIND_INT(vfs->inode_map, &inode, entry);
        if (!entry) {
            header_resp->out.error = -ENOENT;
            *plen = sizeof(struct fuse_out_header);
            return;
        }
        target_path = entry->path;
    }

    if (stat(target_path, &st) < 0) {
        header_resp->out.error = -errno;
        *plen = sizeof(struct fuse_out_header);
        return;
    }

    outattr->attr_valid = 60;
    outattr->attr_valid_nsec = 0;
    outattr->attr.ino = st.st_ino;
    outattr->attr.size = st.st_size;
    outattr->attr.blocks = st.st_blocks;
    outattr->attr.atime = st.st_atime;
    outattr->attr.mtime = st.st_mtime;
    outattr->attr.ctime = st.st_ctime;
    outattr->attr.mode = st.st_mode;
    outattr->attr.nlink = st.st_nlink;
    outattr->attr.uid = st.st_uid;
    outattr->attr.gid = st.st_gid;
    outattr->attr.blksize = st.st_blksize;

    *plen = header_resp->out.len;
}



static void virtio_fs_opendir_handler(virtio_fs_state_t *vfs,
                                      struct virtq_desc vq_desc[4],
                                      uint32_t *plen)
{
    struct fuse_in_header *in_header =
        (struct fuse_in_header *) ((uintptr_t) vfs->ram + vq_desc[0].addr);
    uint64_t nodeid = in_header->nodeid;

    inode_map_entry *entry = NULL;
    HASH_FIND_INT(vfs->inode_map, &nodeid, entry);
    if (!entry) {
        return;
    }

    DIR *dir = opendir(entry->path);
    if (!dir) {
        return;
    }

    dir_handle_t *handle = malloc(sizeof(dir_handle_t));
    handle->dir = dir;
    strncpy(handle->path, entry->path, sizeof(handle->path));
    handle->path[sizeof(handle->path) - 1] = '\0';

    struct fuse_open_out *open_out =
        (struct fuse_open_out *) ((uintptr_t) vfs->ram + vq_desc[3].addr);
    memset(open_out, 0, sizeof(*open_out));
    open_out->fh = (uint64_t) handle;
    open_out->open_flags = 0;

    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len =
        sizeof(struct fuse_out_header) + sizeof(struct fuse_open_out);
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
}

static void virtio_fs_readdirplus_handler(virtio_fs_state_t *vfs,
                                          struct virtq_desc vq_desc[4],
                                          uint32_t *plen)
{
    struct fuse_read_in *read_in =
        (struct fuse_read_in *) ((uintptr_t) vfs->ram + vq_desc[1].addr);
    dir_handle_t *handle = (dir_handle_t *) (uintptr_t) read_in->fh;
    if (!handle || !handle->dir) {
        return;
    }

    DIR *dir = handle->dir;
    const char *dir_path = handle->path;

    uintptr_t base = (uintptr_t) vfs->ram + vq_desc[3].addr;
    size_t offset = 0;

    rewinddir(dir);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char full_path[4096];
        full_path[0] = '\0';

        size_t dir_len = strlen(dir_path);
        size_t name_len = strlen(entry->d_name);

        if (dir_len + 1 + name_len + 1 > sizeof(full_path)) {
            fprintf(stderr, "Path too long: %s/%s\n", dir_path, entry->d_name);
            continue;  // 或 return
        }

        strcpy(full_path, dir_path);
        strcat(full_path, "/");
        strcat(full_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) < 0) {
            printf("[READDIRPLUS] stat failed for: %s\n", full_path);
            continue;
        }

        struct fuse_entry_out *entry_out =
            (struct fuse_entry_out *) (base + offset);
        memset(entry_out, 0, sizeof(*entry_out));
        entry_out->nodeid = st.st_ino;
        entry_out->attr.ino = st.st_ino;
        entry_out->attr.mode = st.st_mode;
        entry_out->attr.nlink = st.st_nlink;
        entry_out->attr.size = st.st_size;
        entry_out->attr.atime = st.st_atime;
        entry_out->attr.mtime = st.st_mtime;
        entry_out->attr.ctime = st.st_ctime;
        entry_out->attr.uid = st.st_uid;
        entry_out->attr.gid = st.st_gid;
        entry_out->attr.blksize = st.st_blksize;
        entry_out->attr.blocks = st.st_blocks;

        struct fuse_direntplus *direntplus =
            (struct fuse_direntplus *) (base + offset +
                                        sizeof(struct fuse_entry_out));
        direntplus->dirent.ino = st.st_ino;
        direntplus->dirent.namelen = strlen(entry->d_name);
        direntplus->dirent.type = (st.st_mode & __S_IFDIR) ? 4 : 8;
        memcpy(direntplus->dirent.name, entry->d_name,
               direntplus->dirent.namelen);

        size_t dirent_size =
            sizeof(struct fuse_direntplus) + direntplus->dirent.namelen;
        size_t dirent_aligned = (dirent_size + 7) & ~7;
        offset += sizeof(struct fuse_entry_out) + dirent_aligned;
        printf("%s      ", entry->d_name);
    }
    printf("\n");

    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len = sizeof(struct fuse_out_header) + offset;
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
    if (header_resp->out.error)
        printf("[READDIRPLUS] error: %d\n", header_resp->out.error);
}

static void virtio_fs_releasedir_handler(virtio_fs_state_t *vfs,
                                         struct virtq_desc vq_desc[4],
                                         uint32_t *plen)
{
    struct fuse_release_in *release_in =
        (struct fuse_release_in *) ((uintptr_t) vfs->ram + vq_desc[1].addr);
    dir_handle_t *handle = (dir_handle_t *) (uintptr_t) release_in->fh;
    if (handle) {
        closedir(handle->dir);
        free(handle);
    }
    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len = sizeof(struct fuse_out_header);
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
}

static void virtio_fs_lookup_handler(virtio_fs_state_t *vfs,
                                     struct virtq_desc vq_desc[4],
                                     uint32_t *plen)
{
    const struct fuse_in_header *in_header =
        (struct fuse_in_header *) ((uintptr_t) vfs->ram + vq_desc[0].addr);
    uint64_t parent_inode = in_header->nodeid;

    struct fuse_lookup_in *lookup_in =
        (struct fuse_lookup_in *) ((uintptr_t) vfs->ram + vq_desc[1].addr);
    char *name = (char *) (lookup_in);
    size_t name_len = vq_desc[1].len;

    char name_buf[256];
    if (name_len >= sizeof(name_buf))
        name_len = sizeof(name_buf) - 1;
    memcpy(name_buf, name, name_len);
    name_buf[name_len] = '\0';

    inode_map_entry *parent_entry = NULL;
    HASH_FIND_INT(vfs->inode_map, &parent_inode, parent_entry);
    if (!parent_entry) {
        printf("[LOOKUP] ERROR: parent inode=%lu not found\n", parent_inode);
        return;
    }
    const char *parent_path = parent_entry->path;

    char host_path[4096];
    host_path[0] = '\0';
    size_t parent_len = strlen(parent_path);
    size_t name_len1 = strlen(name_buf);
    if (parent_len + 1 + name_len1 + 1 > sizeof(host_path)) {
        fprintf(stderr, "Path too long: %s/%s\n", parent_path, name_buf);
        return;
    }
    strcpy(host_path, parent_path);
    strcat(host_path, "/");
    strcat(host_path, name_buf);

    struct stat st;
    if (stat(host_path, &st) < 0) {
        struct vfs_resp_header *header_resp =
            (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
        header_resp->out.error = -ENOENT;
        *plen = sizeof(struct fuse_out_header);
        return;
    }

    inode_map_entry *entry = NULL;
    HASH_FIND_INT(vfs->inode_map, &st.st_ino, entry);
    if (!entry) {
        entry = malloc(sizeof(inode_map_entry));
        entry->ino = st.st_ino;
        strncpy(entry->path, host_path, sizeof(entry->path));
        HASH_ADD_INT(vfs->inode_map, ino, entry);
    }

    struct fuse_entry_out *entry_out =
        (struct fuse_entry_out *) ((uintptr_t) vfs->ram + vq_desc[3].addr);
    memset(entry_out, 0, sizeof(*entry_out));
    entry_out->nodeid = st.st_ino;
    entry_out->attr.ino = st.st_ino;
    entry_out->attr.mode = st.st_mode;
    entry_out->attr.nlink = st.st_nlink;
    entry_out->attr.size = st.st_size;
    entry_out->attr.atime = st.st_atime;
    entry_out->attr.mtime = st.st_mtime;
    entry_out->attr.ctime = st.st_ctime;
    entry_out->attr.uid = st.st_uid;
    entry_out->attr.gid = st.st_gid;
    entry_out->attr.blksize = st.st_blksize;
    entry_out->attr.blocks = st.st_blocks;

    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len =
        sizeof(struct fuse_out_header) + sizeof(struct fuse_entry_out);
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
}

static void virtio_fs_open_handler(virtio_fs_state_t *vfs,
                                   struct virtq_desc vq_desc[4],
                                   uint32_t *plen)
{
    const struct fuse_in_header *in_header =
        (struct fuse_in_header *) ((uintptr_t) vfs->ram + vq_desc[0].addr);
    uint64_t inode = in_header->nodeid;

    const char *target_path = NULL;
    if (inode == 1) {
        target_path = vfs->shared_dir;
    } else {
        inode_map_entry *entry = NULL;
        HASH_FIND_INT(vfs->inode_map, &inode, entry);
        if (!entry) {
            struct vfs_resp_header *header_resp =
                (struct vfs_resp_header *) ((uintptr_t) vfs->ram +
                                            vq_desc[2].addr);
            header_resp->out.error = -ENOENT;
            *plen = sizeof(struct fuse_out_header);
            return;
        }
        target_path = entry->path;
    }

    int fd = open(target_path, O_RDONLY);
    if (fd < 0) {
        struct vfs_resp_header *header_resp =
            (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
        header_resp->out.error = -errno;
        *plen = sizeof(struct fuse_out_header);
        printf("[OPEN] failed: %s, error=%s\n", target_path, strerror(errno));
        return;
    }

    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    struct fuse_open_out *open_out =
        (struct fuse_open_out *) ((uintptr_t) vfs->ram + vq_desc[3].addr);

    open_out->fh = (uint64_t) fd;
    open_out->open_flags = 0;
    header_resp->out.len =
        sizeof(struct fuse_out_header) + sizeof(struct fuse_open_out);
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
}

static void virtio_fs_read_handler(virtio_fs_state_t *vfs,
                                   struct virtq_desc vq_desc[4],
                                   uint32_t *plen)
{
    struct fuse_read_in *read_in =
        (struct fuse_read_in *) ((uintptr_t) vfs->ram + vq_desc[1].addr);
    int fd = (int) (uintptr_t) read_in->fh;
    off_t offset = read_in->offset;
    size_t size = read_in->size;

    char buf[4096];
    if (size > sizeof(buf))
        size = sizeof(buf);

    ssize_t n = pread(fd, buf, size, offset);
    if (n < 0) {
        struct vfs_resp_header *header_resp =
            (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
        header_resp->out.error = -errno;
        *plen = sizeof(struct fuse_out_header);
        printf("[READ] failed: fd=%d, errno=%d\n", fd, errno);
        return;
    }

    memcpy((void *) ((uintptr_t) vfs->ram + vq_desc[3].addr), buf, n);

    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len = sizeof(struct fuse_out_header) + n;
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
}

static void virtio_fs_release_handler(virtio_fs_state_t *vfs,
                                      struct virtq_desc vq_desc[4],
                                      uint32_t *plen)
{
    struct fuse_release_in *release_in =
        (struct fuse_release_in *) ((uintptr_t) vfs->ram + vq_desc[1].addr);
    int fd = (int) release_in->fh;
    close(fd);
    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len = sizeof(struct fuse_out_header);
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
}

static void virtio_fs_flush_handler(virtio_fs_state_t *vfs,
                                    struct virtq_desc vq_desc[4],
                                    uint32_t *plen)
{
    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len = sizeof(struct fuse_out_header);
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
}


static void virtio_fs_forget_handler(virtio_fs_state_t *vfs,
                                     struct virtq_desc vq_desc[4],
                                     uint32_t *plen)
{
    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len = sizeof(struct fuse_out_header);
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
}

static void virtio_fs_destroy_handler(virtio_fs_state_t *vfs,
                                      struct virtq_desc vq_desc[4],
                                      uint32_t *plen)
{
    struct vfs_resp_header *header_resp =
        (struct vfs_resp_header *) ((uintptr_t) vfs->ram + vq_desc[2].addr);
    header_resp->out.len = sizeof(struct fuse_out_header);
    header_resp->out.error = 0;
    *plen = header_resp->out.len;
}



static int virtio_fs_desc_handler(virtio_fs_state_t *vfs,
                                  const virtio_fs_queue_t *queue,
                                  uint32_t desc_idx,
                                  uint32_t *plen)
{
    struct virtq_desc vq_desc[4];
    for (int i = 0; i < 4; i++) {
        /* The size of the `struct virtq_desc` is 4 words */
        const struct virtq_desc *desc =
            (struct virtq_desc *) &vfs->ram[queue->QueueDesc + desc_idx * 4];
        vq_desc[i].addr = desc->addr;
        vq_desc[i].len = desc->len;
        vq_desc[i].flags = desc->flags;
        desc_idx = desc->next;
    }


    const struct vfs_req_header *header_req =
        (struct vfs_req_header *) ((uintptr_t) vfs->ram + vq_desc[0].addr);
    uint32_t op = header_req->in.opcode;
    switch (op) {
    case FUSE_INIT:
        virtio_fs_init_handler(vfs, vq_desc, plen);
        break;
    case FUSE_GETATTR:
        virtio_fs_getattr_handler(vfs, vq_desc, plen);
        break;
    case FUSE_OPENDIR:
        virtio_fs_opendir_handler(vfs, vq_desc, plen);
        break;
    case FUSE_READDIRPLUS:
        virtio_fs_readdirplus_handler(vfs, vq_desc, plen);
        break;
    case FUSE_LOOKUP:
        virtio_fs_lookup_handler(vfs, vq_desc, plen);
        break;
    case FUSE_FORGET:
        virtio_fs_forget_handler(vfs, vq_desc, plen);
        break;
    case FUSE_RELEASEDIR:
        virtio_fs_releasedir_handler(vfs, vq_desc, plen);
        break;
    case FUSE_OPEN:
        virtio_fs_open_handler(vfs, vq_desc, plen);
        break;
    case FUSE_READ:
        virtio_fs_read_handler(vfs, vq_desc, plen);
        break;
    case FUSE_RELEASE:
        virtio_fs_release_handler(vfs, vq_desc, plen);
        break;
    case FUSE_FLUSH:
        virtio_fs_flush_handler(vfs, vq_desc, plen);
        break;
    case FUSE_DESTROY:
        virtio_fs_destroy_handler(vfs, vq_desc, plen);
        break;
    default:
        fprintf(stderr, "unsupported virtio-fs operation!\n");
        return -1;
    }

    return 0;
}



static void virtio_queue_notify_handler(virtio_fs_state_t *vfs, int index)
{
    uint32_t *ram = vfs->ram;
    virtio_fs_queue_t *queue = &vfs->queues[index];
    if (vfs->Status & VIRTIO_STATUS__DEVICE_NEEDS_RESET)
        return;

    if (!((vfs->Status & VIRTIO_STATUS__DRIVER_OK) && queue->ready))
        return virtio_fs_set_fail(vfs);

    uint16_t new_avail = ram[queue->QueueAvail] >> 16;
    if (new_avail - queue->last_avail > (uint16_t) queue->QueueNum)
        return virtio_fs_set_fail(vfs);

    if (queue->last_avail == new_avail)
        return;

    uint16_t new_used = ram[queue->QueueUsed] >> 16;
    while (queue->last_avail != new_avail) {
        uint16_t queue_idx = queue->last_avail % queue->QueueNum;
        uint16_t buffer_idx = ram[queue->QueueAvail + 1 + queue_idx / 2] >>
                              (16 * (queue_idx % 2));

        uint32_t len = 0;
        int result = virtio_fs_desc_handler(vfs, queue, buffer_idx, &len);
        if (result != 0)
            return virtio_fs_set_fail(vfs);

        uint32_t vq_used_addr =
            queue->QueueUsed + 1 + (new_used % queue->QueueNum) * 2;
        ram[vq_used_addr] = buffer_idx;
        ram[vq_used_addr + 1] = len;
        queue->last_avail++;
        new_used++;
    }

    vfs->ram[queue->QueueUsed] &= MASK(16);
    vfs->ram[queue->QueueUsed] |= ((uint32_t) new_used) << 16;

    if (!(ram[queue->QueueAvail] & 1))
        vfs->InterruptStatus |= VIRTIO_INT__USED_RING;
}

static bool virtio_fs_reg_read(virtio_fs_state_t *vfs,
                               uint32_t addr,
                               uint32_t *value)
{
#define _(reg) VIRTIO_##reg
    switch (addr) {
    case _(MagicValue):
        *value = 0x74726976;  // "virt"
        return true;
    case _(Version):
        *value = 2;
        return true;
    case _(DeviceID):
        *value = 26;  // = virtio-fs
        return true;
    case _(VendorID):
        *value = VIRTIO_VENDOR_ID;
        return true;
    case _(DeviceFeatures):
        *value = vfs->DeviceFeaturesSel == 0
                     ? VFS_FEATURES_0
                     : (vfs->DeviceFeaturesSel == 1 ? VFS_FEATURES_1 : 0);
        return true;
    case _(QueueNumMax):
        *value = VFS_QUEUE_NUM_MAX;
        return true;
    case _(QueueReady):
        *value = VFS_QUEUE.ready ? 1 : 0;
        return true;
    case _(InterruptStatus):
        *value = vfs->InterruptStatus;
        return true;
    case _(Status):
        *value = vfs->Status;
        return true;
    case _(ConfigGeneration):
        *value = 0;  // usually static
        return true;
    case NUM_REQUEST_QUEUES_ADDR:
        *value = ((uint32_t *) PRIV(vfs))[addr - _(Config)];
        return true;
    default:
        if (!RANGE_CHECK((addr >> 2), _(Config),
                         sizeof(struct virtio_fs_config)))
            return false;
        uint32_t cfg_offset = addr - ((_(Config)) << 2);
        uint8_t *cfg_bytes = (uint8_t *) PRIV(vfs);
        *value = cfg_bytes[cfg_offset];

        return true;
    }
#undef _
}

static bool virtio_fs_reg_write(virtio_fs_state_t *vfs,
                                uint32_t addr,
                                uint32_t value)
{
#define _(reg) VIRTIO_##reg
    switch (addr) {
    case _(DeviceFeaturesSel):
        vfs->DeviceFeaturesSel = value;
        return true;
    case _(DriverFeatures):
        if (vfs->DriverFeaturesSel == 0)
            vfs->DriverFeatures = value;
        return true;
    case _(DriverFeaturesSel):
        vfs->DriverFeaturesSel = value;
        return true;
    case _(QueueSel):
        if (value < ARRAY_SIZE(vfs->queues)) {
            vfs->QueueSel = value;
        } else
            virtio_fs_set_fail(vfs);
        return true;
    case _(QueueNum):
        if (value > 0 && value <= VFS_QUEUE_NUM_MAX) {
            VFS_QUEUE.QueueNum = value;
        } else
            virtio_fs_set_fail(vfs);
        return true;
    case _(QueueReady):
        VFS_QUEUE.ready = value & 1;
        if (value & 1)
            VFS_QUEUE.last_avail = vfs->ram[VFS_QUEUE.QueueAvail] >> 16;
        return true;
    case _(QueueDescLow):
        VFS_QUEUE.QueueDesc = vfs_preprocess(vfs, value);
        return true;
    case _(QueueDescHigh):
        if (value) {
            virtio_fs_set_fail(vfs);
        }
        return true;
    case _(QueueDriverLow):
        VFS_QUEUE.QueueAvail = vfs_preprocess(vfs, value);
        return true;
    case _(QueueDriverHigh):
        if (value) {
            virtio_fs_set_fail(vfs);
        }
        return true;
    case _(QueueDeviceLow):
        VFS_QUEUE.QueueUsed = vfs_preprocess(vfs, value);
        return true;
    case _(QueueDeviceHigh):
        if (value) {
            virtio_fs_set_fail(vfs);
        }
        return true;
    case _(QueueNotify):
        if (value < ARRAY_SIZE(vfs->queues)) {
            virtio_queue_notify_handler(vfs, value);
        } else
            virtio_fs_set_fail(vfs);
        return true;
    case _(InterruptACK):
        vfs->InterruptStatus &= ~value;
        return true;
    case _(Status):
        virtio_fs_update_status(vfs, value);
        return true;
    default:
        /* Invalid address which exceeded the range */
        if (!RANGE_CHECK(addr, _(Config), sizeof(struct virtio_fs_config)))
            return false;

        /* Write configuration to the corresponding register */
        ((uint32_t *) PRIV(vfs))[addr - _(Config)] = value;

        return true;
    }
#undef _
}

void virtio_fs_read(hart_t *vm,
                    virtio_fs_state_t *vfs,
                    uint32_t addr,
                    uint8_t width,
                    uint32_t *value)
{
    switch (width) {
    case RV_MEM_LW:
        if (!virtio_fs_reg_read(vfs, addr >> 2, value)) {
            vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
        }
        break;
    case RV_MEM_LBU:
    case RV_MEM_LB:
        if (!virtio_fs_reg_read(vfs, addr, value)) {
            vm_set_exception(vm, RV_EXC_LOAD_FAULT, vm->exc_val);
        }
        break;
    case RV_MEM_LHU:
    case RV_MEM_LH:
        vm_set_exception(vm, RV_EXC_LOAD_MISALIGN, vm->exc_val);
        break;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        break;
    }
}

void virtio_fs_write(hart_t *vm,
                     virtio_fs_state_t *vfs,
                     uint32_t addr,
                     uint8_t width,
                     uint32_t value)
{
    switch (width) {
    case RV_MEM_SW:
        if (!virtio_fs_reg_write(vfs, addr >> 2, value)) {
            vm_set_exception(vm, RV_EXC_STORE_FAULT, vm->exc_val);
        }
        break;
    case RV_MEM_SB:
    case RV_MEM_SH:
        vm_set_exception(vm, RV_EXC_STORE_MISALIGN, vm->exc_val);
        break;
    default:
        vm_set_exception(vm, RV_EXC_ILLEGAL_INSN, 0);
        break;
    }
}

bool virtio_fs_init(virtio_fs_state_t *vfs, char *mtag, char *dir)
{
    if (vfs_dev_cnt >= VFS_DEV_CNT_MAX) {
        fprintf(stderr,
                "Exceeded the number of virtio-fs devices that can be "
                "allocated.\n");
        exit(2);
    }

    vfs->priv = &vfs_configs[vfs_dev_cnt++];

    int dir_fd = open(dir, O_RDONLY);
    if (dir_fd < 0) {
        fprintf(stderr, "Could not open directory: %s\n", dir);
        exit(2);
    }
    strncpy(vfs->shared_dir, dir, sizeof(vfs->shared_dir) - 1);
    vfs->shared_dir[sizeof(vfs->shared_dir) - 1] = '\0';

    snprintf(PRIV(vfs)->tag, sizeof(PRIV(vfs)->tag), "%s", mtag);
    PRIV(vfs)->num_request_queues = 3;
    vfs->mount_tag = mtag;

    inode_map_entry *root_entry = malloc(sizeof(inode_map_entry));
    if (!root_entry) {
        return -1;
    }
    root_entry->ino = 1;
    strncpy(root_entry->path, vfs->shared_dir, sizeof(root_entry->path));
    HASH_ADD_INT(vfs->inode_map, ino, root_entry);

    return true;
}
