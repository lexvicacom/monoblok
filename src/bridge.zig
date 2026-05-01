// Outbound NATS bridge. Forwards local publishes matching one of the
// configured `:export` subject filters to a remote NATS cluster via nats.zig
// (pure-Zig client, no system OpenSSL dependency).
//
// Export-only: we do not subscribe on the remote. Messages flow local -> remote.
//
// Threading: nats.zig spawns its own io_task thread that drains a lock-free
// publish ring buffer. `client.publish` is thread-safe and non-blocking, so we
// can call it directly from the libxev loop thread.
//
// Integration: Server wires `bridgePublish` into Router.bridge_fn. Router
// fan-out calls it once per local publish; subject-filter match happens here,
// not in the router's subscription table.

const std = @import("std");
const Allocator = std.mem.Allocator;

const nats = @import("nats");
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
    ConnectFailed,
    BackendInitFailed,
} || Allocator.Error;

/// Parsed bridge config (owned by the caller's arena).
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
    const items: []const sexpr.Value = switch (v) {
        .list => |xs| xs,
        .vector => |xs| xs,
        else => return error.InvalidBridgeForm,
    };
    const out = try arena.alloc([]const u8, items.len);
    for (items, 0..) |x, ix| {
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
    /// nats.zig requires a std.Io. Threaded is the safe default and works
    /// without depending on any specific OS event loop. Lives here so its
    /// address stays stable for the lifetime of the Client.
    backend: ?std.Io.Threaded = null,
    client: ?*nats.Client = null,
    stats: Stats = .{},

    pub fn init(gpa: Allocator, cfg: *const Config) Bridge {
        return .{ .gpa = gpa, .cfg = cfg };
    }

    pub fn deinit(self: *Bridge) void {
        if (self.client) |c| {
            c.deinit();
            self.client = null;
        }
        if (self.backend) |*b| {
            b.deinit();
            self.backend = null;
        }
    }

    pub fn start(self: *Bridge) StartError!void {
        // Initialize Io backend in place so its address is stable.
        self.backend = std.Io.Threaded.init(self.gpa, .{ .environ = .empty });
        const io = self.backend.?.io();

        // nats.zig's connect takes a single URL string. Try servers in order
        // until one accepts; rely on reconnect/discovery for the rest.
        var connect_err: ?anyerror = null;
        for (self.cfg.servers) |server_url| {
            const opts = self.buildOptions();
            if (nats.Client.connect(self.gpa, io, server_url, opts)) |c| {
                self.client = c;
                connect_err = null;
                break;
            } else |err| {
                connect_err = err;
                std.log.warn("bridge: connect to {s} failed: {s}", .{ server_url, @errorName(err) });
            }
        }
        if (self.client == null) {
            self.backend.?.deinit();
            self.backend = null;
            std.log.warn("bridge: all servers failed (last err: {s})", .{
                if (connect_err) |e| @errorName(e) else "unknown",
            });
            return error.ConnectFailed;
        }
    }

    fn buildOptions(self: *const Bridge) nats.Client.Options {
        var opts: nats.Client.Options = .{};
        if (self.cfg.name) |n| opts.name = n;
        // Auth precedence matches the nats.c version: creds > user/pass > token.
        if (self.cfg.creds) |c| {
            opts.creds_file = c;
        } else if (self.cfg.user) |u| {
            opts.user = u;
            if (self.cfg.password) |p| opts.pass = p;
        } else if (self.cfg.token) |t| {
            opts.auth_token = t;
        }
        if (self.cfg.tls) {
            opts.tls_required = true;
            if (self.cfg.tls_ca) |ca| opts.tls_ca_file = ca;
            if (self.cfg.tls_cert) |cert| opts.tls_cert_file = cert;
            if (self.cfg.tls_key) |key| opts.tls_key_file = key;
            if (self.cfg.tls_skip_verify) opts.tls_insecure_skip_verify = true;
        }
        if (self.cfg.connect_timeout_ms) |v| {
            opts.connect_timeout_ns = @intCast(v * std.time.ns_per_ms);
        }
        if (self.cfg.ping_interval_ms) |v| {
            opts.ping_interval_ms = @intCast(v);
        }
        if (self.cfg.max_reconnect) |v| {
            opts.max_reconnect_attempts = @intCast(v);
        }
        if (self.cfg.reconnect_wait_ms) |v| {
            opts.reconnect_wait_ms = @intCast(v);
        }
        return opts;
    }

    /// Called from router fan-out once per local publish. Matches against
    /// configured exports; forwards if any match. Never blocks (publish
    /// encodes into a lock-free ring buffer drained by io_task).
    pub fn publish(self: *Bridge, subject: []const u8, payload: []const u8) void {
        const c = self.client orelse return;
        if (!self.matchesAnyExport(subject)) return;

        c.publish(subject, payload) catch |err| {
            self.stats.dropped += 1;
            self.stats.last_error = @intFromError(err);
            return;
        };
        self.stats.published += 1;
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
