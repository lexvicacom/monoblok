//! Patchbay evaluator: dispatch core and public surface.
//!
//! This file owns `Context`, `Publisher`, `RuleSet`, `run`, the trace
//! plumbing, and the symbol-eval entry point. The actual built-in ops live
//! in `builtins.zig`; the loader and synthetic-publish validator in
//! `load.zig`; rule + per-rule state types in `state.zig`.
//!
//! Public symbols are re-exported here so consumers keep using
//! `patchbay.eval.{Rule, StateEntry, loadRules, ...}` without caring about
//! the internal split.

const std = @import("std");
const Allocator = std.mem.Allocator;
const sexpr = @import("sexpr.zig");
const subject_mod = @import("subject.zig");
const state = @import("state.zig");
const builtins = @import("builtins.zig");
const load = @import("load.zig");

pub const Value = sexpr.Value;

// Public surface; consumers reach types through `patchbay.eval.*`.
pub const Rule = state.Rule;
pub const StateEntry = state.StateEntry;
pub const Ohlc = state.Ohlc;
pub const Ring = state.Ring;

pub const LoadError = load.LoadError;
pub const ValidateFailure = load.ValidateFailure;
pub const loadRules = load.loadRules;
pub const loadRulesReporting = load.loadRulesReporting;
pub const deinitRules = load.deinitRules;
pub const validate = load.validate;
pub const isLiteralFilter = load.isLiteralFilter;

pub const EvalError = error{
    UnknownSymbol,
    TypeMismatch,
    ArityMismatch,
    InvalidSubject,
    PublishFailed,
} || Allocator.Error;

/// Side-effect callback. The router plugs in a closure that enqueues a
/// fan-out for `(publish subject payload)`.
pub const Publisher = struct {
    ctx: *anyopaque,
    publish_fn: *const fn (ctx: *anyopaque, subject: []const u8, payload: []const u8) anyerror!void,

    pub fn publish(self: Publisher, subject: []const u8, payload: []const u8) !void {
        return self.publish_fn(self.ctx, subject, payload);
    }
};

pub const Context = struct {
    subject: []const u8,
    payload: []const u8,
    publisher: Publisher,
    /// Per-message scratch; reset between messages by the caller.
    arena: Allocator,
    /// Long-lived allocator for per-rule state.
    gpa: Allocator,
    /// Set by `run` before each rule body executes.
    current_rule: ?*Rule = null,
    /// Publishes generated for this inbound PUB. Read by the server to
    /// detect rule-amplification blow-ups.
    rule_publishes: u32 = 0,
    /// Ingress wall-clock (ms), stamped once per PUB. Used by `hold-off`.
    now_ms: i64 = 0,
    /// `--trace`: switches `run` to the traced eval path.
    trace: bool = false,
    /// Indirect-dispatch slot picked by `run`; all recursive eval goes
    /// through `ctx.eval_fn(...)` so trace propagates without per-node branches.
    eval_fn: *const fn (ctx: *Context, v: Value) EvalError!Value = evalNormal,
    trace_depth: u8 = 0,
    /// Side-effecting ops append here under trace so `evalTraced` can show
    /// what was published instead of the bare nil those ops return.
    trace_emissions: std.ArrayListUnmanaged(TraceEmit) = .empty,
    /// Re-entry depth. Capped by `max_depth` so a rule that publishes back
    /// into its own filter can't loop forever.
    depth: u8 = 0,
    max_depth: u8 = 8,
    /// Optional re-entry hook. When set, every patchbay-emitted publish
    /// invokes this so downstream rules can react. The hook owns building
    /// the child Context with `depth + 1`.
    reentry_ctx: ?*anyopaque = null,
    reentry_fn: ?*const fn (reentry_ctx: ?*anyopaque, parent: *Context, subject: []const u8, payload: []const u8) anyerror!void = null,

    /// Single emission point used by every side-effecting op. Bumps
    /// counters, records the trace entry, and (under the depth cap)
    /// invokes the re-entry hook.
    pub fn emit(ctx: *Context, subject: []const u8, payload: []const u8) EvalError!void {
        ctx.publisher.publish(subject, payload) catch return error.PublishFailed;
        ctx.rule_publishes += 1;
        if (ctx.current_rule) |r| r.publishes_emitted += 1;
        recordTraceEmit(ctx, subject, payload);
        if (ctx.reentry_fn) |f| {
            if (ctx.depth < ctx.max_depth) {
                f(ctx.reentry_ctx, ctx, subject, payload) catch return error.PublishFailed;
            }
        }
    }
};

