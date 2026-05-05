//! Per-slot clock registry: one xev.Timer per active time-windowed slot,
//! scheduled at the slot's exact next deadline (computed by the kernel).
//! No periodic walker; deadlines are precise to the ms.
//!
//! Lifecycle:
//!   - The patchbay calls `ctx.notifyClockSlot(rule, slot_key)` whenever a
//!     time-windowed slot is created or mutated. The hook runs `arm`,
//!     which (re)schedules the slot's timer using `nextDeadlineMs`.
//!   - When a timer fires, `onSlotTimer` looks up the slot, runs the
//!     correct kernel transition (Bar.timeTick or TimeRing.evict), emits
//!     any outputs, and re-arms based on the slot's new deadline.
//!   - On rule reload / shutdown, `deinit` cancels every armed timer.
//!     Each ClockSlot owns its key-string + completions and is freed
//!     from the cancel callback so an in-flight cancel can still
//!     dereference its userdata.
//!
//! All work runs on the loop thread; no atomics, no locks. The same
//! single-threaded invariant the rest of monoblok lives by.

const std = @import("std");
const Allocator = std.mem.Allocator;
const xev = @import("xev");

const patchbay = @import("patchbay");
const rules_mod = patchbay.eval;
const Rule = rules_mod.Rule;
const StateEntry = rules_mod.StateEntry;
const Publisher = rules_mod.Publisher;

const router_mod = @import("router.zig");

/// Per-rule registry of armed timers. One Registry instance per rule-set;
/// `slots[i]` is the table for `rules[i]`.
pub const Registry = struct {
    gpa: Allocator,
    loop: *xev.Loop,
    rules: []Rule,
    /// Same fan-out target the PUB path uses; closes flow through the LVC
    /// like any other publish.
    publisher: Publisher,
    /// Per-rule slot-key -> ClockSlot. Indexed by rule_idx.
    slots: []std.StringHashMapUnmanaged(*ClockSlot),
    /// Scratch allocator for emit subjects/payloads; reset on each fire.
    arena: std.heap.ArenaAllocator,

    pub fn init(gpa: Allocator, loop: *xev.Loop, rules: []Rule, publisher: Publisher) !Registry {
        const slots = try gpa.alloc(std.StringHashMapUnmanaged(*ClockSlot), rules.len);
        for (slots) |*s| s.* = .empty;
        return .{
            .gpa = gpa,
            .loop = loop,
            .rules = rules,
            .publisher = publisher,
            .slots = slots,
            .arena = .init(gpa),
        };
    }

    /// Cancel every armed timer and free per-rule tables. Each ClockSlot's
    /// memory is released from its own cancel-callback, so it must remain
    /// reachable until the loop drains. This means callers should pump
    /// the loop one more time after deinit when shutting down cleanly.
    pub fn deinit(self: *Registry) void {
        for (self.slots) |*table| {
            var it = table.iterator();
            while (it.next()) |entry| {
                const slot = entry.value_ptr.*;
                slot.beginCancel();
            }
            table.deinit(self.gpa);
        }
        self.gpa.free(self.slots);
        self.arena.deinit();
    }

    /// Patchbay clock-hook entry point. The host-side shim built in
    /// `attachTo` forwards `(rule, slot_key)` here.
    pub fn arm(self: *Registry, rule: *Rule, slot_key: []const u8) void {
        const rule_idx = self.ruleIndex(rule) orelse return;
        const deadline = computeDeadline(rule, slot_key) orelse {
            // Slot exists but has nothing pending (e.g. a Bar that just
            // closed and reset to count=0). If we had a timer, leave it
            // — it'll be re-armed on the next mutation.
            return;
        };
        const now = self.loop.now();
        const due_in_ms: u64 = if (deadline <= now) 0 else @intCast(deadline - now);

        var table = &self.slots[rule_idx];
        if (table.get(slot_key)) |existing| {
            if (existing.armed_deadline_ms == deadline) return; // unchanged
            existing.armed_deadline_ms = deadline;
            existing.timer.reset(self.loop, &existing.completion, &existing.cancel_completion, due_in_ms, ClockSlot, existing, ClockSlot.onFire);
            return;
        }

        const cs = self.gpa.create(ClockSlot) catch return;
        const key_owned = self.gpa.dupe(u8, slot_key) catch {
            self.gpa.destroy(cs);
            return;
        };
        cs.* = .{
            .registry = self,
            .gpa = self.gpa,
            .rule_idx = rule_idx,
            .slot_key = key_owned,
            .timer = xev.Timer.init() catch {
                self.gpa.free(key_owned);
                self.gpa.destroy(cs);
                return;
            },
            .completion = .{},
            .cancel_completion = .{},
            .armed_deadline_ms = deadline,
        };
        table.put(self.gpa, key_owned, cs) catch {
            cs.timer.deinit();
            self.gpa.free(key_owned);
            self.gpa.destroy(cs);
            return;
        };
        cs.timer.run(self.loop, &cs.completion, due_in_ms, ClockSlot, cs, ClockSlot.onFire);
    }

    fn ruleIndex(self: *Registry, rule: *Rule) ?u32 {
        // Pointer math: rules live contiguously in the slice we were
        // built with, so rule_idx is derivable in O(1).
        const base = @intFromPtr(self.rules.ptr);
        const here = @intFromPtr(rule);
        if (here < base) return null;
        const off = here - base;
        const idx = off / @sizeOf(Rule);
        if (idx >= self.rules.len) return null;
        if (&self.rules[idx] != rule) return null;
        return @intCast(idx);
    }
};

