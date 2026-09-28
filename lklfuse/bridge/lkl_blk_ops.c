/*
 * lkl_blk_ops.c - struct lkl_dev_blk_ops that backs LKL's virtio-blk device with
 * an FSKit/file backend (lkfs_block_*), instead of a raw fd. Used by lkl_fskit.c
 * via disk.ops; disk.handle carries the opaque backend pointer.
 *
 * Compiled against the LKL host headers (tools/lkl/include). Mirrors the default
 * blk_request() in posix-host.c, but each iovec segment goes through
 * lkfs_block_pread/pwrite (which handles sector alignment for real block devices).
 */

#include <lkl_host.h>
#include <sys/uio.h>
#include "lkl_fskit.h"

static int fskit_get_capacity(struct lkl_disk disk, unsigned long long *res)
{
    *res = lkfs_block_total_bytes(disk.handle);
    return 0;
}

static int fskit_blk_request(struct lkl_disk disk, struct lkl_blk_req *req)
{
    void *backend = disk.handle;
    long long off = (long long)req->sector * 512;
    int i;

    switch (req->type) {
    case LKL_DEV_BLK_TYPE_READ:
        for (i = 0; i < req->count; i++) {
            int64_t n = lkfs_block_pread(backend, req->buf[i].iov_base,
                                         off, (int64_t)req->buf[i].iov_len);
            if (n != (int64_t)req->buf[i].iov_len)
                return LKL_DEV_BLK_STATUS_IOERR;
            off += req->buf[i].iov_len;
        }
        break;
    case LKL_DEV_BLK_TYPE_WRITE:
        for (i = 0; i < req->count; i++) {
            int64_t n = lkfs_block_pwrite(backend, req->buf[i].iov_base,
                                          off, (int64_t)req->buf[i].iov_len);
            if (n != (int64_t)req->buf[i].iov_len)
                return LKL_DEV_BLK_STATUS_IOERR;
            off += req->buf[i].iov_len;
        }
        break;
    case LKL_DEV_BLK_TYPE_FLUSH:
    case LKL_DEV_BLK_TYPE_FLUSH_OUT:
        if (lkfs_block_sync(backend) != 0)
            return LKL_DEV_BLK_STATUS_IOERR;
        break;
    default:
        return LKL_DEV_BLK_STATUS_UNSUP;
    }

    return LKL_DEV_BLK_STATUS_OK;
}

/* Referenced by lkl_fskit.c when it builds the struct lkl_disk for lkl_disk_add. */
struct lkl_dev_blk_ops lkl_fskit_blk_ops = {
    .get_capacity = fskit_get_capacity,
    .request      = fskit_blk_request,
};
