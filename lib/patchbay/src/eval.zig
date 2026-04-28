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

// Re-exports: keep the public surface identical to pre-split eval.zig so
// every consumer (server, router, snapshot, main) continues to reach types
// through `patchbay.eval.*`.
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
    /// Per-message scratch; caller owns an arena that is reset between messages.
    arena: Allocator,
    /// Long-lived allocator for per-rule state (e.g. `squelch` keeps the
    /// last-seen payload across messages). Distinct from `arena`, which is
    /// reset between messages.
    gpa: Allocator,
    /// Set by `run` before each rule body executes so stateful primitives
    /// know which rule's state table to read/write.
    current_rule: ?*Rule = null,
    /// Count of publishes the patchbay generated for this one inbound PUB.
    /// Incremented by `callPublish` / `callPublishTo`. Read by the server
    /// after `run()` returns to detect rule-amplification blow-ups.
    rule_publishes: u32 = 0,
    /// Ingress wall-clock in milliseconds, stamped once by the server per
    /// inbound PUB so every op in this evaluation sees a consistent "now".
    /// Used by time-based gates like `hold-off`. Defaults to 0 so
    /// router-internal `run()` paths (e.g. no time-gated rules) don't need
    /// to care; tests inject a value directly.
    now_ms: i64 = 0,
    /// `--trace` mode. When true, `run` switches to the traced eval path
    /// (`evalTraced`) which prints each form, its result, and elapsed
    /// time to stderr. Branched once per `run` (top of the function), not
    /// per node, via `eval_fn` below.
    trace: bool = false,
    /// Indirect-dispatch slot for the evaluator. `run` picks `evalNormal`
    /// or `evalTraced` based on `trace` and writes it here once. All
    /// recursive eval calls go through `ctx.eval_fn(...)` so the choice
    /// propagates without each form having to branch.
    eval_fn: *const fn (ctx: *Context, v: Value) EvalError!Value = evalNormal,
    /// Indent level for trace output (one level per recursive eval). Reset
    /// by `run` per rule body.
    trace_depth: u8 = 0,
    /// Recorded emissions for trace output. Each side-effecting op (publish,
    /// publish-to, count, json-demux, ohlc-bar) appends `(subject, payload)`
    /// here when `trace` is on, so `evalTraced` can show what was actually
    /// published instead of the bare `nil` those ops return. Strings live in
    /// `arena`. Only populated under trace; left at .empty otherwise.
    trace_emissions: std.ArrayListUnmanaged(TraceEmit) = .empty,
};

pub const TraceEmit = struct {
    subject: []const u8,
    payload: []const u8,
};

/// `[]Rule` plus a dispatch index. Built once at load time, then queried
/// per inbound PUB. The index lets us skip the linear scan over every
/// rule's filter for the common case of literal-subject filters; only
/// wildcard filters still need the per-PUB scan.
///
/// Indices in both maps point into `rules` by position. The arena that
/// owns `rules` also owns the index slices.
pub const RuleSet = struct {
    rules: []Rule,
    /// Filter string → indices of rules with that exact literal filter.
    /// Multiple rules can share a filter; we keep insertion order so
    /// behavior matches the old linear scan.
    literal_index: std.StringHashMapUnmanaged([]const u32) = .empty,
    /// Indices (into `rules`) of rules whose filter contains a wildcard.
    /// Scanned linearly per PUB. Empty for fully-literal patchbays.
    wildcard_indices: []const u32 = &.{},

    pub fn empty() RuleSet {
        return .{ .rules = &.{} };
    }

    /// Run all matching rules for `ctx.subject`. Literal-filter rules are
    /// found via the index; wildcard rules are scanned linearly. Match
    /// order is: literal hits first (in original patchbay order), then
    /// wildcard hits (also in original order). For a fully-literal
    /// patchbay this is the same order as the legacy linear scan; if you
    /// mix the two, a wildcard rule defined before a literal one will
    /// fire after it. Worth knowing if you depend on rule ordering.
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

/// Build a `RuleSet` from an already-loaded `[]Rule`. The arena allocates
/// the index entries; it must outlive the `RuleSet` (same lifetime as
/// `rules` itself, in practice).
pub fn buildRuleSet(arena: Allocator, rules: []Rule) Allocator.Error!RuleSet {
    var rs: RuleSet = .{ .rules = rules };
    if (rules.len == 0) return rs;

    // First pass: bucket indices into per-filter lists for literals, and
    // flat list for wildcards. Use a scratch map of ArrayLists keyed by
    // filter string; flatten into `[]const u32` slices in the arena after.
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
    // The per-message arena is reset between PUBs by the server, which frees
    // any prior backing storage for `trace_emissions`. Reset the list so we
    // don't carry over a stale capacity pointer into freed memory.
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

/// Trace wrapper: prints each list-form (call) with its result and elapsed
/// time. Atoms and bare symbols pass straight through to `evalNormal`,
/// timing them would add noise without insight.
///
/// Side-effecting ops (publish, publish-to, count, json-demux, ohlc-bar) all
/// return `nil`. To distinguish "I emitted something" from "I was suppressed",
/// we snapshot `ctx.trace_emissions.items.len` before recursing; any new
/// entries belong to this form's subtree. If the form added emissions AND
/// returned nil, we print `=> published "subj" payload` instead of the bare
/// `=> nil`. We only consume the leaf-level emission (the parent of a `(->)`
/// chain shows aggregate timing only); we leave the full list in place so
/// outer forms aren't fooled into re-printing the same emissions.
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

/// Best-effort hint for why a form returned `nil`. Static dispatch on the
/// head symbol, doesn't know the actual reason (e.g. `rising-edge` returns
/// nil for both "first sight" and "stayed false"), just gives a generic gloss
/// so readers don't have to remember each op's suppression semantics. Returns
/// null for forms whose nil is unremarkable (atoms, unknown ops, wrappers
/// that already showed their children).
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
        .{ "ohlc-bar", "bar in progress" },
        .{ "json-get", "key missing" },
    };
    inline for (map) |entry| {
        if (std.mem.eql(u8, head, entry[0])) return entry[1];
    }
    return null;
}

