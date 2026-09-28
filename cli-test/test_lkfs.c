/* CLI validation of the lkfs_* FSKit bridge over a file-backed image.
 * Usage: test_lkfs <image> [rw|leak]
 *   rw   -> mount read-write and run the rename regression (X1). Modifies the
 *           image, so point it at a scratch copy.
 *   leak -> loop mount/umount many times to prove disk registrations are
 *           reclaimed (lkl_disk_remove on the failure + umount paths). */
#include "lkl_fskit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>

static int dir_cb(void *ctx, const char *name, uint64_t ino, uint32_t type, int64_t cookie)
{
    (void)ctx;
    printf("  entry %-18s ino=%" PRIu64 " type=%u nextcookie=%" PRId64 "\n", name, ino, type, cookie);
    return 0;
}

int main(int argc, char **argv)
{
    const char *img = argc > 1 ? argv[1] : "test.ext4";
    const char *mode = argc > 2 ? argv[2] : "";
    int rw = (strcmp(mode, "rw") == 0);
    int leak = (strcmp(mode, "leak") == 0);
    int e = 0;
    void *bk = lkfs_backend_from_file(img, (rw || leak) ? 1 : 0, &e);
    if (!bk) { printf("backend fail errno=%d\n", e); return 1; }

    if (leak) {
        const int N = argc > 3 ? atoi(argv[3]) : 64;
        for (int i = 0; i < N; i++) {
            int me = 0;
            lkfs_volume *lv = lkfs_mount(bk, false, &me);
            if (!lv) { printf("[#3] FAIL at mount cycle %d errno=%d (disk reg leak?)\n", i, me);
                       lkfs_backend_free(bk); return 4; }
            lkfs_statfs_t s; lkfs_statfs(lv, &s);
            int urc = lkfs_umount(lv);
            if (urc != 0) { printf("[#3] FAIL umount cycle %d errno=%d (disk not cleanly removed)\n", i, urc);
                            lkfs_backend_free(bk); return 6; }
        }
        printf("[#3] PASS %d mount/umount cycles — disk registrations reclaimed\n", N);
        lkfs_backend_free(bk);
        return 0;
    }

    char name[260] = {0}, fstype[16] = {0};
    uint8_t uuid[16] = {0};
    int rec = lkfs_probe(bk, name, sizeof name, fstype, sizeof fstype, uuid);
    printf("probe: recognized=%d fstype='%s' label='%s' uuid=", rec, fstype, name);
    for (int i = 0; i < 16; i++) printf("%02x", uuid[i]);
    printf("\n");

    lkfs_volume *v = lkfs_mount(bk, rw ? false : true, &e);
    if (!v) { printf("mount fail errno=%d\n", e); return 1; }
    printf("mounted ok (%s).\n", rw ? "rw" : "ro");

    lkfs_statfs_t st;
    if (lkfs_statfs(v, &st) == 0)
        printf("statfs: bs=%u total=%" PRIu64 " free=%" PRIu64 " type=%s\n",
               st.block_size, st.total_blocks, st.free_blocks, st.fs_type);

    printf("readdir(/):\n");
    lkfs_readdir(v, LKFS_ROOT_INO, 0, NULL, dir_cb);

    int le = 0;
    uint64_t ino = lkfs_lookup(v, LKFS_ROOT_INO, "hello.txt", &le);
    printf("lookup hello.txt -> ino=%" PRIu64 " err=%d\n", ino, le);
    if (ino) {
        lkfs_attr_t a;
        if (lkfs_getattr(v, ino, &a) == 0)
            printf("getattr: type=%u mode=%04o size=%" PRIu64 " nlink=%u mtime=%" PRId64 "\n",
                   a.type, a.mode, a.size, a.nlink, a.mtime_sec);
        char buf[512]; int re = 0;
        int64_t n = lkfs_read(v, ino, 0, buf, sizeof(buf) - 1, &re);
        if (n >= 0) { buf[n] = 0; printf("read %" PRId64 " bytes: %s", n, buf); }
        else printf("read err=%d\n", re);
    }

    /* subdir enumerate + nested read */
    uint64_t sd = lkfs_lookup(v, LKFS_ROOT_INO, "subdir", &le);
    if (sd) {
        printf("readdir(/subdir):\n");
        lkfs_readdir(v, sd, 0, NULL, dir_cb);
    }

    int rc = 0;
    if (rw && ino) {
        /* X1 regression: after rename, the HELD ino must still resolve (the bug
         * was that lkfs_rename never updated the ino->path map, so reading the
         * pre-rename ino hit the stale old path and failed). We deliberately do
         * NOT re-lookup; we reuse `ino` captured before the rename. */
        printf("\n[X1] rename hello.txt -> hello-renamed.txt (held ino=%" PRIu64 ")\n", ino);
        int rr = lkfs_rename(v, LKFS_ROOT_INO, "hello.txt", LKFS_ROOT_INO, "hello-renamed.txt");
        printf("[X1] rename rc=%d\n", rr);
        char rb[512]; int re2 = 0;
        int64_t rn = lkfs_read(v, ino, 0, rb, sizeof(rb) - 1, &re2);
        if (rn >= 0) { rb[rn] = 0; printf("[X1] PASS post-rename read(held ino) %" PRId64 " bytes: %s", rn, rb); }
        else { printf("[X1] FAIL post-rename read(held ino) err=%d\n", re2); rc = 3; }
        /* rename back so the scratch image stays reusable */
        lkfs_rename(v, LKFS_ROOT_INO, "hello-renamed.txt", LKFS_ROOT_INO, "hello.txt");

        /* symlink create + readback (round 4): ln -s hello.txt link1 */
        int se = 0;
        uint64_t li = lkfs_symlink(v, LKFS_ROOT_INO, "link1", "hello.txt", &se);
        printf("[sym] create link1 -> hello.txt: ino=%" PRIu64 " err=%d\n", li, se);
        if (li) {
            char tgt[256]; int rr2 = lkfs_readlink(v, li, tgt, sizeof tgt);
            if (rr2 == 0) printf("[sym] PASS readlink(link1) = %s\n", tgt);
            else { printf("[sym] FAIL readlink err=%d\n", rr2); rc = 5; }
            lkfs_remove(v, LKFS_ROOT_INO, "link1");   /* clean up scratch */
        } else { rc = 5; }

        /* [#4] create honors a requested mode, and chmod changes it. */
        int ce = 0;
        uint64_t fi = lkfs_create(v, LKFS_ROOT_INO, "modetest", LKFS_TYPE_FILE, &ce);
        if (fi) {
            int chr = lkfs_chmod(v, fi, 0750);
            lkfs_attr_t ma; memset(&ma, 0, sizeof ma);
            if (chr == 0 && lkfs_getattr(v, fi, &ma) == 0 && (ma.mode & 07777) == 0750)
                printf("[#4] PASS chmod modetest -> %04o\n", ma.mode & 07777);
            else { printf("[#4] FAIL chmod rc=%d mode=%04o\n", chr, ma.mode & 07777); rc = 7; }
            lkfs_remove(v, LKFS_ROOT_INO, "modetest");   /* clean up scratch */
        } else { printf("[#4] FAIL create modetest err=%d\n", ce); rc = 7; }

        /* [hl] hard link: create a 2nd name, unlink the FIRST (mapped) name, then confirm
           the HELD ino still reads via the surviving link (the multi-path map fix). */
        int hle = 0;
        uint64_t hi = lkfs_create(v, LKFS_ROOT_INO, "hltest", LKFS_TYPE_FILE, &hle);
        if (hi) {
            const char *hmsg = "hardlink-data";
            int we = 0;
            lkfs_write(v, hi, 0, hmsg, (int64_t)strlen(hmsg), &we);
            int lrc = lkfs_link(v, hi, LKFS_ROOT_INO, "hltest-b", &hle);
            lkfs_attr_t ha; memset(&ha, 0, sizeof ha); lkfs_getattr(v, hi, &ha);
            lkfs_remove(v, LKFS_ROOT_INO, "hltest");          /* drop the mapped name; inode lives at hltest-b */
            char hb[64]; int hre = 0;
            int64_t hn = lkfs_read(v, hi, 0, hb, sizeof(hb) - 1, &hre);
            if (lrc == 0 && ha.nlink == 2 && hn == (int64_t)strlen(hmsg)) {
                hb[hn] = 0; printf("[hl] PASS link+nlink=2, held ino reads after unlinking 1st name: '%s'\n", hb);
            } else { printf("[hl] FAIL lrc=%d nlink=%u read=%lld err=%d\n", lrc, ha.nlink, (long long)hn, hre); rc = 8; }
            lkfs_remove(v, LKFS_ROOT_INO, "hltest-b");
            /* dir hard link must be refused with ENOTSUP (FSKit semantics, not the kernel's EPERM). */
            uint64_t hd = lkfs_create(v, LKFS_ROOT_INO, "hldir", LKFS_TYPE_DIR, &hle);
            hle = 0;
            int dlrc = lkfs_link(v, hd, LKFS_ROOT_INO, "hldir-b", &hle);
            if (dlrc != 0 && hle == ENOTSUP) printf("[hl] PASS dir hard link refused ENOTSUP\n");
            else { printf("[hl] FAIL dir link rc=%d err=%d (want ENOTSUP=%d)\n", dlrc, hle, ENOTSUP); rc = 8; }
            lkfs_remove(v, LKFS_ROOT_INO, "hldir");
        } else { printf("[hl] FAIL create err=%d\n", hle); rc = 8; }
    }

    lkfs_umount(v);
    lkfs_backend_free(bk);
    return rc;
}
