const std = @import("std");
const Allocator = std.mem.Allocator;

const patchbay = @import("patchbay");
const subject_mod = patchbay.subject;
const rules_mod = patchbay.eval;

pub const prefix = "$LVC.";

pub const Filter = struct {
    filter: []const u8,
    tokens: []const []const u8,
    is_literal: bool,
    first_token: []const u8,

    pub fn init(arena: Allocator, filter: []const u8) !Filter {
        return .{
            .filter = filter,
            .tokens = try splitTokens(arena, filter),
            .is_literal = rules_mod.isLiteralFilter(filter),
            .first_token = firstToken(filter),
        };
    }
};

pub const Store = struct {
    gpa: Allocator,
    enabled: bool,
    last_value: std.StringHashMap(std.ArrayList(u8)),

    /// Null means legacy/global LVC when `enabled` is true. A non-null
    /// slice means only subjects matching one configured `(lvc ...)` filter
    /// participate in cache writes and live `$LVC.*` delivery.
    filters: ?[]const Filter = null,
    literal_filters: std.StringHashMapUnmanaged(void) = .empty,
    wildcard_buckets: std.StringHashMapUnmanaged(std.ArrayListUnmanaged(*const Filter)) = .empty,
    wildcard_global: std.ArrayListUnmanaged(*const Filter) = .empty,

    pub fn init(gpa: Allocator, enabled: bool) Store {
        return .{
            .gpa = gpa,
            .enabled = enabled,
            .last_value = .init(gpa),
        };
    }

    pub fn deinit(self: *Store) void {
        var lv_it = self.last_value.iterator();
        while (lv_it.next()) |e| {
            self.gpa.free(e.key_ptr.*);
            e.value_ptr.deinit(self.gpa);
        }
        self.last_value.deinit();

        var lf_it = self.literal_filters.iterator();
        while (lf_it.next()) |e| self.gpa.free(e.key_ptr.*);
        self.literal_filters.deinit(self.gpa);

        var lwb_it = self.wildcard_buckets.iterator();
        while (lwb_it.next()) |e| {
            e.value_ptr.deinit(self.gpa);
            self.gpa.free(e.key_ptr.*);
        }
        self.wildcard_buckets.deinit(self.gpa);
        self.wildcard_global.deinit(self.gpa);
    }

    pub fn configure(self: *Store, filters: []const Filter) !void {
        self.enabled = filters.len != 0;
        self.filters = filters;
        for (filters) |*filter| {
            if (filter.is_literal) {
                const gop = try self.literal_filters.getOrPut(self.gpa, filter.filter);
                if (!gop.found_existing) {
                    gop.key_ptr.* = try self.gpa.dupe(u8, filter.filter);
                    gop.value_ptr.* = {};
                }
            } else if (isWildcardToken(filter.first_token)) {
                try self.wildcard_global.append(self.gpa, filter);
            } else {
                const gop = try self.wildcard_buckets.getOrPut(self.gpa, filter.first_token);
                if (!gop.found_existing) {
                    gop.key_ptr.* = try self.gpa.dupe(u8, filter.first_token);
                    gop.value_ptr.* = .empty;
                }
                try gop.value_ptr.append(self.gpa, filter);
            }
        }
    }

    pub fn shouldStore(self: *Store, subject: []const u8, excluded_prefix: []const u8) bool {
        return !std.mem.startsWith(u8, subject, excluded_prefix) and self.subjectEnabled(subject);
    }

    pub fn subjectEnabled(self: *Store, subject: []const u8) bool {
        if (!self.enabled) return false;
        const filters = self.filters orelse return true;
        if (filters.len == 0) return false;

        if (self.literal_filters.contains(subject)) return true;

        const have_wildcard_buckets = self.wildcard_buckets.count() != 0;
        const have_wildcard_global = self.wildcard_global.items.len != 0;
        if (!have_wildcard_buckets and !have_wildcard_global) return false;

        var subject_tokens_buf: [subject_mod.max_tokens][]const u8 = undefined;
        const subject_tokens = splitInto(subject, &subject_tokens_buf);

        if (have_wildcard_buckets) {
            const subj_first = firstToken(subject);
            if (self.wildcard_buckets.getPtr(subj_first)) |bucket| {
                for (bucket.items) |filter| {
                    if (subject_mod.matchesTokens(filter.tokens, subject_tokens)) return true;
                }
            }
        }
        if (have_wildcard_global) {
            for (self.wildcard_global.items) |filter| {
                if (subject_mod.matchesTokens(filter.tokens, subject_tokens)) return true;
            }
        }
        return false;
    }

    pub fn store(self: *Store, subject: []const u8, payload: []const u8) !void {
        const gop = try self.last_value.getOrPut(subject);
        if (!gop.found_existing) {
            gop.key_ptr.* = try self.gpa.dupe(u8, subject);
            gop.value_ptr.* = .empty;
        }
        const list = gop.value_ptr;
        list.clearRetainingCapacity();
        try list.appendSlice(self.gpa, payload);
    }

    pub fn count(self: *const Store) usize {
        return self.last_value.count();
    }
};

/// Returns the first dot-separated token of `s`, or all of `s` if no dot.
fn firstToken(s: []const u8) []const u8 {
    const i = std.mem.indexOfScalar(u8, s, '.') orelse return s;
    return s[0..i];
}

/// True if `tok` is a wildcard token (`*` or `>`).
fn isWildcardToken(tok: []const u8) bool {
    return tok.len == 1 and (tok[0] == '*' or tok[0] == '>');
}

fn splitTokens(gpa: Allocator, s: []const u8) ![][]const u8 {
    var count: usize = 1;
    for (s) |c| if (c == '.') {
        count += 1;
    };
    const out = try gpa.alloc([]const u8, count);
    var it = std.mem.splitScalar(u8, s, '.');
    var i: usize = 0;
    while (it.next()) |tok| : (i += 1) out[i] = tok;
    return out;
}

fn splitInto(s: []const u8, buf: *[subject_mod.max_tokens][]const u8) [][]const u8 {
    var it = std.mem.splitScalar(u8, s, '.');
    var i: usize = 0;
    while (it.next()) |tok| : (i += 1) {
        if (i >= buf.len) break;
        buf[i] = tok;
    }
    return buf[0..i];
}