/// True for the side-effecting ops that publish their own subjects (and
/// therefore record emissions). Wrapper forms like `->`, `do`, `when`, `if`
/// also return nil and may have emissions in their subtree, but their
/// children already printed the `published ...` line, so we keep the wrapper
/// at `=> nil` to show timing without duplicating the emission line.
fn isLeafEmitter(v: Value) bool {
    if (v != .list or v.list.len == 0) return false;
    if (v.list[0] != .symbol) return false;
    const head = v.list[0].symbol;
    return std.mem.eql(u8, head, "publish") or
        std.mem.eql(u8, head, "publish-to") or
        std.mem.eql(u8, head, "count") or
        std.mem.eql(u8, head, "json-demux") or
        std.mem.eql(u8, head, "ohlc-bar");
}

fn indentTrace(depth: u8) void {
    var i: u8 = 0;
    while (i < depth) : (i += 1) std.debug.print("  ", .{});
}

/// CLOCK_MONOTONIC in nanoseconds. Used only on the trace path; we go
/// straight to `posix.system.clock_gettime` because `std.time.Timer`
/// doesn't exist in Zig 0.16 and the `std.Io` clock API requires an
/// `Io` instance we don't carry through `Context`. Identical syscall to
/// what xev's loop uses internally.
fn monotonicNs() u64 {
    var ts: std.posix.timespec = undefined;
    _ = std.posix.system.clock_gettime(std.posix.CLOCK.MONOTONIC, &ts);
    const sec: u64 = @intCast(ts.sec);
    const nsec: u64 = @intCast(ts.nsec);
    return sec *| std.time.ns_per_s +| nsec;
}

/// Returns a pretty elapsed-time string ("180ns", "12µs", "3ms"). The
/// returned slice points into `tls_ns_buf` and is invalidated by the next
/// call from this thread; trace output is single-threaded (loop thread)
/// so the buffer reuse is safe.
threadlocal var tls_ns_buf: [32]u8 = undefined;
fn formatNs(ns: u64) []const u8 {
    if (ns < 1_000) return std.fmt.bufPrint(&tls_ns_buf, "{d}ns", .{ns}) catch "?";
    if (ns < 1_000_000) return std.fmt.bufPrint(&tls_ns_buf, "{d}µs", .{ns / 1_000}) catch "?";
    if (ns < 1_000_000_000) return std.fmt.bufPrint(&tls_ns_buf, "{d}ms", .{ns / 1_000_000}) catch "?";
    return std.fmt.bufPrint(&tls_ns_buf, "{d}s", .{ns / 1_000_000_000}) catch "?";
}

/// Print a Value back in its source-level shape. Used only by the trace
/// path; not a general pretty-printer (no escape handling on strings, no
/// keyword support, since trace input is rule bodies and rule bodies
/// don't contain keywords).
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
