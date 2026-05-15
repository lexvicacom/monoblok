#include "snapshot.h"

#include "array.h"
#include "fs.h"
#include "slice.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    SNAP_VERSION = 3,
    SNAP_KIND_LVC = 0x01,
    SNAP_KIND_RULE_STATE = 0x02,
    SNAP_MAX_FIELD = 16 * 1024 * 1024,
};

static bool append_bytes(mb_buf *buf, const void *ptr, size_t len)
{
    return mb_buf_append(buf, ptr, len);
}

static bool append_u8(mb_buf *buf, uint8_t v)
{
    return mb_buf_append_byte(buf, v);
}

static bool append_u32(mb_buf *buf, uint32_t v)
{
    const uint8_t bytes[4] = {
        (uint8_t)(v & 0xff),
        (uint8_t)((v >> 8) & 0xff),
        (uint8_t)((v >> 16) & 0xff),
        (uint8_t)((v >> 24) & 0xff),
    };
    return append_bytes(buf, bytes, sizeof bytes);
}

static bool append_u64(mb_buf *buf, uint64_t v)
{
    uint8_t bytes[8];
    for (size_t i = 0; i < sizeof bytes; i += 1)
    {
        bytes[i] = (uint8_t)((v >> (i * 8)) & 0xff);
    }
    return append_bytes(buf, bytes, sizeof bytes);
}

static bool append_f64(mb_buf *buf, double v)
{
    uint64_t bits = 0;
    memcpy(&bits, &v, sizeof bits);
    return append_u64(buf, bits);
}

static bool append_len_prefixed(mb_buf *buf, mb_slice s)
{
    if (s.len > SNAP_MAX_FIELD)
    {
        return false;
    }
    return append_u32(buf, (uint32_t)s.len) && append_bytes(buf, s.ptr, s.len);
}

static bool read_u8(const uint8_t *bytes, size_t len, size_t *pos, uint8_t *out)
{
    if (*pos >= len)
    {
        return false;
    }
    *out = bytes[*pos];
    *pos += 1;
    return true;
}

static bool read_u32(const uint8_t *bytes, size_t len, size_t *pos, uint32_t *out)
{
    if (len - *pos < 4)
    {
        return false;
    }
    *out = (uint32_t)bytes[*pos] |
           ((uint32_t)bytes[*pos + 1] << 8) |
           ((uint32_t)bytes[*pos + 2] << 16) |
           ((uint32_t)bytes[*pos + 3] << 24);
    *pos += 4;
    return true;
}

static bool read_u64(const uint8_t *bytes, size_t len, size_t *pos, uint64_t *out)
{
    if (len - *pos < 8)
    {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < 8; i += 1)
    {
        v |= (uint64_t)bytes[*pos + i] << (i * 8);
    }
    *pos += 8;
    *out = v;
    return true;
}

static bool read_f64(const uint8_t *bytes, size_t len, size_t *pos, double *out)
{
    uint64_t bits = 0;
    if (!read_u64(bytes, len, pos, &bits))
    {
        return false;
    }
    memcpy(out, &bits, sizeof bits);
    return true;
}

static bool skip_bytes(size_t len, size_t *pos, size_t n)
{
    if (len - *pos < n)
    {
        return false;
    }
    *pos += n;
    return true;
}

static bool checked_field_bytes(uint32_t count, size_t elem_size, size_t *out)
{
    if (elem_size == 0 || count > SNAP_MAX_FIELD / elem_size)
    {
        return false;
    }
    *out = (size_t)count * elem_size;
    return true;
}

static bool read_len_prefixed(const uint8_t *bytes, size_t len, size_t *pos, mb_slice *out)
{
    uint32_t n = 0;
    if (!read_u32(bytes, len, pos, &n) || n > SNAP_MAX_FIELD || len - *pos < n)
    {
        return false;
    }
    *out = (mb_slice){.ptr = bytes + *pos, .len = n};
    *pos += n;
    return true;
}

