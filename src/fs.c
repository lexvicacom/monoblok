#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    if (fclose(f) != 0) {
        ok = false;
    }
    return ok;
}

bool mb_write_file_atomic(const char *path, mb_slice bytes) {
    if (path == NULL) {
        return false;
    }
    if (bytes.len != 0 && bytes.ptr == NULL) {
        return false;
    }

    const size_t path_len = strlen(path);
    if (path_len > SIZE_MAX - 5) {
        return false;
    }

    char *tmp_path = malloc(path_len + 5);
    if (tmp_path == NULL) {
        return false;
    }
    memcpy(tmp_path, path, path_len);
    memcpy(tmp_path + path_len, ".tmp", 5);

    FILE *f = fopen(tmp_path, "wb");
    if (f == NULL) {
        free(tmp_path);
        return false;
    }

    bool ok = true;

    if (bytes.len != 0 && fwrite(bytes.ptr, 1, bytes.len, f) != bytes.len) {
        ok = false;
    }

    if (ok && fflush(f) != 0) {
        ok = false;
    }

    if (fclose(f) != 0) {
        ok = false;
    }

    if (!ok) {
        remove(tmp_path);
        free(tmp_path);
        return false;
    }

    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        free(tmp_path);
        return false;
    }

    free(tmp_path);
    return true;
}
