#include <stdlib.h>
#include <stdio.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/vfs.h>
#include <time.h>

#include "cutils/properties.h"

#include <fs_mgr.h>
#include "roots.h"

#include "bu.h"

#include "voldclient.h"

using namespace android;

static int append_sod(const char* opt_hash)
{
    const char* key;
    char value[PROPERTY_VALUE_MAX];
    int len;
    char buf[PROP_LINE_LEN];
    char sodbuf[PROP_LINE_LEN*10];
    char* p = sodbuf;

    key = "hash.name";
    strcpy(value, opt_hash);
    p += sprintf(p, "%s=%s\n", key, value);

    key = "ro.product.device";
    property_get(key, value, "");
    p += sprintf(p, "%s=%s\n", key, value);

    for (int i = 0; i < MAX_PART; ++i) {
        partspec* part = part_get(i);
        if (!part)
            break;
        int fd = open(part->vol->blk_device, O_RDONLY);
        part->size = part->used = lseek64(fd, 0, SEEK_END);
        close(fd);
        p += sprintf(p, "fs.%s.size=%llu\n", part->name, part->size);
        p += sprintf(p, "fs.%s.used=%llu\n", part->name, part->used);
    }

    int rc = tar_append_file_contents(tar, "SOD", 0600,
            getuid(), getgid(), sodbuf, p-sodbuf);
    return rc;
}

static int append_eod(const char* opt_hash)
{
    char buf[PROP_LINE_LEN];
    char eodbuf[PROP_LINE_LEN*10];
    char* p = eodbuf;
    int n;

    p += sprintf(p, "hash.datalen=%u\n", hash_datalen);

    unsigned char digest[HASH_MAX_LENGTH];
    char hexdigest[HASH_MAX_STRING_LENGTH];

    if (!strcasecmp(opt_hash, "sha1")) {
        SHA1_Final(digest, &sha_ctx);
        for (n = 0; n < SHA_DIGEST_LENGTH; ++n) {
            sprintf(hexdigest+2*n, "%02x", digest[n]);
        }
        p += sprintf(p, "hash.value=%s\n", hexdigest);
    }
    else { // default to md5
        MD5_Final(digest, &md5_ctx);
        for (n = 0; n < MD5_DIGEST_LENGTH; ++n) {
            sprintf(hexdigest+2*n, "%02x", digest[n]);
        }
        p += sprintf(p, "hash.value=%s\n", hexdigest);
    }

    int rc = tar_append_file_contents(tar, "EOD", 0600,
            getuid(), getgid(), eodbuf, p-eodbuf);
    return rc;
}

static int tar_append_device_contents(TAR* t, const char* devname, const char* savename)
{
    struct stat st;
    memset(&st, 0, sizeof(st));
    if (lstat(devname, &st) != 0) {
        logmsg("tar_append_device_contents: lstat %s failed\n", devname);
        return -1;
    }
    st.st_mode = 0644 | S_IFREG;

    int fd = open(devname, O_RDONLY);
    if (fd < 0) {
        logmsg("tar_append_device_contents: open %s failed\n", devname);
        return -1;
    }
    st.st_size = lseek64(fd, 0, SEEK_END);
    close(fd);

    th_set_from_stat(t, &st);
    th_set_path(t, savename);
    if (th_write(t) != 0) {
        logmsg("tar_append_device_contents: th_write failed\n");
        return -1;
    }
    if (tar_append_regfile(t, devname) != 0) {
        logmsg("tar_append_device_contents: tar_append_regfile %s failed\n", devname);
        return -1;
    }
    return 0;
}

int do_backup(int argc, char **argv)
{
    int rc = 1;
    int n;
    int i;

    int len;
    int written;

    const char* opt_compress = "gzip";
    const char* opt_hash = "md5";

    int optidx = 0;
    while (optidx < argc && argv[optidx][0] == '-' && argv[optidx][1] == '-') {
        char* optname = &argv[optidx][2];
        ++optidx;
        char* optval = strchr(optname, '=');
        if (optval) {
            *optval = '\0';
            ++optval;
        }
        else {
            if (optidx >= argc) {
                logmsg("No argument to --%s\n", optname);
                return -1;
            }
            optval = argv[optidx];
            ++optidx;
        }
        if (!strcmp(optname, "compress")) {
            opt_compress = optval;
            logmsg("do_backup: compress=%s\n", opt_compress);
        }
        else if (!strcmp(optname, "hash")) {
            opt_hash = optval;
            logmsg("do_backup: hash=%s\n", opt_hash);
        }
        else {
            logmsg("do_backup: invalid option name \"%s\"\n", optname);
            return -1;
        }
    }
    for (n = optidx; n < argc; ++n) {
        const char* partname = argv[n];
        if (*partname == '-')
            ++partname;
        if (part_add(partname) != 0) {
            logmsg("Failed to add partition %s\n", partname);
            return -1;
        }
    }

    rc = create_tar(adb_ofd, opt_compress, "w");
    if (rc != 0) {
        logmsg("do_backup: cannot open tar stream\n");
        return rc;
    }

    append_sod(opt_hash);

    hash_name = strdup(opt_hash);

    for (i = 0; i < MAX_PART; ++i) {
        partspec* curpart = part_get(i);
        if (!curpart)
            break;

        part_set(curpart);
        rc = tar_append_device_contents(tar, curpart->vol->blk_device, curpart->name);
    }

    free(hash_name);
    hash_name = NULL;

    append_eod(opt_hash);

    tar_append_eof(tar);

    if (opt_compress)
        gzflush(gzf, Z_FINISH);

    logmsg("backup complete: rc=%d\n", rc);

    return rc;
}