static bool skip_rule_state(const uint8_t *bytes, size_t len, size_t *pos)
{
    uint32_t n = 0;
    uint8_t variant = 0;
    mb_slice ignored = {0};
    if (!read_u32(bytes, len, pos, &n) ||
        !read_len_prefixed(bytes, len, pos, &ignored) ||
        !read_len_prefixed(bytes, len, pos, &ignored) ||
        !read_u8(bytes, len, pos, &variant))
    {
        return false;
    }
    switch (variant)
    {
    case 0x00:
        return true;
    case 0x01:
        return read_len_prefixed(bytes, len, pos, &ignored);
    case 0x02:
        return skip_bytes(len, pos, 8);
    case 0x03:
    {
        uint32_t cap = 0;
        uint32_t max_len = 0;
        uint32_t min_len = 0;
        size_t cap_bytes = 0;
        size_t max_bytes = 0;
        size_t min_bytes = 0;
        return read_u32(bytes, len, pos, &cap) &&
               checked_field_bytes(cap, 8, &cap_bytes) &&
               skip_bytes(len, pos, 8 + 8 + cap_bytes) &&
               read_u32(bytes, len, pos, &max_len) &&
               checked_field_bytes(max_len, 8, &max_bytes) &&
               skip_bytes(len, pos, max_bytes) &&
               read_u32(bytes, len, pos, &min_len) &&
               checked_field_bytes(min_len, 8, &min_bytes) &&
               skip_bytes(len, pos, min_bytes);
    }
    case 0x04:
        return skip_bytes(len, pos, 8 + 8 + 8 + 4 + 4 + 8 + 8 + 8);
    case 0x05:
        if (!skip_bytes(len, pos, 8) || !read_u32(bytes, len, pos, &n))
        {
            return false;
        }
        size_t sample_bytes = 0;
        return checked_field_bytes(n, 16, &sample_bytes) && skip_bytes(len, pos, sample_bytes);
    default:
        return false;
    }
}

static bool split_key(mb_slice key, mb_slice *op, mb_slice *subject)
{
    for (size_t i = 0; i < key.len; i += 1)
    {
        if (key.ptr[i] == ':')
        {
            *op = (mb_slice){.ptr = key.ptr, .len = i};
            *subject = (mb_slice){.ptr = key.ptr + i + 1, .len = key.len - i - 1};
            return true;
        }
    }
    return false;
}

static pb_eval_state_entry *append_state(pb_program *program, uint32_t rule_idx, mb_slice filter, mb_slice key)
{
    if (program == NULL || rule_idx >= program->len)
    {
        return NULL;
    }
    pb_rule *rule = &program->rules[rule_idx];
    if (!mb_slice_eq((mb_slice){.ptr = (const uint8_t *)rule->filter.ptr, .len = rule->filter.len}, filter))
    {
        return NULL;
    }
    mb_slice op = {0};
    mb_slice subject = {0};
    if (!split_key(key, &op, &subject) ||
        !mb_array_reserve((void **)&rule->state.items, &rule->state.cap, rule->state.len + 1,
                          sizeof rule->state.items[0], 8))
    {
        return NULL;
    }
    pb_eval_state_entry *entry = &rule->state.items[rule->state.len];
    *entry = (pb_eval_state_entry){.rule_id = rule_idx};
    uint8_t *op_ptr = NULL;
    uint8_t *subject_ptr = NULL;
    if (!mb_slice_dup(op, &op_ptr, &entry->op_len) ||
        !mb_slice_dup(subject, &subject_ptr, &entry->subject_len))
    {
        free(op_ptr);
        free(subject_ptr);
        *entry = (pb_eval_state_entry){0};
        return NULL;
    }
    entry->op = (char *)op_ptr;
    entry->subject = (char *)subject_ptr;
    rule->state.len += 1;
    return entry;
}

