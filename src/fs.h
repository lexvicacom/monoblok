#ifndef MB_FS_H
#define MB_FS_H

#include "proto.h"
#include "buf.h"

#include <stdbool.h>

// Read an entire file into a growable buffer.
// Existing buffer contents are preserved and appended to.
bool mb_read_file(const char *path, mb_buf *out);

// Atomically replace a file by writing to a temporary sibling and renaming.
// Intended for snapshots and future persistent state/config writes.
bool mb_write_file_atomic(const char *path, mb_slice bytes);

#endif
