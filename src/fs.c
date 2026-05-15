#include "fs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

bool mb_read_file(const char *path, mb_buf *out) {
    if (path == NULL || out == NULL) {
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }

    unsigned char tmp[4096];
    bool ok = true;

    while (!feof(f)) {
        const size_t n = fread(tmp, 1, sizeof tmp, f);
        if (n != 0 && !mb_buf_append(out, tmp, n)) {
            ok = false;
            break;
        }
        if (ferror(f)) {
            ok = false;
            break;
        }
    }

    fclose(f);
    return ok;
}

bool mb_write_file_atomic(const char *path, mb_slice bytes) {
    if (path == NULL) {
        return false;
    }
    if (bytes.len != 0 && bytes.ptr == NULL) {
        return false;
    }

    char tmp_path[PATH_MAX];
    const int rc = snprintf(tmp_path, sizeof tmp_path, "%s.tmp", path);
    if (rc < 0 || (size_t)rc >= sizeof tmp_path) {
        return false;
    }

    FILE *f = fopen(tmp_path, "wb");
    if (f == NULL) {
        return false;
    }

    bool ok = true;

    if (bytes.len != 0 && fwrite(bytes.ptr, 1, bytes.len, f) != bytes.len) {
        ok = false;
    }

    if (ok && fflush(f) != 0) {
        ok = false;
    }

#ifndef _WIN32
    if (ok && fsync(fileno(f)) != 0) {
        ok = false;
    }
#endif

    if (fclose(f) != 0) {
        ok = false;
    }

    if (!ok) {
        remove(tmp_path);
        return false;
    }

    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        return false;
    }

    return true;
}