static bool load_rule_state(const uint8_t *bytes, size_t len, size_t *pos, pb_program *program, size_t *loaded)
{
    uint32_t rule_idx = 0;
    uint8_t variant = 0;
    mb_slice filter = {0};
    mb_slice key = {0};
    if (!read_u32(bytes, len, pos, &rule_idx) ||
        !read_len_prefixed(bytes, len, pos, &filter) ||
        !read_len_prefixed(bytes, len, pos, &key) ||
        !read_u8(bytes, len, pos, &variant))
    {
        return false;
    }
    pb_eval_state_entry *entry = append_state(program, rule_idx, filter, key);
    switch (variant)
    {
    case 0x00:
        if (entry != NULL)
        {
            entry->kind = PB_EVAL_STATE_EMPTY;
            *loaded += 1;
        }
        return true;
    case 0x01:
    {
        mb_slice data = {0};
        if (!read_len_prefixed(bytes, len, pos, &data))
        {
            return false;
        }
        if (entry != NULL)
        {
            entry->bytes = malloc(data.len == 0 ? 1 : data.len);
            if (entry->bytes == NULL)
                return false;
            memcpy(entry->bytes, data.ptr, data.len);
            entry->bytes_len = data.len;
            entry->bytes_cap = data.len;
            entry->kind = PB_EVAL_STATE_BYTES;
            *loaded += 1;
        }
        return true;
    }
    case 0x02:
    {
        double n = 0;
        if (!read_f64(bytes, len, pos, &n))
        {
            return false;
        }
        if (entry != NULL)
        {
            entry->kind = PB_EVAL_STATE_NUMBER;
            entry->number = n;
            *loaded += 1;
        }
        return true;
    }
    case 0x03:
    {
        uint32_t cap = 0;
        uint64_t counter = 0;
        double sum = 0;
        uint32_t max_len = 0;
        uint32_t min_len = 0;
        if (!read_u32(bytes, len, pos, &cap) ||
            !read_u64(bytes, len, pos, &counter) ||
            !read_f64(bytes, len, pos, &sum))
        {
            return false;
        }
        size_t values_pos = *pos;
        if (!skip_bytes(len, pos, (size_t)cap * 8) ||
            !read_u32(bytes, len, pos, &max_len) ||
            !skip_bytes(len, pos, (size_t)max_len * 8) ||
            !read_u32(bytes, len, pos, &min_len) ||
            !skip_bytes(len, pos, (size_t)min_len * 8))
        {
            return false;
        }
        (void)counter;
        if (entry != NULL)
        {
            entry->ring_values = calloc(cap == 0 ? 1 : cap, sizeof entry->ring_values[0]);
            if (entry->ring_values == NULL)
                return false;
            for (uint32_t i = 0; i < cap; i += 1)
            {
                size_t p = values_pos + (size_t)i * 8;
                if (!read_f64(bytes, len, &p, &entry->ring_values[i]))
                    return false;
            }
            entry->kind = PB_EVAL_STATE_RING;
            entry->ring_cap = cap;
            entry->ring_len = cap;
            entry->ring_sum = sum;
            *loaded += 1;
        }
        return true;
    }
    case 0x04:
    {
        double open = 0;
        double high = 0;
        double low = 0;
        uint32_t count = 0;
        uint32_t cap = 0;
        uint64_t window_ms = 0;
        uint64_t window_start_ms = 0;
        double last_close = 0;
        if (!read_f64(bytes, len, pos, &open) ||
            !read_f64(bytes, len, pos, &high) ||
            !read_f64(bytes, len, pos, &low) ||
            !read_u32(bytes, len, pos, &count) ||
            !read_u32(bytes, len, pos, &cap) ||
            !read_u64(bytes, len, pos, &window_ms) ||
            !read_u64(bytes, len, pos, &window_start_ms) ||
            !read_f64(bytes, len, pos, &last_close))
        {
            return false;
        }
        if (entry != NULL)
        {
            entry->kind = PB_EVAL_STATE_BAR;
            entry->bar_open = open;
            entry->bar_high = high;
            entry->bar_low = low;
            entry->bar_count = count;
            entry->bar_cap = cap;
            entry->bar_window_ms = window_ms;
            entry->bar_window_start_ms = window_start_ms;
            entry->bar_last_close = last_close;
            entry->bar_time_window = window_ms != 0;
            *loaded += 1;
        }
        return true;
    }
    case 0x05:
    {
        uint64_t window_ms = 0;
        uint32_t n = 0;
        if (!read_u64(bytes, len, pos, &window_ms) || !read_u32(bytes, len, pos, &n))
        {
            return false;
        }
        size_t samples_pos = *pos;
        if (!skip_bytes(len, pos, (size_t)n * 16))
        {
            return false;
        }
        if (entry != NULL)
        {
            entry->ring_values = calloc(n == 0 ? 1 : n, sizeof entry->ring_values[0]);
            entry->ring_times_ms = calloc(n == 0 ? 1 : n, sizeof entry->ring_times_ms[0]);
            if (entry->ring_values == NULL || entry->ring_times_ms == NULL)
                return false;
            double sum = 0;
            for (uint32_t i = 0; i < n; i += 1)
            {
                size_t p = samples_pos + (size_t)i * 16;
                uint64_t ts = 0;
                if (!read_f64(bytes, len, &p, &entry->ring_values[i]) ||
                    !read_u64(bytes, len, &p, &ts))
                {
                    return false;
                }
                entry->ring_times_ms[i] = ts;
                sum += entry->ring_values[i];
            }
            entry->kind = PB_EVAL_STATE_RING;
            entry->ring_cap = n;
            entry->ring_len = n;
            entry->ring_sum = sum;
            entry->ring_window_ms = window_ms;
            entry->ring_time_window = true;
            *loaded += 1;
        }
        return true;
    }
    default:
        return false;
    }
}

