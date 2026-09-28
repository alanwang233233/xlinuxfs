#include "lkl_fskit.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "FAIL line %d: %s (errno=%d)\n", __LINE__, #test, e); exit(1); } } while (0)

int main(int argc, char **argv)
{
    int e = 0;
    CHECK(argc == 3);
    const int writable = strcmp(argv[2], "rw") == 0;
    void *backend = lkfs_backend_from_file(argv[1], writable, &e);
    CHECK(backend != NULL);
    char label[260], type[16];
    unsigned char uuid[16];
    CHECK(lkfs_probe(backend, label, sizeof(label), type, sizeof(type), uuid) == 1);
    lkfs_volume *volume = lkfs_mount(backend, !writable, &e);
    CHECK(volume != NULL);
    lkfs_statfs_t stats;
    CHECK(lkfs_statfs(volume, &stats) == 0);
    CHECK(stats.total_blocks > 0 && stats.block_size > 0);
    const char *payload = "xlinuxfs platform persistence\n";
    const char *name = "platform-persistent.txt";
    if (writable) {
        uint64_t ino = lkfs_create(volume, LKFS_ROOT_INO, "platform-staging.txt", LKFS_TYPE_FILE, &e);
        CHECK(ino != 0);
        CHECK(lkfs_write(volume, ino, 0, payload, strlen(payload), &e) == (int64_t)strlen(payload));
        CHECK(lkfs_rename(volume, LKFS_ROOT_INO, "platform-staging.txt", LKFS_ROOT_INO, name) == 0);
        char buffer[128] = {0};
        CHECK(lkfs_read(volume, ino, 0, buffer, sizeof(buffer), &e) == (int64_t)strlen(payload));
        CHECK(strcmp(buffer, payload) == 0);
    } else {
        CHECK(lkfs_create(volume, LKFS_ROOT_INO, "must-not-write.txt", LKFS_TYPE_FILE, &e) == 0);
        CHECK(e == EROFS);
        if (strcmp(argv[2], "verify") == 0) {
            uint64_t ino = lkfs_lookup(volume, LKFS_ROOT_INO, name, &e);
            CHECK(ino != 0);
            char buffer[128] = {0};
            CHECK(lkfs_read(volume, ino, 0, buffer, sizeof(buffer), &e) == (int64_t)strlen(payload));
            CHECK(strcmp(buffer, payload) == 0);
        }
    }
    CHECK(lkfs_umount(volume) == 0);
    lkfs_backend_free(backend);
    printf("PASS %s %s: mount, access, I/O, unmount\n", type, argv[2]);
    return 0;
}
