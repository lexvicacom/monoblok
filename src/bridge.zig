// Outbound NATS bridge. Forwards local publishes matching one of the
// configured `:export` subject filters to a remote NATS cluster via nats.c.
//
// Export-only: we do not subscribe on the remote. Messages flow local -> remote.
//
// Threading: nats.c runs its own sockets/io threads internally. We only call
// into nats.c from the loop thread (natsConnection_Publish / _Destroy), which
// is thread-safe in nats.c. No callbacks fire back at us.
//
// Integration: Server wires `bridgePublish` into Router.bridge_fn. Router
// fan-out calls it once per local publish; subject-filter match happens here,
// not in the router's subscription table.

const std = @import("std");
const Allocator = std.mem.Allocator;

const nats = @import("nats_c.zig");
const patchbay = @import("patchbay");
const sexpr = patchbay.sexpr;
const subject_mod = patchbay.subject;

pub const ConfigError = error{
    InvalidBridgeForm,
    DuplicateBridge,
    MissingServers,
    UnknownKeyword,
} || sexpr.ParseError;

pub const StartError = error{
    NatsOpenFailed,
    OptsCreateFailed,
    OptsApplyFailed,
    ConnectFailed,
} || Allocator.Error;

/// Parsed bridge config — owned by the caller's arena.
pub const Config = struct {
    servers: []const []const u8,
    name: ?[]const u8 = null,
    creds: ?[]const u8 = null,
    user: ?[]const u8 = null,
    password: ?[]const u8 = null,
    token: ?[]const u8 = null,
    tls: bool = false,
    tls_ca: ?[]const u8 = null,
    tls_cert: ?[]const u8 = null,
    tls_key: ?[]const u8 = null,
    tls_skip_verify: bool = false,
    connect_timeout_ms: ?i64 = null,
    ping_interval_ms: ?i64 = null,
    max_reconnect: ?i32 = null,
    reconnect_wait_ms: ?i64 = null,
    /// Compiled export subject filters. A local publish forwards to the
    /// remote iff one of these matches.
    exports: []const []const []const u8, // each export pre-split into tokens
    /// Original filter strings, for error messages.
    export_filters: []const []const u8,
};

/// Walk top-level forms looking for a single `(bridge :key v :key v ...)`.
/// Returns null if absent. Errors on a second bridge form or malformed keys.
pub fn loadConfig(arena: Allocator, source: []const u8) ConfigError!?Config {
    const forms = try sexpr.parseAll(arena, source);
    var found: ?Config = null;
    for (forms) |f| {
        if (f != .list or f.list.len == 0) continue;
        const head = f.list[0];
        if (head != .symbol or !std.mem.eql(u8, head.symbol, "bridge")) continue;

        if (found != null) return error.DuplicateBridge;
        found = try parseBridgeForm(arena, f.list[1..]);
    }
    return found;
}

const BridgeKey = enum {
    servers,
    name,
    creds,
    user,
    password,
    token,
    tls,
    tls_ca,
    tls_cert,
    tls_key,
    tls_skip_verify,
    connect_timeout_ms,
    ping_interval_ms,
    max_reconnect,
    reconnect_wait_ms,
    @"export",
};

const keyword_map = std.StaticStringMap(BridgeKey).initComptime(.{
    .{ "servers", .servers },
    .{ "name", .name },
    .{ "creds", .creds },
    .{ "user", .user },
    .{ "password", .password },
    .{ "token", .token },
    .{ "tls", .tls },
    .{ "tls-ca", .tls_ca },
    .{ "tls-cert", .tls_cert },
    .{ "tls-key", .tls_key },
    .{ "tls-skip-verify", .tls_skip_verify },
    .{ "connect-timeout-ms", .connect_timeout_ms },
    .{ "ping-interval-ms", .ping_interval_ms },
    .{ "max-reconnect", .max_reconnect },
    .{ "reconnect-wait-ms", .reconnect_wait_ms },
    .{ "export", .@"export" },
});