bool mb_snapshot_load(const char *path, mb_router *router, pb_program *program, mb_snapshot_counts *counts)
{
    *counts = (mb_snapshot_counts){0};
    mb_buf file = {0};
    errno = 0;
    if (!mb_read_file(path, &file))
    {
        if (errno == ENOENT)
        {
            return true;
        }
        perror(path);
        return false;
    }

    const uint8_t *bytes = file.ptr;
    const size_t len = file.len;
    if (len < 8 || memcmp(bytes, "MBLK", 4) != 0 || bytes[4] != SNAP_VERSION)
    {
        fprintf(stderr, "snapshot: ignoring unsupported snapshot %s\n", path);
        mb_buf_free(&file);
        return true;
    }

    size_t pos = 8;
    bool ok = true;
    while (ok && pos < len)
    {
        uint8_t kind = 0;
        ok = read_u8(bytes, len, &pos, &kind);
        if (!ok)
        {
            break;
        }
        if (kind == SNAP_KIND_LVC)
        {
            mb_slice subject = {0};
            mb_slice payload = {0};
            ok = read_len_prefixed(bytes, len, &pos, &subject) &&
                 read_len_prefixed(bytes, len, &pos, &payload) &&
                 mb_router_store_lvc(router, subject, payload);
            if (ok)
            {
                counts->lvc += 1;
            }
        }
        else if (kind == SNAP_KIND_RULE_STATE)
        {
            if (program == NULL)
            {
                ok = skip_rule_state(bytes, len, &pos);
            }
            else
            {
                ok = load_rule_state(bytes, len, &pos, program, &counts->rule_state);
            }
        }
        else
        {
            ok = false;
        }
    }
    if (!ok)
    {
        fprintf(stderr, "snapshot: failed to parse %s\n", path);
    }
    mb_buf_free(&file);
    return ok;
}

static bool append_key(mb_buf *out, const pb_eval_state_entry *entry)
{
    const size_t total = entry->op_len + 1 + entry->subject_len;
    if (total > SNAP_MAX_FIELD || !append_u32(out, (uint32_t)total))
    {
        return false;
    }
    return append_bytes(out, entry->op, entry->op_len) &&
           append_u8(out, ':') &&
           append_bytes(out, entry->subject, entry->subject_len);
}

static bool append_pb_slice(mb_buf *out, pb_slice s)
{
    return append_len_prefixed(out, (mb_slice){.ptr = (const uint8_t *)s.ptr, .len = s.len});
}

