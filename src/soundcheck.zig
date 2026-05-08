const std = @import("std");
const Io = std.Io;
const xev = @import("xev");

const patchbay = @import("patchbay");
const rules_mod = patchbay.eval;
const subject_mod = patchbay.subject;
const clock_mod = @import("clock.zig");

const default_linger_ms: u64 = 10_000;

pub const Options = struct {
    label: bool = false,
    linger_ms: u64 = default_linger_ms,
    trace: bool = false,
};

pub const Runner = struct {
    gpa: std.mem.Allocator,
    arena: std.mem.Allocator,
    io: Io,
    ruleset: rules_mod.RuleSet,
    opts: Options,
    loop: xev.Loop,
    stdout: ?*Io.Writer = null,
    clock_registry: ?*clock_mod.Registry = null,
    stop_timer: xev.Timer = undefined,
    stop_completion: xev.Completion = undefined,

    pub fn init(
        gpa: std.mem.Allocator,
        arena: std.mem.Allocator,
        io: Io,
        ruleset: rules_mod.RuleSet,
        opts: Options,
    ) !Runner {
        return .{
            .gpa = gpa,
            .arena = arena,
            .io = io,
            .ruleset = ruleset,
            .opts = opts,
            .loop = try xev.Loop.init(.{}),
        };
    }

    pub fn deinit(self: *Runner) void {
        if (self.clock_registry) |reg| {
            reg.deinit();
            self.gpa.destroy(reg);
            self.clock_registry = null;
            self.loop.run(.until_done) catch {};
        }
        self.loop.deinit();
    }

    pub fn run(self: *Runner) !void {
        var stdout_buf: [4096]u8 = undefined;
        var stdout_writer = Io.File.stdout().writerStreaming(self.io, &stdout_buf);
        self.stdout = &stdout_writer.interface;
        defer {
            self.stdout = null;
            stdout_writer.interface.flush() catch {};
        }

        if (rules_mod.rulesUseTimeWindows(self.ruleset.rules)) {
            const reg = try self.gpa.create(clock_mod.Registry);
            reg.* = try clock_mod.Registry.init(self.gpa, &self.loop, self.ruleset.rules, self.publisher());
            self.clock_registry = reg;
            _ = reg.armExisting();
        }

        var line: std.ArrayList(u8) = .empty;
        defer line.deinit(self.gpa);

        var read_buf: [4096]u8 = undefined;
        while (true) {
            const n = try std.posix.read(std.posix.STDIN_FILENO, &read_buf);
            if (n == 0) break;
            for (read_buf[0..n]) |b| {
                if (b == '\n') {
                    try self.handleLine(std.mem.trimEnd(u8, line.items, "\r"));
                    line.clearRetainingCapacity();
                    try self.pumpReady();
                } else {
                    try line.append(self.gpa, b);
                }
            }
        }
        if (line.items.len > 0) try self.handleLine(std.mem.trimEnd(u8, line.items, "\r"));
        try self.pumpReady();
        try self.linger();
    }

    fn handleLine(self: *Runner, line: []const u8) !void {
        if (line.len == 0) return;
        const sep = std.mem.indexOfScalar(u8, line, '|') orelse {
            std.debug.print("monoblok soundcheck: expected SUBJECT|payload, got: {s}\n", .{line});
            return;
        };
        const subject = std.mem.trim(u8, line[0..sep], " \t");
        const payload = line[sep + 1 ..];
        if (subject.len == 0) {
            std.debug.print("monoblok soundcheck: empty subject\n", .{});
            return;
        }
        subject_mod.validatePublish(subject) catch {
            std.debug.print("monoblok soundcheck: invalid subject: {s}\n", .{subject});
            return;
        };

        try self.writeRow(if (self.opts.label) "in" else null, subject, payload);

        var msg_arena: std.heap.ArenaAllocator = .init(self.gpa);
        defer msg_arena.deinit();
        const scratch = msg_arena.allocator();
        const subject_owned = try scratch.dupe(u8, subject);
        const payload_owned = try scratch.dupe(u8, payload);

        var ctx: rules_mod.Context = .{
            .subject = subject_owned,
            .payload = payload_owned,
            .publisher = self.publisher(),
            .arena = scratch,
            .gpa = self.gpa,
            .now_ms = self.loop.now(),
            .wall_ms = wallClockMs(),
            .trace = self.opts.trace,
            .reentry_ctx = self,
            .reentry_fn = reentry,
            .clock_hook_ctx = self.clock_registry,
            .clock_hook_fn = if (self.clock_registry != null) clock_mod.hookFn else null,
        };
        self.ruleset.run(&ctx) catch |err| {
            std.log.warn("soundcheck rule error: {s}", .{@errorName(err)});
        };
        try self.stdout.?.flush();
    }

    fn linger(self: *Runner) !void {
        if (self.clock_registry == null) return;
        if (self.opts.linger_ms == 0) {
            self.cancelClocks();
            try self.loop.run(.until_done);
            return;
        }
        self.stop_timer = try xev.Timer.init();
        self.stop_timer.run(&self.loop, &self.stop_completion, self.opts.linger_ms, Runner, self, onLingerDone);
        try self.loop.run(.until_done);
    }

    fn pumpReady(self: *Runner) !void {
        try self.loop.run(.no_wait);
    }

    fn cancelClocks(self: *Runner) void {
        if (self.clock_registry) |reg| {
            reg.deinit();
            self.gpa.destroy(reg);
            self.clock_registry = null;
        }
    }

    fn onLingerDone(
        self_opt: ?*Runner,
        _: *xev.Loop,
        _: *xev.Completion,
        _: xev.Timer.RunError!void,
    ) xev.CallbackAction {
        const self = self_opt.?;
        self.cancelClocks();
        self.stop_timer.deinit();
        return .disarm;
    }

    fn publisher(self: *Runner) rules_mod.Publisher {
        return .{ .ctx = self, .publish_fn = publish };
    }

    fn publish(ctx: *anyopaque, subject: []const u8, payload: []const u8) anyerror!void {
        const self: *Runner = @ptrCast(@alignCast(ctx));
        try self.writeRow(if (self.opts.label) "out" else null, subject, payload);
    }

    fn writeRow(self: *Runner, label: ?[]const u8, subject: []const u8, payload: []const u8) !void {
        const stdout = self.stdout.?;
        if (label) |l| {
            try stdout.print("{s}|{s}|{s}\n", .{ l, subject, payload });
        } else {
            try stdout.print("{s}|{s}\n", .{ subject, payload });
        }
    }

    fn reentry(
        reentry_ctx: ?*anyopaque,
        parent: *rules_mod.Context,
        subject: []const u8,
        payload: []const u8,
    ) anyerror!void {
        const self: *Runner = @ptrCast(@alignCast(reentry_ctx.?));
        var child: rules_mod.Context = .{
            .subject = subject,
            .payload = payload,
            .publisher = parent.publisher,
            .arena = parent.arena,
            .gpa = parent.gpa,
            .now_ms = parent.now_ms,
            .wall_ms = parent.wall_ms,
            .trace = parent.trace,
            .depth = parent.depth + 1,
            .max_depth = parent.max_depth,
            .reentry_ctx = parent.reentry_ctx,
            .reentry_fn = parent.reentry_fn,
            .clock_hook_ctx = parent.clock_hook_ctx,
            .clock_hook_fn = parent.clock_hook_fn,
        };
        try self.ruleset.run(&child);
    }
};

fn wallClockMs() i64 {
    var ts: std.posix.timespec = undefined;
    _ = std.posix.system.clock_gettime(std.posix.CLOCK.REALTIME, &ts);
    return @as(i64, @intCast(ts.sec)) *| std.time.ms_per_s +| @divFloor(ts.nsec, std.time.ns_per_ms);
}
