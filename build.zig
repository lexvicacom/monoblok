const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const libxev_dep = b.dependency("libxev", .{
        .target = target,
        .optimize = optimize,
    });

    const manifest_mod = b.createModule(.{ .root_source_file = b.path("build.zig.zon") });

    const exe_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
        // Link libc so Zig's default gpa is the platform malloc (fast) rather
        // than the DebugAllocator it picks in ReleaseSafe otherwise. See
        // std.start `use_debug_allocator` for the rule — without this, Safe
        // pays canary/guard-page overhead on every allocation.
        .link_libc = true,
        .imports = &.{
            .{ .name = "xev", .module = libxev_dep.module("xev") },
            .{ .name = "manifest", .module = manifest_mod },
        },
    });

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

    const test_step = b.step("test", "Run tests");
    test_step.dependOn(&run_exe_tests.step);

    // `zig build dist` — ReleaseSafe cross-compiles for common targets,
    // staged into dist/<triple>/ alongside patchbay.edn and the bench script.
    // Safe (not Fast) because these are the shippable binaries — network
    // input shouldn't silently miscompute on an overflow.
    const dist_step = b.step("dist", "Cross-compile release binaries for common targets into dist/");
    const dist_targets = [_][]const u8{
        "x86_64-linux-musl",
        "aarch64-linux-musl",
        "x86_64-windows-gnu",
    };
    for (dist_targets) |triple| {
        addDistTarget(b, libxev_dep, manifest_mod, dist_step, triple);
    }
}

fn addDistTarget(
    b: *std.Build,
    libxev_dep: *std.Build.Dependency,
    manifest_mod: *std.Build.Module,
    dist_step: *std.Build.Step,
    triple: []const u8,
) void {
    const query = std.Target.Query.parse(.{ .arch_os_abi = triple }) catch @panic("bad target triple");
    const resolved = b.resolveTargetQuery(query);

    const mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = resolved,
        .optimize = .ReleaseSafe,
        .link_libc = true,
        .imports = &.{
            .{ .name = "xev", .module = libxev_dep.module("xev") },
            .{ .name = "manifest", .module = manifest_mod },
        },
    });

    const exe = b.addExecutable(.{
        .name = "monoblok",
        .root_module = mod,
    });

    const install = b.addInstallArtifact(exe, .{
        .dest_dir = .{ .override = .{ .custom = b.fmt("../dist/{s}", .{triple}) } },
    });
    dist_step.dependOn(&install.step);

    const install_patchbay = b.addInstallFile(b.path("patchbay.edn"), b.fmt("../dist/{s}/patchbay.edn", .{triple}));
    dist_step.dependOn(&install_patchbay.step);

    const install_bench = b.addInstallFile(b.path("scripts/bench.sh"), b.fmt("../dist/{s}/bench.sh", .{triple}));
    dist_step.dependOn(&install_bench.step);
}
