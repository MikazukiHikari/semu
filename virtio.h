#pragma once

#define VIRTIO_VENDOR_ID 0x12345678

#define VIRTIO_STATUS__DRIVER_OK 4
#define VIRTIO_STATUS__DEVICE_NEEDS_RESET 64

#define VIRTIO_INT__USED_RING 1
#define VIRTIO_INT__CONF_CHANGE 2

#define VIRTIO_DESC_F_NEXT 1
#define VIRTIO_DESC_F_WRITE 2

#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_T_FLUSH 4
#define VIRTIO_BLK_T_GET_ID 8
#define VIRTIO_BLK_T_GET_LIFETIME 10
#define VIRTIO_BLK_T_DISCARD 11
#define VIRTIO_BLK_T_WRITE_ZEROES 13
#define VIRTIO_BLK_T_SECURE_ERASE 14

#define VIRTIO_BLK_S_OK 0
#define VIRTIO_BLK_S_IOERR 1
#define VIRTIO_BLK_S_UNSUPP 2

#define FUSE_INIT 26
#define FUSE_GETATTR 3
#define FUSE_OPENDIR 27
#define FUSE_READDIRPLUS 44
#define FUSE_LOOKUP 1
#define FUSE_FORGET 2
#define FUSE_RELEASEDIR 29
#define FUSE_OPEN 14
#define FUSE_READ 15
#define FUSE_RELEASE 18
#define FUSE_FLUSH 25
#define FUSE_DESTROY 38


#define FUSE_ASYNC_READ (1 << 0)   // Supports asynchronous read requests
#define FUSE_POSIX_LOCKS (1 << 1)  // Supports POSIX file locking
#define FUSE_FILE_OPS (1 << 2)  // Handles open/release operations in userspace
#define FUSE_ATOMIC_O_TRUNC (1 << 3)  // Supports atomic O_TRUNC during open
#define FUSE_EXPORT_SUPPORT \
    (1 << 4)  // Allows file system to be exported (e.g., via NFS)
#define FUSE_BIG_WRITES (1 << 5)      // Supports writes larger than 4KB
#define FUSE_DONT_MASK (1 << 6)       // Do not mask file mode bits
#define FUSE_SPLICE_WRITE (1 << 7)    // Supports splice() for writing
#define FUSE_SPLICE_MOVE (1 << 8)     // Supports splice() for moving data
#define FUSE_SPLICE_READ (1 << 9)     // Supports splice() for reading
#define FUSE_FLOCK_LOCKS (1 << 10)    // Supports flock() file locking
#define FUSE_HAS_IOCTL_DIR (1 << 11)  // Supports ioctl() on directories
#define FUSE_AUTO_INVAL_DATA \
    (1 << 12)  // Automatically invalidate cache on write/change
#define FUSE_DO_READDIRPLUS \
    (1 << 13)  // Supports readdirplus (dir entry + attributes)
#define FUSE_READDIRPLUS_AUTO (1 << 14)  // Automatically use readdirplus
#define FUSE_ASYNC_DIO (1 << 15)         // Supports asynchronous direct I/O
#define FUSE_WRITEBACK_CACHE (1 << 16)   // Enables kernel writeback caching
#define FUSE_NO_OPEN_SUPPORT (1 << 17)   // No open/release support needed
#define FUSE_PARALLEL_DIROPS (1 << 18)   // Allows parallel directory operations
#define FUSE_HANDLE_KILLPRIV \
    (1 << 19)  // Supports killing privilege bits (e.g., setuid)
#define FUSE_POSIX_ACL (1 << 20)       // Supports POSIX ACLs
#define FUSE_ABORT_ERROR (1 << 21)     // Supports returning abort errors
#define FUSE_MAX_PAGES (1 << 22)       // Supports max_pages parameter
#define FUSE_CACHE_SYMLINKS (1 << 23)  // Caches symbolic links
#define FUSE_NO_OPENDIR_SUPPORT \
    (1 << 24)  // No opendir/releasedir support needed



/* VirtIO MMIO registers */
#define VIRTIO_REG_LIST                  \
    _(MagicValue, 0x000)        /* R */  \
    _(Version, 0x004)           /* R */  \
    _(DeviceID, 0x008)          /* R */  \
    _(VendorID, 0x00c)          /* R */  \
    _(DeviceFeatures, 0x010)    /* R */  \
    _(DeviceFeaturesSel, 0x014) /* W */  \
    _(DriverFeatures, 0x020)    /* W */  \
    _(DriverFeaturesSel, 0x024) /* W */  \
    _(QueueSel, 0x030)          /* W */  \
    _(QueueNumMax, 0x034)       /* R */  \
    _(QueueNum, 0x038)          /* W */  \
    _(QueueReady, 0x044)        /* RW */ \
    _(QueueNotify, 0x050)       /* W */  \
    _(InterruptStatus, 0x60)    /* R */  \
    _(InterruptACK, 0x064)      /* W */  \
    _(Status, 0x070)            /* RW */ \
    _(QueueDescLow, 0x080)      /* W */  \
    _(QueueDescHigh, 0x084)     /* W */  \
    _(QueueDriverLow, 0x090)    /* W */  \
    _(QueueDriverHigh, 0x094)   /* W */  \
    _(QueueDeviceLow, 0x0a0)    /* W */  \
    _(QueueDeviceHigh, 0x0a4)   /* W */  \
    _(ConfigGeneration, 0x0fc)  /* R */  \
    _(Config, 0x100)            /* RW */

enum {
#define _(reg, addr) VIRTIO_##reg = addr >> 2,
    VIRTIO_REG_LIST
#undef _
};

PACKED(struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
});