fn recordTraceEmit(ctx: *Context, subject: []const u8, payload: []const u8) void {
    if (!ctx.trace) return;
    ctx.trace_emissions.append(ctx.arena, .{ .subject = subject, .payload = payload }) catch {};
}

pub const TraceEmit = struct {
    subject: []const u8,
    payload: []const u8,
};

/// `[]Rule` plus a dispatch index built at load time. Literal-filter rules
/// hit a hashmap; wildcard rules still need the per-PUB linear scan.
pub const RuleSet = struct {
    rules: []Rule,
    /// Filter string -> rule indices. Insertion order preserved.
    literal_index: std.StringHashMapUnmanaged([]const u32) = .empty,
    /// Indices of rules with wildcard filters; scanned linearly per PUB.
    wildcard_indices: []const u32 = &.{},

    pub fn empty() RuleSet {
        return .{ .rules = &.{} };
    }

    /// Run matching rules for `ctx.subject`. Order: literal hits first
    /// (patchbay order), then wildcard hits (patchbay order). Mixing the
    /// two means a wildcard rule defined before a literal one fires after.
    pub fn run(self: *const RuleSet, ctx: *Context) !void {
        if (self.rules.len == 0) return;
        ctx.eval_fn = if (ctx.trace) evalTraced else evalNormal;
        ctx.trace_emissions = .empty;
        if (ctx.trace) {
            std.debug.print("trace: {s} {s}\n", .{ ctx.subject, ctx.payload });
        }
        const t_pub_start: u64 = if (ctx.trace) monotonicNs() else 0;

        if (self.literal_index.get(ctx.subject)) |idxs| {
            for (idxs) |i| try runOne(self.rules, i, ctx);
        }
        for (self.wildcard_indices) |i| {
            const rule = &self.rules[i];
            if (!subject_mod.matches(rule.filter, ctx.subject)) continue;
            try runOne(self.rules, i, ctx);
        }

        ctx.current_rule = null;
        if (ctx.trace) {
            const ns = monotonicNs() -| t_pub_start;
            std.debug.print("total [{s}]\n", .{formatNs(ns)});
        }
    }
};

/// Build a `RuleSet` from `[]Rule`. Index entries live in `arena`, which
/// must outlive the RuleSet (same lifetime as `rules`).
pub fn buildRuleSet(arena: Allocator, rules: []Rule) Allocator.Error!RuleSet {
    var rs: RuleSet = .{ .rules = rules };
    if (rules.len == 0) return rs;

    var lit_buckets: std.StringHashMapUnmanaged(std.ArrayListUnmanaged(u32)) = .empty;
    var wild: std.ArrayListUnmanaged(u32) = .empty;
    defer {
        var it = lit_buckets.iterator();
        while (it.next()) |e| e.value_ptr.deinit(arena);
        lit_buckets.deinit(arena);
        wild.deinit(arena);
    }

    for (rules, 0..) |*r, i| {
        const idx: u32 = @intCast(i);
        if (load.isLiteralFilter(r.filter)) {
            const gop = try lit_buckets.getOrPut(arena, r.filter);
            if (!gop.found_existing) gop.value_ptr.* = .empty;
            try gop.value_ptr.append(arena, idx);
        } else {
            try wild.append(arena, idx);
        }
    }

    var it = lit_buckets.iterator();
    while (it.next()) |e| {
        const slice = try arena.dupe(u32, e.value_ptr.items);
        try rs.literal_index.put(arena, e.key_ptr.*, slice);
    }
    rs.wildcard_indices = try arena.dupe(u32, wild.items);
    return rs;
}

fn runOne(rules: []Rule, i: u32, ctx: *Context) !void {
    const rule = &rules[i];
    ctx.current_rule = rule;
    ctx.trace_depth = 0;
    if (ctx.trace) {
        std.debug.print("  rule {d} (on \"{s}\") matched\n", .{ i, rule.filter });
        ctx.trace_depth = 1;
        const t_start = monotonicNs();
        _ = try ctx.eval_fn(ctx, rule.body);
        const ns = monotonicNs() -| t_start;
        std.debug.print("  rule {d} done [{s}]\n", .{ i, formatNs(ns) });
    } else {
        _ = try ctx.eval_fn(ctx, rule.body);
    }
}