/// One armed timer for one (rule, slot_key). Allocated lazily on the first
/// `arm` for a slot, freed only after a successful cancel completion (or
/// during deinit, with the same restriction).
pub const ClockSlot = struct {
    registry: *Registry,
    gpa: Allocator,
    rule_idx: u32,
    /// gpa-owned, freed in onCancelDone.
    slot_key: []u8,
    timer: xev.Timer,
    completion: xev.Completion,
    /// Required by xev.Timer.reset / .cancel: holds the in-flight cancel
    /// state. Must persist for the slot's life.
    cancel_completion: xev.Completion,
    /// What the timer is currently scheduled for. -1 means "not currently
    /// armed" (set when we want to keep the slot record but skip the
    /// timer until the slot is mutated again).
    armed_deadline_ms: i64,
    /// True after `beginCancel` has been called; the next `onFire` (which
    /// will arrive with error.Canceled) is the one that frees us.
    cancelled: bool = false,

    fn onFire(
        self_opt: ?*ClockSlot,
        _: *xev.Loop,
        _: *xev.Completion,
        r: xev.Timer.RunError!void,
    ) xev.CallbackAction {
        const self = self_opt.?;
        // The cancel path arrives here as error.Canceled. We took ownership
        // at beginCancel and now release the heap-side state.
        if (self.cancelled) {
            _ = r catch {};
            self.free();
            return .disarm;
        }
        _ = r catch {
            // Spurious error (not a cancel). Nothing useful to do; drop.
            return .disarm;
        };

        // Fire: run the slot's transition, emit if needed, re-arm.
        const reg = self.registry;
        const rule = &reg.rules[self.rule_idx];
        const entry_ptr = rule.state.getPtr(self.slot_key) orelse {
            // Slot vanished (rule reload race). Drop ourselves.
            self.removeFromTable();
            self.free();
            return .disarm;
        };

        _ = reg.arena.reset(.retain_capacity);
        const arena = reg.arena.allocator();
        const now = reg.loop.now();
        switch (entry_ptr.*) {
            .ohlc => |*bar| {
                if (bar.timeTick(now)) |c| {
                    const subj_root = subjectFromBarKey(self.slot_key) orelse return reArm(self, rule);
                    emitBar(arena, reg.publisher, subj_root, c.open, c.high, c.low, c.close) catch {};
                    rule.publishes_emitted += 4;
                }
            },
            .time_ring => |*ring| ring.evict(now),
            else => {},
        }

        return reArm(self, rule);
    }

    fn reArm(self: *ClockSlot, rule: *Rule) xev.CallbackAction {
        const reg = self.registry;
        const next = computeDeadline(rule, self.slot_key) orelse {
            // Nothing left to do; tear down this entry. A future mutation
            // will reinstate it via Registry.arm.
            self.removeFromTable();
            self.free();
            return .disarm;
        };
        const now = reg.loop.now();
        const due_in_ms: u64 = if (next <= now) 0 else @intCast(next - now);
        self.armed_deadline_ms = next;
        self.timer.run(reg.loop, &self.completion, due_in_ms, ClockSlot, self, ClockSlot.onFire);
        return .disarm;
    }

    fn beginCancel(self: *ClockSlot) void {
        if (self.cancelled) return;
        self.cancelled = true;
        // We don't need a separate cancel callback; the original `onFire`
        // sees error.Canceled, notices `cancelled = true`, and frees.
        self.timer.cancel(self.registry.loop, &self.completion, &self.cancel_completion, ClockSlot, self, ClockSlot.onCancelComplete);
    }

    fn onCancelComplete(
        _: ?*ClockSlot,
        _: *xev.Loop,
        _: *xev.Completion,
        _: xev.Timer.CancelError!void,
    ) xev.CallbackAction {
        // Cancel ack. The `onFire` callback (with error.Canceled) does the
        // actual free; nothing to do here.
        return .disarm;
    }

    fn removeFromTable(self: *ClockSlot) void {
        const table = &self.registry.slots[self.rule_idx];
        _ = table.remove(self.slot_key);
    }

    fn free(self: *ClockSlot) void {
        const gpa = self.gpa;
        self.timer.deinit();
        gpa.free(self.slot_key);
        gpa.destroy(self);
    }
};

