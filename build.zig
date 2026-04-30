const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // Bridge (outbound NATS connection via nats.zig — pure Zig, no system deps).
    const bridge = b.option(bool, "bridge", "Build the outbound NATS bridge") orelse true;

    const libxev_dep = b.dependency("libxev", .{
        .target = target,
        .optimize = optimize,
    });

    const nats_dep = if (bridge) b.dependency("nats", .{
        .target = target,
        .optimize = optimize,
    }) else null;

    const manifest_mod = b.createModule(.{ .root_source_file = b.path("build.zig.zon") });

    const build_options = b.addOptions();
    build_options.addOption(bool, "bridge", bridge);

    // Patchbay: the s-expression DSL evaluator, decoupled from the daemon.
    // Lives in lib/patchbay/ as a module so it can be embedded by other
    // hosts (CLI transformers, microcontroller firmware, etc.) without
    // dragging in monoblok's NATS server.
    const patchbay_mod = b.createModule(.{
        .root_source_file = b.path("lib/patchbay/src/patchbay.zig"),
        .target = target,
        .optimize = optimize,
    });

    const exe_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
        // Link libc so Zig's default gpa is the platform malloc (fast) rather
        // than the DebugAllocator it picks in ReleaseSafe otherwise. See
        // std.start `use_debug_allocator` for the rule (without this, Safe
        // pays canary/guard-page overhead on every allocation).
        .link_libc = true,
        .imports = &.{
            .{ .name = "xev", .module = libxev_dep.module("xev") },
            .{ .name = "manifest", .module = manifest_mod },
            .{ .name = "build_options", .module = build_options.createModule() },
            .{ .name = "patchbay", .module = patchbay_mod },
        },
    });

    if (bridge) {
        exe_mod.addImport("nats", nats_dep.?.module("nats"));
    }

    const exe = b.addExecutable(.{
        .name = "monoblok",
        .root_module = exe_mod,
    });
    b.installArtifact(exe);

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| run_cmd.addArgs(args);

    const run_step = b.step("run", "Run the daemon");
    run_step.dependOn(&run_cmd.step);

    const exe_tests = b.addTest(.{ .root_module = exe_mod });
    const run_exe_tests = b.addRunArtifact(exe_tests);

    const patchbay_tests = b.addTest(.{ .root_module = patchbay_mod });
    const run_patchbay_tests = b.addRunArtifact(patchbay_tests);

    const test_step = b.step("test", "Run tests");
    test_step.dependOn(&run_exe_tests.step);
    test_step.dependOn(&run_patchbay_tests.step);
}


