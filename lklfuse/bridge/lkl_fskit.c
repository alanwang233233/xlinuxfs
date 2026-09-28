/*
 * lkl_fskit.c - the lkfs_* FSKit bridge implemented over the in-process LKL
 * kernel (POSIX syscalls). FSKit is inode-keyed; LKL is path-based, so the
 * volume keeps an ino->path map populated by lookup()/readdir(). The real Linux
 * st_ino is the reported FSItem id, except the root which is LKFS_ROOT_INO.
 *
 * The LKL kernel is a process-global singleton: started once, then each volume
 * adds its own virtio-blk disk (backed by an lkfs backend) and mounts it under
 * /mnt/<id>. Filesystem type is detected from the superblock magic so neither
 * probe nor mount needs to guess.
 */

#include <lkl.h>
#include <lkl_host.h>
#include "lkl_fskit.h"

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

extern struct lkl_dev_blk_ops lkl_fskit_blk_ops;   /* lkl_blk_ops.c */

/* ------------------------------------------------------------------ */
/* Process-global LKL kernel (started once).                           */
/* ------------------------------------------------------------------ */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_started;

static int ensure_kernel(void)
{
    int ret = 0;
    pthread_mutex_lock(&g_lock);
    if (!g_started) {
        if (lkl_init(&lkl_host_ops) < 0) { ret = -ENOMEM; goto out; }
        ret = lkl_start_kernel("mem=64M");
        if (ret) { ret = -EIO; goto out; }
        g_started = 1;
    }
out:
    pthread_mutex_unlock(&g_lock);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Superblock probe (no LKL needed) — fstype + label from magic.       */
/* ------------------------------------------------------------------ */
static uint16_t rd_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Fills fstype ("ext4"/"xfs"/"btrfs"), label, and the 16-byte native fs UUID
 * (uuid_out may be NULL); returns 1 if recognized. */
static int detect_fs(void *backend, char *fstype, size_t fcap, char *label, size_t lcap, uint8_t *uuid_out)
{
    uint8_t buf[4096];
    if (label && lcap) label[0] = 0;
    if (uuid_out) memset(uuid_out, 0, 16);

    /* ext2/3/4: magic 0xEF53 at byte 1024+56; label at 1024+120 (16 bytes); s_uuid at 1024+0x68. */
    if (lkfs_block_pread(backend, buf, 1024, 256) == 256 && rd_le16(buf + 56) == 0xEF53) {
        snprintf(fstype, fcap, "ext4");
        if (label && lcap) { memcpy(label, buf + 120, 16 < lcap ? 16 : lcap - 1); label[16 < lcap ? 16 : lcap - 1] = 0; }
        if (uuid_out) memcpy(uuid_out, buf + 0x68, 16);
        return 1;
    }
    /* XFS: magic "XFSB" at byte 0; sb_uuid at 0x20; label sb_fname at 0x6c (12 bytes). */
    if (lkfs_block_pread(backend, buf, 0, 512) == 512 && memcmp(buf, "XFSB", 4) == 0) {
        snprintf(fstype, fcap, "xfs");
        if (label && lcap) { size_t n = 12 < lcap - 1 ? 12 : lcap - 1; memcpy(label, buf + 0x6c, n); label[n] = 0; }
        if (uuid_out) memcpy(uuid_out, buf + 0x20, 16);
        return 1;
    }
    /* Btrfs: magic "_BHRfS_M" at byte 0x10040; fsid at 0x10000+0x20; label at 0x10000+0x12b (256). */
    if (lkfs_block_pread(backend, buf, 0x10000, 4096) == 4096 && memcmp(buf + 0x40, "_BHRfS_M", 8) == 0) {
        snprintf(fstype, fcap, "btrfs");
        if (label && lcap) { strncpy(label, (char *)buf + 0x12b, lcap - 1); label[lcap - 1] = 0; }
        if (uuid_out) memcpy(uuid_out, buf + 0x20, 16);
        return 1;
    }
    return 0;
}

int lkfs_probe(void *backend, char *name_out, size_t name_cap, char *fstype_out, size_t fstype_cap, uint8_t uuid_out[16])
{
    char fstype[16];
    char label[260];
    if (!detect_fs(backend, fstype, sizeof(fstype), label, sizeof(label), uuid_out))
        return 0;
    if (name_out && name_cap) { strncpy(name_out, label, name_cap - 1); name_out[name_cap - 1] = 0; }
    if (fstype_out && fstype_cap) { strncpy(fstype_out, fstype, fstype_cap - 1); fstype_out[fstype_cap - 1] = 0; }
    return 1;
}

/* ------------------------------------------------------------------ */
/* ino -> path map (per volume).                                       */
/* ------------------------------------------------------------------ */
#define IMAP_BUCKETS 2048
struct ino_node { uint64_t ino; char *path; struct ino_node *next; };

struct lkfs_volume {
    char mountpoint[64];
    char fstype[16];
    char label[256];
    uint8_t uuid[16];
    unsigned int disk_id;
    struct lkl_disk disk;   /* as populated by lkl_disk_add; .dev needed by lkl_disk_remove */
    void *backend;
    int read_only;
    struct ino_node *buckets[IMAP_BUCKETS];
};

static struct ino_node *map_find(lkfs_volume *v, uint64_t ino)
{
    struct ino_node *n = v->buckets[ino % IMAP_BUCKETS];
    for (; n; n = n->next)
        if (n->ino == ino) return n;
    return NULL;
}

static const char *map_get(lkfs_volume *v, uint64_t ino)
{
    struct ino_node *n = map_find(v, ino);
    return n ? n->path : NULL;
}

/* Record an ino->path mapping. An inode may be reachable by several names (hard
 * links), so dedup on (ino, path) rather than ino: every distinct path gets a node,
 * and removing one name (map_forget_subtree) leaves the inode's other names in place,
 * so a held handle still resolves to a surviving link. */
static void map_put(lkfs_volume *v, uint64_t ino, const char *path)
{
    for (struct ino_node *it = v->buckets[ino % IMAP_BUCKETS]; it; it = it->next)
        if (it->ino == ino && strcmp(it->path, path) == 0) return;
    struct ino_node *n = malloc(sizeof(*n));
    if (!n) return;
    n->ino = ino;
    n->path = strdup(path);
    n->next = v->buckets[ino % IMAP_BUCKETS];
    v->buckets[ino % IMAP_BUCKETS] = n;
}

static void map_free(lkfs_volume *v)
{
    for (int i = 0; i < IMAP_BUCKETS; i++) {
        struct ino_node *n = v->buckets[i];
        while (n) { struct ino_node *x = n->next; free(n->path); free(n); n = x; }
        v->buckets[i] = NULL;
    }
}

/* Remove the cached entry for `prefix` and anything under `prefix/`. Used after
 * a remove (unlink/rmdir) and to drop an overwritten rename target, so a stale
 * held inode can't keep resolving to — or be confused with — a recreated path. */
static void map_forget_subtree(lkfs_volume *v, const char *prefix)
{
    size_t plen = strlen(prefix);
    for (int i = 0; i < IMAP_BUCKETS; i++) {
        struct ino_node **pp = &v->buckets[i];
        while (*pp) {
            const char *p = (*pp)->path;
            if (strcmp(p, prefix) == 0 || (strncmp(p, prefix, plen) == 0 && p[plen] == '/')) {
                struct ino_node *dead = *pp;
                *pp = dead->next;
                free(dead->path);
                free(dead);
            } else {
                pp = &(*pp)->next;
            }
        }
    }
}

/* After renaming old_prefix -> new_prefix, re-point every cached path that is
 * old_prefix itself or lives under old_prefix/ so a held FSItem (keyed by ino)
 * stays resolvable. The bucket is keyed by ino, not path, so mutating the path
 * in place keeps the node in its bucket. */
static void map_rebase(lkfs_volume *v, const char *old_prefix, const char *new_prefix)
{
    size_t oldlen = strlen(old_prefix);
    for (int i = 0; i < IMAP_BUCKETS; i++) {
        for (struct ino_node *n = v->buckets[i]; n; n = n->next) {
            char *np = NULL;
            if (strcmp(n->path, old_prefix) == 0) {
                np = strdup(new_prefix);
            } else if (strncmp(n->path, old_prefix, oldlen) == 0 && n->path[oldlen] == '/') {
                const char *tail = n->path + oldlen;   /* includes the leading '/' */
                size_t need = strlen(new_prefix) + strlen(tail) + 1;
                np = malloc(need);
                if (np) snprintf(np, need, "%s%s", new_prefix, tail);
            }
            if (np) { free(n->path); n->path = np; }
        }
    }
}

/* Max path length we build/store; Linux PATH_MAX. */
#define LKFS_PATH_MAX 4096

/* Build "<dir path>/<name>" into out. Returns 0, or -1 if it would truncate. */
static int join(char *out, size_t cap, const char *dir, const char *name)
{
    int n = (strcmp(dir, "/") == 0) ? snprintf(out, cap, "/%s", name)
                                    : snprintf(out, cap, "%s/%s", dir, name);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

/* Map a held ino to its path, or NULL. */
static const char *ino_path(lkfs_volume *v, uint64_t ino)
{
    if (ino == LKFS_ROOT_INO) return v->mountpoint;
    return map_get(v, ino);
}

/* Reported ino for a real st_ino (root collapses to LKFS_ROOT_INO). */
static uint64_t report_ino(lkfs_volume *v, uint64_t real, const char *path)
{
    if (strcmp(path, v->mountpoint) == 0) return LKFS_ROOT_INO;
    return real;
}

/* ------------------------------------------------------------------ */
/* mount / umount / statfs / sync                                      */
/* ------------------------------------------------------------------ */
/* Remove a disk (added via lkl_disk_add) under the kernel lock. */
static void disk_remove_locked(struct lkl_disk disk)
{
    pthread_mutex_lock(&g_lock);
    lkl_disk_remove(disk);
    pthread_mutex_unlock(&g_lock);
}

lkfs_volume *lkfs_mount(void *backend, bool read_only, int *out_errno)
{
    char fstype[16], label[260];
    uint8_t uuid[16];
    if (!detect_fs(backend, fstype, sizeof(fstype), label, sizeof(label), uuid)) {
        if (out_errno) *out_errno = EINVAL;
        return NULL;
    }
    int e = ensure_kernel();
    if (e) { if (out_errno) *out_errno = -e; return NULL; }

    struct lkl_disk disk;
    memset(&disk, 0, sizeof(disk));
    disk.handle = backend;
    disk.ops = &lkl_fskit_blk_ops;

    pthread_mutex_lock(&g_lock);
    int disk_id = lkl_disk_add(&disk);
    pthread_mutex_unlock(&g_lock);
    if (disk_id < 0) { if (out_errno) *out_errno = ENXIO; return NULL; }

    lkfs_volume *v = calloc(1, sizeof(*v));
    if (!v) { disk_remove_locked(disk); if (out_errno) *out_errno = ENOMEM; return NULL; }
    v->backend = backend;
    v->disk_id = (unsigned int)disk_id;
    v->disk = disk;
    v->read_only = read_only ? 1 : 0;
    strncpy(v->fstype, fstype, sizeof(v->fstype) - 1);
    strncpy(v->label, label, sizeof(v->label) - 1);
    memcpy(v->uuid, uuid, 16);

    /* A read-only mount must not replay a dirty journal/log: recovery needs to write,
     * which fails outright on read-only media (the reproduced "recovery required on
     * readonly filesystem" -> superblock/journal write I/O errors). Skip recovery so the
     * mount is truly write-free — XFS takes `norecovery`, ext2/3/4 take `noload`; both give
     * a last-checkpoint read-only view. A clean fs is unaffected, and a read-write mount
     * still recovers normally. Btrfs also needs nologreplay: MS_RDONLY alone
     * does not suppress tree-log recovery. */
    const char *data = NULL;
    if (read_only) {
        if (strcmp(fstype, "xfs") == 0) data = "norecovery";
        else if (strcmp(fstype, "ext4") == 0) data = "noload";
        else if (strcmp(fstype, "btrfs") == 0) data = "nologreplay";
    }
    long rc = lkl_mount_dev(v->disk_id, 0, fstype,
                            read_only ? LKL_MS_RDONLY : 0, data,
                            v->mountpoint, sizeof(v->mountpoint));
    if (rc) {
        disk_remove_locked(disk);
        free(v);
        if (out_errno) *out_errno = (int)-rc;
        return NULL;
    }
    map_put(v, LKFS_ROOT_INO, v->mountpoint);
    return v;
}

/* Tears down a volume. Returns 0 only when the LKL disk was actually removed — in
 * which case `v` is freed and the caller MUST now release the backend. On any
 * failure it returns a positive errno, does NOT remove the disk, and does NOT free
 * `v`: the disk stays registered and keeps referencing the backend via disk.handle,
 * so the caller must keep BOTH the handle and the backend alive (freeing the backend
 * would dangle; removing the disk would surprise-abort the fs). */
int lkfs_umount(lkfs_volume *v)
{
    if (!v) return 0;
    pthread_mutex_lock(&g_lock);
    /* Commit dirty data, then unmount. */
    lkl_sys_sync();
    long rc = lkl_umount_dev(v->disk_id, 0, 0, 2000);
    if (rc != 0) {                       /* still mounted — keep the disk + backend */
        pthread_mutex_unlock(&g_lock);
        return (int)-rc ? (int)-rc : EIO;
    }
    /* umount() detaches the mount, but LKL's cooperative kernel runs the superblock
     * teardown (jbd2 commit + ext4_put_super writing the clean flag, then releasing
     * the block device) as DEFERRED work. Removing the disk before that finishes is
     * a surprise removal that aborts the journal and leaves the fs needing recovery.
     * Wait DETERMINISTICALLY rather than by a fixed sleep: a block device still held
     * by the fs fails O_EXCL open with EBUSY; O_EXCL succeeds only once the fs has
     * released it — i.e. teardown is complete and the fs is marked clean. Poll that
     * (yielding 1ms so the kernel drains), bounded. The mountpoint is "/mnt/%08x"
     * where %08x is the block dev id. */
    unsigned int dev = (unsigned int)strtoul(v->mountpoint + 5, NULL, 16);
    char devp[16];
    snprintf(devp, sizeof(devp), "/dev/%08x", dev);
    lkl_sys_mknod(devp, LKL_S_IFBLK | 0600, dev);
    struct __lkl__kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1ms */
    int released = 0;
    for (int i = 0; i < 2000; i++) {     /* bounded ~2s; normally ready in ~15ms */
        long fd = lkl_sys_open(devp, LKL_O_RDONLY | LKL_O_EXCL, 0);
        if (fd >= 0) { lkl_sys_close((unsigned)fd); released = 1; break; }
        lkl_sys_nanosleep(&ts, NULL);
    }
    lkl_sys_unlink(devp);
    if (!released) {                     /* bdev still held — do NOT surprise-remove */
        pthread_mutex_unlock(&g_lock);
        return EBUSY;
    }
    lkl_disk_remove(v->disk);
    pthread_mutex_unlock(&g_lock);
    map_free(v);
    free(v);
    return 0;
}

int lkfs_sync(lkfs_volume *v)
{
    if (!v) return -EINVAL;
    lkl_sys_sync();
    return 0;
}

int lkfs_statfs(lkfs_volume *v, lkfs_statfs_t *out)
{
    if (!v || !out) return -EINVAL;
    struct lkl_statfs st;
    memset(out, 0, sizeof(*out));
    long rc = lkl_sys_statfs(v->mountpoint, &st);
    if (rc) return (int)rc;
    out->block_size = (uint32_t)st.f_bsize;
    out->total_blocks = (uint64_t)st.f_blocks;
    out->free_blocks = (uint64_t)st.f_bfree;
    out->total_files = (uint64_t)st.f_files;
    out->free_files = (uint64_t)st.f_ffree;
    out->read_only = v->read_only;
    strncpy(out->fs_type, v->fstype, sizeof(out->fs_type) - 1);
    strncpy(out->volume_name, v->label, sizeof(out->volume_name) - 1);
    memcpy(out->uuid, v->uuid, 16);
    return 0;
}

/* ------------------------------------------------------------------ */
/* attr helpers                                                        */
/* ------------------------------------------------------------------ */
static uint32_t mode_to_type(uint32_t mode)
{
    switch (mode & LKL_S_IFMT) {
    case LKL_S_IFDIR:  return LKFS_TYPE_DIR;
    case LKL_S_IFREG:  return LKFS_TYPE_FILE;
    case LKL_S_IFLNK:  return LKFS_TYPE_SYMLINK;
    case LKL_S_IFIFO:  return LKFS_TYPE_FIFO;
    case LKL_S_IFCHR:  return LKFS_TYPE_CHARDEV;
    case LKL_S_IFBLK:  return LKFS_TYPE_BLOCKDEV;
    case LKL_S_IFSOCK: return LKFS_TYPE_SOCKET;
    default:           return LKFS_TYPE_UNKNOWN;
    }
}

static uint32_t dtype_to_type(unsigned char d_type)
{
    switch (d_type) {
    case 4:  return LKFS_TYPE_DIR;      /* DT_DIR */
    case 8:  return LKFS_TYPE_FILE;     /* DT_REG */
    case 10: return LKFS_TYPE_SYMLINK;  /* DT_LNK */
    case 1:  return LKFS_TYPE_FIFO;     /* DT_FIFO */
    case 2:  return LKFS_TYPE_CHARDEV;  /* DT_CHR */
    case 6:  return LKFS_TYPE_BLOCKDEV; /* DT_BLK */
    case 12: return LKFS_TYPE_SOCKET;   /* DT_SOCK */
    default: return LKFS_TYPE_UNKNOWN;
    }
}

static void fill_attr(lkfs_volume *v, const struct lkl_stat *st, const char *path, lkfs_attr_t *a)
{
    memset(a, 0, sizeof(*a));
    a->ino = report_ino(v, (uint64_t)st->st_ino, path);
    a->parent_ino = 0;
    a->type = mode_to_type(st->st_mode);
    a->mode = st->st_mode & 07777;
    a->nlink = st->st_nlink;
    a->size = (uint64_t)st->st_size;
    a->alloc_size = (uint64_t)st->st_blocks * 512;
    a->mtime_sec = st->lkl_st_mtime; a->mtime_nsec = st->st_mtime_nsec;
    a->atime_sec = st->lkl_st_atime; a->atime_nsec = st->st_atime_nsec;
    a->ctime_sec = st->lkl_st_ctime; a->ctime_nsec = st->st_ctime_nsec;
    a->btime_sec = st->lkl_st_ctime; a->btime_nsec = st->st_ctime_nsec; /* no btime in lkl_stat */
}

int lkfs_getattr(lkfs_volume *v, uint64_t ino, lkfs_attr_t *out)
{
    const char *path = ino_path(v, ino);
    if (!path) return -ENOENT;
    struct lkl_stat st;
    long rc = lkl_sys_lstat(path, &st);
    if (rc) return (int)rc;
    fill_attr(v, &st, path, out);
    return 0;
}

uint64_t lkfs_lookup(lkfs_volume *v, uint64_t dir_ino, const char *name, int *out_errno)
{
    const char *dir = ino_path(v, dir_ino);
    if (!dir) { if (out_errno) *out_errno = ENOENT; return 0; }
    char path[LKFS_PATH_MAX];
    if (join(path, sizeof(path), dir, name)) { if (out_errno) *out_errno = ENAMETOOLONG; return 0; }
    struct lkl_stat st;
    long rc = lkl_sys_lstat(path, &st);
    if (rc) { if (out_errno) *out_errno = (int)-rc; return 0; }
    uint64_t ino = report_ino(v, (uint64_t)st.st_ino, path);
    map_put(v, ino, path);
    return ino;
}

/* ------------------------------------------------------------------ */
/* readdir (getdents64) — struct lkl_linux_dirent64 comes from lkl headers */
/* ------------------------------------------------------------------ */
int lkfs_readdir(lkfs_volume *v, uint64_t dir_ino, int64_t start_cookie, void *ctx, lkfs_dir_cb cb)
{
    const char *dir = ino_path(v, dir_ino);
    if (!dir) return -ENOENT;
    long fd = lkl_sys_open(dir, LKL_O_RDONLY | LKL_O_DIRECTORY, 0);
    if (fd < 0) return (int)fd;

    char buf[8192];
    int64_t index = 0;       /* 0-based position among non-dot entries */
    int rc = 0;
    for (;;) {
        long n = lkl_sys_getdents64((unsigned int)fd, (struct lkl_linux_dirent64 *)buf, sizeof(buf));
        if (n < 0) { rc = (int)n; break; }
        if (n == 0) break;
        for (long off = 0; off < n; ) {
            struct lkl_linux_dirent64 *d = (struct lkl_linux_dirent64 *)(buf + off);
            off += d->d_reclen;
            if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
                continue;
            if (index++ < start_cookie)
                continue;
            char child[LKFS_PATH_MAX];
            if (join(child, sizeof(child), dir, d->d_name)) { rc = -ENAMETOOLONG; goto done; }
            uint64_t ino = report_ino(v, d->d_ino, child);
            map_put(v, ino, child);
            if (cb(ctx, d->d_name, ino, dtype_to_type(d->d_type), index) != 0)
                goto done;
        }
    }
done:
    lkl_sys_close((unsigned int)fd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* read / write                                                        */
/* ------------------------------------------------------------------ */
int64_t lkfs_read(lkfs_volume *v, uint64_t ino, int64_t offset, void *buf, int64_t len, int *out_errno)
{
    const char *path = ino_path(v, ino);
    if (!path) { if (out_errno) *out_errno = ENOENT; return -1; }
    long fd = lkl_sys_open(path, LKL_O_RDONLY, 0);
    if (fd < 0) { if (out_errno) *out_errno = (int)-fd; return -1; }
    if (lkl_sys_lseek((unsigned int)fd, offset, LKL_SEEK_SET) < 0) {
        if (out_errno) *out_errno = EIO; lkl_sys_close((unsigned int)fd); return -1;
    }
    long n = lkl_sys_read((unsigned int)fd, buf, (size_t)len);
    lkl_sys_close((unsigned int)fd);
    if (n < 0) { if (out_errno) *out_errno = (int)-n; return -1; }
    return n;
}

int64_t lkfs_write(lkfs_volume *v, uint64_t ino, int64_t offset, const void *buf, int64_t len, int *out_errno)
{
    const char *path = ino_path(v, ino);
    if (!path) { if (out_errno) *out_errno = ENOENT; return -1; }
    long fd = lkl_sys_open(path, LKL_O_RDWR, 0);
    if (fd < 0) { if (out_errno) *out_errno = (int)-fd; return -1; }
    if (lkl_sys_lseek((unsigned int)fd, offset, LKL_SEEK_SET) < 0) {
        if (out_errno) *out_errno = EIO; lkl_sys_close((unsigned int)fd); return -1;
    }
    long n = lkl_sys_write((unsigned int)fd, buf, (size_t)len);
    lkl_sys_close((unsigned int)fd);
    if (n < 0) { if (out_errno) *out_errno = (int)-n; return -1; }
    return n;
}

/* ------------------------------------------------------------------ */
/* create / remove / truncate / set_times / rename / readlink         */
/* ------------------------------------------------------------------ */
uint64_t lkfs_create(lkfs_volume *v, uint64_t dir_ino, const char *name, uint32_t type, int *out_errno)
{
    const char *dir = ino_path(v, dir_ino);
    if (!dir) { if (out_errno) *out_errno = ENOENT; return 0; }
    char path[LKFS_PATH_MAX];
    if (join(path, sizeof(path), dir, name)) { if (out_errno) *out_errno = ENAMETOOLONG; return 0; }
    long rc;
    if (type == LKFS_TYPE_DIR) {
        rc = lkl_sys_mkdir(path, 0755);
    } else {
        long fd = lkl_sys_open(path, LKL_O_CREAT | LKL_O_EXCL | LKL_O_WRONLY, 0644);
        if (fd >= 0) { lkl_sys_close((unsigned int)fd); rc = 0; } else rc = fd;
    }
    if (rc) { if (out_errno) *out_errno = (int)-rc; return 0; }
    struct lkl_stat st;
    if (lkl_sys_lstat(path, &st)) { if (out_errno) *out_errno = EIO; return 0; }
    uint64_t ino = report_ino(v, (uint64_t)st.st_ino, path);
    map_put(v, ino, path);
    return ino;
}

uint64_t lkfs_symlink(lkfs_volume *v, uint64_t dir_ino, const char *name, const char *target, int *out_errno)
{
    const char *dir = ino_path(v, dir_ino);
    if (!dir) { if (out_errno) *out_errno = ENOENT; return 0; }
    char path[LKFS_PATH_MAX];
    if (join(path, sizeof(path), dir, name)) { if (out_errno) *out_errno = ENAMETOOLONG; return 0; }
    long rc = lkl_sys_symlink(target, path);   /* symlink(target, linkpath) */
    if (rc) { if (out_errno) *out_errno = (int)-rc; return 0; }
    struct lkl_stat st;
    if (lkl_sys_lstat(path, &st)) { if (out_errno) *out_errno = EIO; return 0; }
    uint64_t ino = report_ino(v, (uint64_t)st.st_ino, path);
    map_put(v, ino, path);
    return ino;
}

/* Hard link: give the inode behind `target_ino` an additional name `name` in dir_ino.
 * The new path is recorded for the (same) inode so a held handle survives unlinking
 * either name. ext/XFS/Btrfs support hard links; the kernel refuses them on dirs. */
int lkfs_link(lkfs_volume *v, uint64_t target_ino, uint64_t dir_ino, const char *name, int *out_errno)
{
    const char *target = ino_path(v, target_ino);
    const char *dir = ino_path(v, dir_ino);
    if (!target || !dir) { if (out_errno) *out_errno = ENOENT; return -1; }
    /* FSKit expects ENOTSUP when hard links aren't supported for the object's type (a dir);
       the kernel would otherwise return EPERM for link(2) on a directory. */
    struct lkl_stat st;
    if (lkl_sys_lstat(target, &st) == 0 && LKL_S_ISDIR(st.st_mode)) {
        if (out_errno) *out_errno = ENOTSUP; return -1;
    }
    char path[LKFS_PATH_MAX];
    if (join(path, sizeof(path), dir, name)) { if (out_errno) *out_errno = ENAMETOOLONG; return -1; }
    long rc = lkl_sys_link(target, path);   /* link(oldpath=target, newpath=path) */
    if (rc) { if (out_errno) *out_errno = (int)-rc; return -1; }
    map_put(v, target_ino, path);
    return 0;
}

int lkfs_remove(lkfs_volume *v, uint64_t dir_ino, const char *name)
{
    const char *dir = ino_path(v, dir_ino);
    if (!dir) return -ENOENT;
    char path[LKFS_PATH_MAX];
    if (join(path, sizeof(path), dir, name)) return -ENAMETOOLONG;
    struct lkl_stat st;
    long lrc = lkl_sys_lstat(path, &st);
    if (lrc) return (int)lrc;   /* propagate the real errno (EIO/ENOENT/…), not a blanket ENOENT */
    long rc = LKL_S_ISDIR(st.st_mode) ? lkl_sys_rmdir(path) : lkl_sys_unlink(path);
    if (rc == 0) map_forget_subtree(v, path);
    return (int)rc;
}

int lkfs_truncate(lkfs_volume *v, uint64_t ino, uint64_t size)
{
    const char *path = ino_path(v, ino);
    if (!path) return -ENOENT;
    return (int)lkl_sys_truncate(path, (long)size);
}

/* Apply the POSIX permission bits requested by create / setAttributes (mode is
 * masked to the 07777 perm/setid/sticky bits; the file type is untouched). */
int lkfs_chmod(lkfs_volume *v, uint64_t ino, uint32_t mode)
{
    const char *path = ino_path(v, ino);
    if (!path) return -ENOENT;
    /* The kernel fchmodat syscall is 3-arg (dfd, path, mode) — no flags (that's fchmodat2). */
    return (int)lkl_sys_fchmodat(LKL_AT_FDCWD, path, mode & 07777);
}

int lkfs_set_times(lkfs_volume *v, uint64_t ino,
                   int64_t mtime_sec, int64_t mtime_nsec,
                   int64_t atime_sec, int64_t atime_nsec)
{
    const char *path = ino_path(v, ino);
    if (!path) return -ENOENT;
    const long UTIME_OMIT_ = ((1L << 30) - 2);   /* skip this timestamp */
    struct __lkl__kernel_timespec ts[2];
    /* [0]=atime [1]=mtime */
    if (atime_sec == INT64_MIN) { ts[0].tv_sec = 0; ts[0].tv_nsec = UTIME_OMIT_; }
    else { ts[0].tv_sec = atime_sec; ts[0].tv_nsec = atime_nsec; }
    if (mtime_sec == INT64_MIN) { ts[1].tv_sec = 0; ts[1].tv_nsec = UTIME_OMIT_; }
    else { ts[1].tv_sec = mtime_sec; ts[1].tv_nsec = mtime_nsec; }
    return (int)lkl_sys_utimensat(LKL_AT_FDCWD, path, ts, 0);
}

int lkfs_rename(lkfs_volume *v, uint64_t src_dir, const char *src_name,
                uint64_t dst_dir, const char *dst_name)
{
    const char *sd = ino_path(v, src_dir), *dd = ino_path(v, dst_dir);
    if (!sd || !dd) return -ENOENT;
    char sp[LKFS_PATH_MAX], dp[LKFS_PATH_MAX];
    if (join(sp, sizeof(sp), sd, src_name)) return -ENAMETOOLONG;
    if (join(dp, sizeof(dp), dd, dst_name)) return -ENAMETOOLONG;
    long rc = lkl_sys_rename(sp, dp);
    if (rc == 0) {
        /* The moved inode (and any cached descendants) now live at dp; re-point
         * their cached paths so a held FSItem stays resolvable. Drop any stale
         * entry (or subtree) for an overwritten destination first. */
        map_forget_subtree(v, dp);
        map_rebase(v, sp, dp);
    }
    return (int)rc;
}

int lkfs_readlink(lkfs_volume *v, uint64_t ino, char *buf, size_t cap)
{
    const char *path = ino_path(v, ino);
    if (!path) return -ENOENT;
    long n = lkl_sys_readlink(path, buf, cap);   /* fill up to cap; n==cap means it was truncated */
    if (n < 0) return (int)n;
    if ((size_t)n >= cap) return -ENAMETOOLONG;  /* target needs >= cap bytes, no room for the NUL */
    buf[n] = 0;
    return 0;
}