static bool append_rule_state(mb_buf *out, const pb_program *program)
{
    if (program == NULL)
    {
        return true;
    }
    for (size_t rule_idx = 0; rule_idx < program->len; rule_idx += 1)
    {
        const pb_rule *rule = &program->rules[rule_idx];
        for (size_t state_idx = 0; state_idx < rule->state.len; state_idx += 1)
        {
            const pb_eval_state_entry *entry = &rule->state.items[state_idx];
            if (entry->kind == PB_EVAL_STATE_CLOCK)
            {
                continue;
            }
            bool ok = append_u8(out, SNAP_KIND_RULE_STATE) &&
                      append_u32(out, (uint32_t)rule_idx) &&
                      append_pb_slice(out, rule->filter) &&
                      append_key(out, entry);
            if (!ok)
                return false;
            switch (entry->kind)
            {
            case PB_EVAL_STATE_EMPTY:
                ok = append_u8(out, 0x00);
                break;
            case PB_EVAL_STATE_BYTES:
                ok = append_u8(out, 0x01) &&
                     append_len_prefixed(out, (mb_slice){.ptr = (const uint8_t *)entry->bytes, .len = entry->bytes_len});
                break;
            case PB_EVAL_STATE_NUMBER:
                ok = append_u8(out, 0x02) && append_f64(out, entry->number);
                break;
            case PB_EVAL_STATE_RING:
                if (entry->ring_time_window)
                {
                    ok = append_u8(out, 0x05) &&
                         append_u64(out, entry->ring_window_ms) &&
                         append_u32(out, (uint32_t)entry->ring_len);
                    for (size_t i = 0; ok && i < entry->ring_len; i += 1)
                    {
                        const size_t idx = entry->ring_cap == 0 ? 0 : (entry->ring_start + i) % entry->ring_cap;
                        ok = append_f64(out, entry->ring_values[idx]) &&
                             append_u64(out, entry->ring_times_ms[idx]);
                    }
                }
                else
                {
                    ok = append_u8(out, 0x03) &&
                         append_u32(out, (uint32_t)entry->ring_len) &&
                         append_u64(out, (uint64_t)entry->ring_len) &&
                         append_f64(out, entry->ring_sum);
                    for (size_t i = 0; ok && i < entry->ring_len; i += 1)
                    {
                        const size_t idx = entry->ring_cap == 0 ? 0 : (entry->ring_start + i) % entry->ring_cap;
                        ok = append_f64(out, entry->ring_values[idx]);
                    }
                    ok = ok && append_u32(out, 0) && append_u32(out, 0);
                }
                break;
            case PB_EVAL_STATE_BAR:
                ok = append_u8(out, 0x04) &&
                     append_f64(out, entry->bar_open) &&
                     append_f64(out, entry->bar_high) &&
                     append_f64(out, entry->bar_low) &&
                     append_u32(out, entry->bar_count) &&
                     append_u32(out, entry->bar_cap) &&
                     append_u64(out, entry->bar_time_window ? entry->bar_window_ms : 0) &&
                     append_u64(out, entry->bar_window_start_ms) &&
                     append_f64(out, entry->bar_last_close);
                break;
            case PB_EVAL_STATE_CLOCK:
                ok = true;
                break;
            }
            if (!ok)
                return false;
        }
    }
    return true;
}

bool mb_snapshot_build(mb_buf *out, const mb_router *router, const pb_program *program)
{
    bool ok = append_bytes(out, "MBLK", 4) &&
              append_u8(out, SNAP_VERSION) &&
              append_bytes(out, "\0\0\0", 3);
    for (size_t i = 0; ok && i < mb_router_lvc_count(router); i += 1)
    {
        mb_slice subject = {0};
        mb_slice payload = {0};
        ok = mb_router_lvc_entry(router, i, &subject, &payload) &&
             append_u8(out, SNAP_KIND_LVC) &&
             append_len_prefixed(out, subject) &&
             append_len_prefixed(out, payload);
    }
    return ok && append_rule_state(out, program);
}

bool mb_snapshot_write_bytes(const char *path, const mb_buf *snapshot)
{
    if (snapshot == NULL)
    {
        return false;
    }
    const bool ok = mb_write_file_atomic(path, (mb_slice){.ptr = snapshot->ptr, .len = snapshot->len});
    if (!ok)
    {
        perror(path);
    }
    return ok;
}

bool mb_snapshot_write(const char *path, const mb_router *router, const pb_program *program)
{
    mb_buf out = {0};
    bool ok = mb_snapshot_build(&out, router, program);
    if (!ok)
    {
        mb_buf_free(&out);
        return false;
    }
    ok = mb_snapshot_write_bytes(path, &out);
    mb_buf_free(&out);
    return ok;
}