fn parseBridgeForm(arena: Allocator, kvs: []const sexpr.Value) ConfigError!Config {
    if (kvs.len % 2 != 0) return error.InvalidBridgeForm;

    var cfg: Config = .{
        .servers = &.{},
        .exports = &.{},
        .export_filters = &.{},
    };

    var i: usize = 0;
    while (i < kvs.len) : (i += 2) {
        if (kvs[i] != .keyword) return error.InvalidBridgeForm;
        const k = kvs[i].keyword;
        const v = kvs[i + 1];

        const tag = keyword_map.get(k) orelse return error.UnknownKeyword;
        switch (tag) {
            .servers => cfg.servers = try stringList(arena, v),
            .name => cfg.name = try asString(v),
            .creds => cfg.creds = try asString(v),
            .user => cfg.user = try asString(v),
            .password => cfg.password = try asString(v),
            .token => cfg.token = try asString(v),
            .tls => cfg.tls = try asBool(v),
            .tls_ca => cfg.tls_ca = try asString(v),
            .tls_cert => cfg.tls_cert = try asString(v),
            .tls_key => cfg.tls_key = try asString(v),
            .tls_skip_verify => cfg.tls_skip_verify = try asBool(v),
            .connect_timeout_ms => cfg.connect_timeout_ms = try asI64(v),
            .ping_interval_ms => cfg.ping_interval_ms = try asI64(v),
            .max_reconnect => cfg.max_reconnect = @intCast(try asI64(v)),
            .reconnect_wait_ms => cfg.reconnect_wait_ms = try asI64(v),
            .@"export" => {
                const filters = try stringList(arena, v);
                const toks = try arena.alloc([]const []const u8, filters.len);
                for (filters, 0..) |filt, ix| {
                    subject_mod.validateFilter(filt) catch return error.InvalidBridgeForm;
                    toks[ix] = try splitTokens(arena, filt);
                }
                cfg.exports = toks;
                cfg.export_filters = filters;
            },
        }
    }

    if (cfg.servers.len == 0) return error.MissingServers;
    return cfg;
}

fn asString(v: sexpr.Value) ConfigError![]const u8 {
    return switch (v) {
        .string => |s| s,
        else => error.InvalidBridgeForm,
    };
}

fn asBool(v: sexpr.Value) ConfigError!bool {
    return switch (v) {
        .boolean => |b| b,
        else => error.InvalidBridgeForm,
    };
}

fn asI64(v: sexpr.Value) ConfigError!i64 {
    return switch (v) {
        .number => |n| @intFromFloat(n),
        else => error.InvalidBridgeForm,
    };
}