pub fn run(rules: []Rule, ctx: *Context) !void {
    ctx.eval_fn = if (ctx.trace) evalTraced else evalNormal;
    // Reset; the per-message arena was wiped between PUBs.
    ctx.trace_emissions = .empty;
    if (ctx.trace) {
        std.debug.print("trace: {s} {s}\n", .{ ctx.subject, ctx.payload });
    }
    const t_pub_start: u64 = if (ctx.trace) monotonicNs() else 0;
    for (rules, 0..) |*rule, i| {
        if (!subject_mod.matches(rule.filter, ctx.subject)) continue;
        ctx.current_rule = rule;
        ctx.trace_depth = 0;
        if (ctx.trace) {
            std.debug.print("  rule {d} (on \"{s}\") matched\n", .{ i, rule.filter });
            ctx.trace_depth = 1;
            const t_start = monotonicNs();
            _ = try ctx.eval_fn(ctx, rule.body);
            const ns = monotonicNs() -| t_start;
            std.debug.print("  rule {d} done [{s}]\n", .{ i, formatNs(ns) });
        } else {
            _ = try ctx.eval_fn(ctx, rule.body);
        }
    }
    ctx.current_rule = null;
    if (ctx.trace) {
        const ns = monotonicNs() -| t_pub_start;
        std.debug.print("total [{s}]\n", .{formatNs(ns)});
    }
}

// --- Eval dispatchers ---------------------------------------------------

fn evalNormal(ctx: *Context, v: Value) EvalError!Value {
    return switch (v) {
        .nil, .boolean, .number, .string => v,
        .symbol => |s| evalSymbol(ctx, s),
        .list => |items| builtins.evalCall(ctx, items),
        .keyword => error.UnknownSymbol, // keywords are config-only, not rule body values
    };
}

/// Trace wrapper. Atoms pass through to `evalNormal`; list-forms get
/// printed with their result and elapsed time. We snapshot
/// `trace_emissions.items.len` before recursing so leaf emitters can show
/// `=> published ...` instead of bare `nil`; wrappers leave the slice in
/// place so outer forms don't re-print emissions.
fn evalTraced(ctx: *Context, v: Value) EvalError!Value {
    if (v != .list or v.list.len == 0) return evalNormal(ctx, v);

    indentTrace(ctx.trace_depth);
    formatValue(v);
    std.debug.print("\n", .{});

    const emit_before = ctx.trace_emissions.items.len;
    ctx.trace_depth +|= 1;
    const t_start = monotonicNs();
    const result = try evalNormal(ctx, v);
    const ns = monotonicNs() -| t_start;
    ctx.trace_depth -|= 1;
    const new_emits = ctx.trace_emissions.items[emit_before..];

    indentTrace(ctx.trace_depth +| 1);
    if (result == .nil and new_emits.len > 0 and isLeafEmitter(v)) {
        if (new_emits.len == 1) {
            std.debug.print("=> published \"{s}\" {s} [{s}]\n", .{ new_emits[0].subject, new_emits[0].payload, formatNs(ns) });
        } else {
            std.debug.print("=> published {d} subjects [{s}]\n", .{ new_emits.len, formatNs(ns) });
            for (new_emits) |e| {
                indentTrace(ctx.trace_depth +| 2);
                std.debug.print("\"{s}\" {s}\n", .{ e.subject, e.payload });
            }
        }
    } else if (result == .nil) {
        if (suppressionHint(v)) |hint| {
            std.debug.print("=> nil ({s}) [{s}]\n", .{ hint, formatNs(ns) });
        } else {
            std.debug.print("=> nil [{s}]\n", .{formatNs(ns)});
        }
    } else {
        std.debug.print("=> ", .{});
        formatValue(result);
        std.debug.print(" [{s}]\n", .{formatNs(ns)});
    }
    return result;
}

