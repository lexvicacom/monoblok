#ifndef MB_TEST_TEMP_H
#define MB_TEST_TEMP_H

#include "test_check.h"

#include <uv.h>

// Owns a unique temporary directory and the test file inside it.
typedef struct mb_test_temp {
    char dir[4096];
    char path[4096];
} mb_test_temp;

static void mb_test_temp_init(mb_test_temp *temp) {
    char root[4096];
    size_t size = sizeof root;
    CHECK(uv_os_tmpdir(root, &size) == 0);
    int len = snprintf(temp->dir, sizeof temp->dir, "%s/monoblok-test-XXXXXX", root);
    CHECK(len >= 0 && (size_t)len < sizeof temp->dir);

    uv_fs_t req = {0};
    const int result = uv_fs_mkdtemp(NULL, &req, temp->dir, NULL);
    if (result == 0) {
        len = snprintf(temp->dir, sizeof temp->dir, "%s", req.path);
    }
    uv_fs_req_cleanup(&req);
    CHECK(result == 0);
    CHECK(len >= 0 && (size_t)len < sizeof temp->dir);
    len = snprintf(temp->path, sizeof temp->path, "%s/file", temp->dir);
    CHECK(len >= 0 && (size_t)len < sizeof temp->path);
}

static void mb_test_temp_free(mb_test_temp *temp) {
    CHECK(remove(temp->path) == 0);
    uv_fs_t req = {0};
    const int result = uv_fs_rmdir(NULL, &req, temp->dir, NULL);
    uv_fs_req_cleanup(&req);
    CHECK(result == 0);
}

#endif