fn stringList(arena: Allocator, v: sexpr.Value) ConfigError![]const []const u8 {
    if (v != .list) return error.InvalidBridgeForm;
    const out = try arena.alloc([]const u8, v.list.len);
    for (v.list, 0..) |x, ix| {
        if (x != .string) return error.InvalidBridgeForm;
        out[ix] = x.string;
    }
    return out;
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

// --- Runtime ------------------------------------------------------------

/// Must start with the same two fields as `server.BridgeStats`. Server reads
/// via a pointer of that type without taking a hard dependency on bridge.zig.
pub const Stats = extern struct {
    published: u64 = 0,
    dropped: u64 = 0,
    last_error: i32 = 0,
};

pub const Bridge = struct {
    gpa: Allocator,
    cfg: *const Config,
    nc: ?*nats.natsConnection = null,
    stats: Stats = .{},
    // Zero-terminated storage for strings we hand to nats.c (it expects C strings).
    z_strings: std.ArrayList([:0]u8) = .empty,

    pub fn init(gpa: Allocator, cfg: *const Config) Bridge {
        return .{ .gpa = gpa, .cfg = cfg };
    }

    pub fn deinit(self: *Bridge) void {
        if (self.nc) |c| {
            nats.natsConnection_Close(c);
            nats.natsConnection_Destroy(c);
            self.nc = null;
        }
        for (self.z_strings.items) |s| self.gpa.free(s);
        self.z_strings.deinit(self.gpa);
        _ = nats.nats_Close();
    }

    pub fn start(self: *Bridge) StartError!void {
        const s_open = nats.nats_Open(-1);
        if (s_open != nats.NATS_OK) return error.NatsOpenFailed;

        var opts: ?*nats.natsOptions = null;
        if (nats.natsOptions_Create(&opts) != nats.NATS_OK) return error.OptsCreateFailed;
        errdefer nats.natsOptions_Destroy(opts);

        try self.applyOpts(opts.?);

        if (nats.natsConnection_Connect(&self.nc, opts) != nats.NATS_OK) {
            nats.natsOptions_Destroy(opts);
            return error.ConnectFailed;
        }
        nats.natsOptions_Destroy(opts);
    }

    fn zdup(self: *Bridge, s: []const u8) ![:0]u8 {
        const z = try self.gpa.dupeZ(u8, s);
        try self.z_strings.append(self.gpa, z);
        return z;
    }

    fn applyOpts(self: *Bridge, opts: *nats.natsOptions) !void {
        // Servers list.
        const server_ptrs = try self.gpa.alloc([*c]const u8, self.cfg.servers.len);
        defer self.gpa.free(server_ptrs);
        for (self.cfg.servers, 0..) |srv, i| {
            const z = try self.zdup(srv);
            server_ptrs[i] = z.ptr;
        }
        if (nats.natsOptions_SetServers(opts, server_ptrs.ptr, @intCast(server_ptrs.len)) != nats.NATS_OK) return error.OptsApplyFailed;

        if (self.cfg.name) |n| {
            const z = try self.zdup(n);
            _ = nats.natsOptions_SetName(opts, z.ptr);
        }

        // Auth: creds file wins over user/pass wins over token.
        if (self.cfg.creds) |c| {
            const z = try self.zdup(c);
            if (nats.natsOptions_SetUserCredentialsFromFiles(opts, z.ptr, null) != nats.NATS_OK) return error.OptsApplyFailed;
        } else if (self.cfg.user) |u| {
            const uz = try self.zdup(u);
            const pz = if (self.cfg.password) |p| (try self.zdup(p)).ptr else null;
            _ = nats.natsOptions_SetUserInfo(opts, uz.ptr, pz);
        } else if (self.cfg.token) |t| {
            const z = try self.zdup(t);
            _ = nats.natsOptions_SetToken(opts, z.ptr);
        }

        if (self.cfg.tls) {
            _ = nats.natsOptions_SetSecure(opts, true);
            if (self.cfg.tls_ca) |ca| {
                const z = try self.zdup(ca);
                if (nats.natsOptions_LoadCATrustedCertificates(opts, z.ptr) != nats.NATS_OK) return error.OptsApplyFailed;
            }
            if (self.cfg.tls_cert) |cert| {
                const cz = try self.zdup(cert);
                const kz = if (self.cfg.tls_key) |k| (try self.zdup(k)).ptr else null;
                if (nats.natsOptions_LoadCertificatesChain(opts, cz.ptr, kz) != nats.NATS_OK) return error.OptsApplyFailed;
            }
            if (self.cfg.tls_skip_verify) _ = nats.natsOptions_SkipServerVerification(opts, true);
        }

        if (self.cfg.connect_timeout_ms) |v| _ = nats.natsOptions_SetTimeout(opts, v);
        if (self.cfg.ping_interval_ms) |v| _ = nats.natsOptions_SetPingInterval(opts, v);
        if (self.cfg.max_reconnect) |v| _ = nats.natsOptions_SetMaxReconnect(opts, v);
        if (self.cfg.reconnect_wait_ms) |v| _ = nats.natsOptions_SetReconnectWait(opts, v);
    }

    /// Called from router fan-out once per local publish. Matches against
    /// configured exports; forwards if any match. Never blocks.
    pub fn publish(self: *Bridge, subject: []const u8, payload: []const u8) void {
        const nc = self.nc orelse return;
        if (!self.matchesAnyExport(subject)) return;

        // nats.c expects a NUL-terminated subject. Subjects over 511 bytes
        // are absurd for this use case; drop rather than heap-alloc.
        var sb: [512]u8 = undefined;
        if (subject.len >= sb.len) {
            self.stats.dropped += 1;
            return;
        }
        @memcpy(sb[0..subject.len], subject);
        sb[subject.len] = 0;
        const subj_z: [*:0]const u8 = @ptrCast(&sb[0]);

        const s = nats.natsConnection_Publish(nc, subj_z, payload.ptr, @intCast(payload.len));
        if (s == nats.NATS_OK) {
            self.stats.published += 1;
        } else {
            self.stats.dropped += 1;
            self.stats.last_error = s;
        }
    }

    fn matchesAnyExport(self: *const Bridge, subject: []const u8) bool {
        var tokens_buf: [subject_mod.max_tokens][]const u8 = undefined;
        var it = std.mem.splitScalar(u8, subject, '.');
        var n: usize = 0;
        while (it.next()) |t| : (n += 1) {
            if (n >= tokens_buf.len) return false;
            tokens_buf[n] = t;
        }
        const toks = tokens_buf[0..n];
        for (self.cfg.exports) |filter_tokens| {
            if (subject_mod.matchesTokens(filter_tokens, toks)) return true;
        }
        return false;
    }
};

// --- Tests --------------------------------------------------------------

const testing = std.testing;

test "parse minimal bridge" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const src =
        \\(bridge
        \\  :servers ("nats://a:4222" "nats://b:4222")
        \\  :export  ("telemetry.>" "alerts.>"))
    ;
    const cfg = (try loadConfig(arena, src)).?;
    try testing.expectEqual(@as(usize, 2), cfg.servers.len);
    try testing.expectEqualStrings("nats://a:4222", cfg.servers[0]);
    try testing.expectEqual(@as(usize, 2), cfg.exports.len);
    try testing.expectEqualStrings("telemetry.>", cfg.export_filters[0]);
}