/// Generic gloss for why a form returned nil; doesn't know the actual
/// reason. Returns null for unremarkable nils.
fn suppressionHint(v: Value) ?[]const u8 {
    if (v != .list or v.list.len == 0) return null;
    if (v.list[0] != .symbol) return null;
    const head = v.list[0].symbol;
    const map = .{
        .{ "squelch", "squelched" },
        .{ "deadband", "within deadband" },
        .{ "hold-off", "rate-limited" },
        .{ "rising-edge", "no rising edge" },
        .{ "falling-edge", "no falling edge" },
        .{ "transition", "no transition" },
        .{ "changed?", "unchanged" },
        .{ "when", "branch not taken" },
        .{ "if", "branch not taken" },
        .{ "bar", "bar in progress" },
        .{ "json-get", "key missing" },
    };
    inline for (map) |entry| {
        if (std.mem.eql(u8, head, entry[0])) return entry[1];
    }
    return null;
}

/// True for ops that publish their own subjects. Wrappers (`->`, `do`,
/// `when`, `if`) stay at `=> nil` so their children's emission lines aren't
/// duplicated.
fn isLeafEmitter(v: Value) bool {
    if (v != .list or v.list.len == 0) return false;
    if (v.list[0] != .symbol) return false;
    const head = v.list[0].symbol;
    return std.mem.eql(u8, head, "publish") or
        std.mem.eql(u8, head, "publish-to") or
        std.mem.eql(u8, head, "count") or
        std.mem.eql(u8, head, "json-demux") or
        std.mem.eql(u8, head, "bar");
}

fn indentTrace(depth: u8) void {
    var i: u8 = 0;
    while (i < depth) : (i += 1) std.debug.print("  ", .{});
}

/// CLOCK_MONOTONIC in ns. Direct syscall: `std.time.Timer` is gone in
/// 0.16 and `std.Io`'s clock needs an `Io` we don't carry on `Context`.
fn monotonicNs() u64 {
    var ts: std.posix.timespec = undefined;
    _ = std.posix.system.clock_gettime(std.posix.CLOCK.MONOTONIC, &ts);
    const sec: u64 = @intCast(ts.sec);
    const nsec: u64 = @intCast(ts.nsec);
    return sec *| std.time.ns_per_s +| nsec;
}

/// Pretty elapsed-time string. Returned slice aliases `tls_ns_buf` and is
/// invalidated by the next call from this thread.
threadlocal var tls_ns_buf: [32]u8 = undefined;
fn formatNs(ns: u64) []const u8 {
    if (ns < 1_000) return std.fmt.bufPrint(&tls_ns_buf, "{d}ns", .{ns}) catch "?";
    if (ns < 1_000_000) return std.fmt.bufPrint(&tls_ns_buf, "{d}µs", .{ns / 1_000}) catch "?";
    if (ns < 1_000_000_000) return std.fmt.bufPrint(&tls_ns_buf, "{d}ms", .{ns / 1_000_000}) catch "?";
    return std.fmt.bufPrint(&tls_ns_buf, "{d}s", .{ns / 1_000_000_000}) catch "?";
}

/// Print a Value in source-level shape (trace-only, no escape handling).
fn formatValue(v: Value) void {
    switch (v) {
        .nil => std.debug.print("nil", .{}),
        .boolean => |b| std.debug.print("{s}", .{if (b) "true" else "false"}),
        .number => |n| std.debug.print("{d}", .{n}),
        .symbol => |s| std.debug.print("{s}", .{s}),
        .keyword => |s| std.debug.print(":{s}", .{s}),
        .string => |s| std.debug.print("\"{s}\"", .{s}),
        .list => |items| {
            std.debug.print("(", .{});
            for (items, 0..) |it, i| {
                if (i > 0) std.debug.print(" ", .{});
                formatValue(it);
            }
            std.debug.print(")", .{});
        },
    }
}

fn evalSymbol(ctx: *Context, name: []const u8) EvalError!Value {
    if (std.mem.eql(u8, name, "subject")) return .{ .string = ctx.subject };
    if (std.mem.eql(u8, name, "payload")) return .{ .string = ctx.payload };
    if (std.mem.eql(u8, name, "payload-float")) {
        const n = std.fmt.parseFloat(f64, ctx.payload) catch return error.TypeMismatch;
        return .{ .number = n };
    }
    if (std.mem.eql(u8, name, "payload-int")) {
        const n = std.fmt.parseInt(i64, ctx.payload, 10) catch return error.TypeMismatch;
        return .{ .number = @floatFromInt(n) };
    }
    return error.UnknownSymbol;
}

test {
    _ = state;
    _ = builtins;
    _ = load;
    _ = @import("eval_test.zig");
}
