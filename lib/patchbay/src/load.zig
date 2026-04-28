//! Patchbay loader: parse source into `[]Rule`, plus a synthetic-publish
//! validator that form-lints each rule. No evaluator state lives here, but
//! `validate` does spin up a `Context` and call `run`, so this module pulls
//! in the evaluator.

const std = @import("std");
const Allocator = std.mem.Allocator;
const sexpr = @import("sexpr.zig");
const subject_mod = @import("subject.zig");
const state = @import("state.zig");
const eval = @import("eval.zig");

pub const Rule = state.Rule;

pub const LoadError = error{
    InvalidRuleForm,
    UnknownTopLevel,
} || sexpr.ParseError || subject_mod.Error;

pub fn loadRules(arena: Allocator, source: []const u8) LoadError![]Rule {
    var discard: usize = 0;
    return loadRulesReporting(arena, source, &discard);
}

/// Same as `loadRules` but on a parser-level failure writes the byte offset
/// in `source` to `parse_err_offset`. Post-parse errors (InvalidRuleForm,
/// UnknownTopLevel, subject validation) leave it untouched, since they are
/// per-form, not per-byte.
pub fn loadRulesReporting(arena: Allocator, source: []const u8, parse_err_offset: *usize) LoadError![]Rule {
    const forms = try sexpr.parseAllReporting(arena, source, parse_err_offset);
    var out: std.ArrayList(Rule) = .empty;
    for (forms) |f| {
        if (f != .list) return error.InvalidRuleForm;
        const items = f.list;
        if (items.len < 2 or items[0] != .symbol) return error.UnknownTopLevel;
        // Other modules own their own top-level forms (e.g. `bridge`). Skip
        // anything we don't recognize as a rule, the other loader will
        // claim it and raise its own error if malformed.
        if (!std.mem.eql(u8, items[0].symbol, "on")) continue;
        if (items.len != 3 or items[1] != .string) return error.InvalidRuleForm;
        try subject_mod.validateFilter(items[1].string);
        try out.append(arena, .{ .filter = items[1].string, .body = items[2] });
    }
    return try out.toOwnedSlice(arena);
}

pub fn deinitRules(rules: []Rule, gpa: Allocator) void {
    for (rules) |*r| r.deinit(gpa);
}

/// Result of a single rule failing its synthetic-publish exercise. Strings
/// are owned by the arena passed to `validate`.
pub const ValidateFailure = struct {
    rule_index: usize,
    filter: []const u8,
    synthetic_subject: []const u8,
    err: anyerror,
};

/// Form-lints `rules` by firing one synthetic publish per rule against a
/// no-op publisher. Catches typo'd ops, arity bugs, and type errors on any
/// branch the synthetic input flows into. Branches gated off by the input
/// (e.g. behind a `(when ...)` that the synthetic value doesn't satisfy)
/// stay dark.
///
/// Synthetic subject: rule's filter with `*` and a trailing `>` replaced
/// by `"x"`. Synthetic payload: `"1"` (parses as a float for `payload-float`).
///
/// Caller-owned: `arena` holds returned failure strings; `gpa` owns any
/// per-rule state the exercise allocates (cleared via `deinitRules` later
/// or carried into the live ruleset). Exercising mutates `rule.state`, so
/// only call this on rules you'll either discard or reset.
pub fn validate(
    arena: Allocator,
    gpa: Allocator,
    rules: []Rule,
) Allocator.Error![]const ValidateFailure {
    var failures: std.ArrayListUnmanaged(ValidateFailure) = .empty;
    if (rules.len == 0) return &.{};

    var msg_arena_state = std.heap.ArenaAllocator.init(gpa);
    defer msg_arena_state.deinit();

    var dummy_pub_ctx: u8 = 0;
    const publisher: eval.Publisher = .{
        .ctx = &dummy_pub_ctx,
        .publish_fn = noopPublish,
    };

    for (rules, 0..) |*rule, i| {
        const subj = try synthSubjectFor(arena, rule.filter);
        var ctx: eval.Context = .{
            .subject = subj,
            .payload = "1",
            .publisher = publisher,
            .arena = msg_arena_state.allocator(),
            .gpa = gpa,
        };
        // Run only this rule by passing a single-element slice. Filter is
        // guaranteed to match because `subj` was synthesized from it.
        const single = rules[i .. i + 1];
        eval.run(single, &ctx) catch |err| {
            try failures.append(arena, .{
                .rule_index = i,
                .filter = rule.filter,
                .synthetic_subject = subj,
                .err = err,
            });
        };
        _ = msg_arena_state.reset(.retain_capacity);
    }

    return try failures.toOwnedSlice(arena);
}

fn noopPublish(_: *anyopaque, _: []const u8, _: []const u8) anyerror!void {
    return;
}

fn synthSubjectFor(arena: Allocator, filter: []const u8) Allocator.Error![]const u8 {
    var out: std.ArrayListUnmanaged(u8) = .empty;
    var it = std.mem.splitScalar(u8, filter, '.');
    var first = true;
    while (it.next()) |tok| {
        if (!first) try out.append(arena, '.');
        first = false;
        if (tok.len == 1 and (tok[0] == '*' or tok[0] == '>')) {
            try out.appendSlice(arena, "x");
        } else {
            try out.appendSlice(arena, tok);
        }
    }
    return out.toOwnedSlice(arena);
}

/// Returns true if `filter` contains no `*` or `>` token. Such filters can
/// be looked up by direct string equality against an inbound subject, the
/// foundation of `RuleSet`'s literal-subject dispatch table.
pub fn isLiteralFilter(filter: []const u8) bool {
    var it = std.mem.splitScalar(u8, filter, '.');
    while (it.next()) |tok| {
        if (tok.len == 1 and (tok[0] == '*' or tok[0] == '>')) return false;
    }
    return true;
}