test "parse full bridge with tls + creds" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const src =
        \\(bridge
        \\  :servers ("tls://a.example:4222")
        \\  :creds   "/tmp/ngs.creds"
        \\  :tls     true
        \\  :tls-ca  "/tmp/ca.pem"
        \\  :name    "monoblok-bridge"
        \\  :reconnect-wait-ms 2000
        \\  :export  ("local.>"))
    ;
    const cfg = (try loadConfig(arena, src)).?;
    try testing.expect(cfg.tls);
    try testing.expectEqualStrings("/tmp/ngs.creds", cfg.creds.?);
    try testing.expectEqualStrings("/tmp/ca.pem", cfg.tls_ca.?);
    try testing.expectEqualStrings("monoblok-bridge", cfg.name.?);
    try testing.expectEqual(@as(i64, 2000), cfg.reconnect_wait_ms.?);
}

test "no bridge form returns null" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const src = "(on \"foo.*\" payload)";
    const cfg = try loadConfig(arena, src);
    try testing.expect(cfg == null);
}

test "duplicate bridge rejected" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const src =
        \\(bridge :servers ("a") :export ("x.>"))
        \\(bridge :servers ("b") :export ("y.>"))
    ;
    try testing.expectError(error.DuplicateBridge, loadConfig(arena, src));
}

test "missing :servers rejected" {
    var arena_state: std.heap.ArenaAllocator = .init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const src = "(bridge :export (\"x.>\"))";
    try testing.expectError(error.MissingServers, loadConfig(arena, src));
}