/// Look up the slot's StateEntry on the rule and return its next deadline,
/// or null if the slot is empty / unrecognised. The host evaluator owns
/// the storage; the kernel exposes the math.
fn computeDeadline(rule: *Rule, slot_key: []const u8) ?i64 {
    const entry = rule.state.getPtr(slot_key) orelse return null;
    return switch (entry.*) {
        .ohlc => |bar| bar.nextDeadlineMs(),
        .time_ring => |ring| ring.nextDeadlineMs(),
        else => null,
    };
}

/// Strip the `"bar/m:"` prefix from a slot key to recover the bar's
/// emission subject. Slot keys are produced by `state.keyForWindow`, so
/// the prefix shape is the contract between that and us.
fn subjectFromBarKey(key: []const u8) ?[]const u8 {
    const prefix = "bar/m:";
    if (!std.mem.startsWith(u8, key, prefix)) return null;
    return key[prefix.len..];
}

fn emitBar(
    arena: Allocator,
    publisher: Publisher,
    subject: []const u8,
    open: f64,
    high: f64,
    low: f64,
    close: f64,
) !void {
    const fields = [_]struct { name: []const u8, val: f64 }{
        .{ .name = "open", .val = open },
        .{ .name = "high", .val = high },
        .{ .name = "low", .val = low },
        .{ .name = "close", .val = close },
    };
    for (fields) |f| {
        const subj = try std.fmt.allocPrint(arena, "{s}.bar.{s}", .{ subject, f.name });
        const out = try std.fmt.allocPrint(arena, "{d}", .{f.val});
        try publisher.publish(subj, out);
    }
}

/// Trampoline that adapts `Registry.arm` to the C-style hook fn pointer
/// patchbay expects.
pub fn hookFn(ctx: ?*anyopaque, rule: *Rule, slot_key: []const u8) void {
    const reg: *Registry = @ptrCast(@alignCast(ctx.?));
    reg.arm(rule, slot_key);
}
