/*
 * lkl_file_backend.c - a pure-C, image-file implementation of the lkfs backend
 * I/O surface, for the CLI test harness (no FSKit). The .appex uses
 * linux_device_fskit.m instead, which implements the same symbols over an
 * FSBlockDeviceResource (and also files). Link exactly one of the two.
 */

#include "lkl_fskit.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

struct file_backend { int fd; uint64_t size; int writable; };

void *lkfs_backend_from_file(const char *path, int writable, int *out_err)
{
    int fd = open(path, writable ? O_RDWR : O_RDONLY);
    if (fd < 0) { if (out_err) *out_err = errno; return NULL; }
    struct stat st;
    if (fstat(fd, &st) != 0) { if (out_err) *out_err = errno; close(fd); return NULL; }
    struct file_backend *b = calloc(1, sizeof(*b));
    if (!b) { close(fd); if (out_err) *out_err = ENOMEM; return NULL; }
    b->fd = fd; b->size = (uint64_t)st.st_size; b->writable = writable ? 1 : 0;
    return b;
}

void lkfs_backend_free(void *backend)
{
    struct file_backend *b = backend;
    if (!b) return;
    if (b->fd >= 0) close(b->fd);
    free(b);
}

uint32_t lkfs_block_sector_size(void *backend) { (void)backend; return 512; }
uint64_t lkfs_block_total_bytes(void *backend) { return ((struct file_backend *)backend)->size; }
int      lkfs_block_is_writable(void *backend) { return ((struct file_backend *)backend)->writable; }

int64_t lkfs_block_pread(void *backend, void *buf, int64_t offset, int64_t count)
{
    struct file_backend *b = backend;
    if (offset < 0 || count < 0) { errno = EINVAL; return -1; }
    if ((uint64_t)offset >= b->size) return 0;
    if ((uint64_t)(offset + count) > b->size) count = (int64_t)b->size - offset;
    return pread(b->fd, buf, (size_t)count, (off_t)offset);
}

int64_t lkfs_block_pwrite(void *backend, const void *buf, int64_t offset, int64_t count)
{
    struct file_backend *b = backend;
    if (!b->writable) { errno = EROFS; return -1; }
    if (offset < 0 || count < 0) { errno = EINVAL; return -1; }
    return pwrite(b->fd, buf, (size_t)count, (off_t)offset);
}

int lkfs_block_sync(void *backend)
{
    struct file_backend *b = backend;
    return fsync(b->fd) == 0 ? 0 : -errno;
}
