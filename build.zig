const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // -Dforce-epoll=true forces libxev to use the epoll backend on Linux
    // instead of its io_uring default. Useful in container environments
    // where seccomp blocks io_uring_* syscalls (e.g. Docker 29.x default).
    // Silently ignored on non-Linux targets (kqueue / iocp stay the default).
    const force_epoll_opt = b.option(bool, "force-epoll", "force libxev's epoll backend instead of io_uring (Linux only)") orelse false;
    const target_os = target.result.os.tag;
    const force_epoll = force_epoll_opt and target_os == .linux;

    const libxev_dep = b.dependency("libxev", .{
        .target = target,
        .optimize = optimize,
    });

    const nats_dep = b.dependency("nats", .{
        .target = target,
        .optimize = optimize,
    });

    const manifest_mod = b.createModule(.{ .root_source_file = b.path("build.zig.zon") });

    // `xev_mod` is the module everything else imports as "xev". When
    // force-epoll is set, route imports through a thin shim that re-exports
    // libxev's epoll backend as if it were the default xev API.
    const xev_mod = if (force_epoll) blk: {
        const shim = b.createModule(.{
            .root_source_file = b.path("src/xev_epoll.zig"),
            .target = target,
            .optimize = optimize,
        });
        shim.addImport("xev", libxev_dep.module("xev"));
        break :blk shim;
    } else libxev_dep.module("xev");

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
            .{ .name = "xev", .module = xev_mod },
            .{ .name = "manifest", .module = manifest_mod },
            .{ .name = "patchbay", .module = patchbay_mod },
            .{ .name = "nats", .module = nats_dep.module("nats") },
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

    const patchbay_tests = b.addTest(.{ .root_module = patchbay_mod });
    const run_patchbay_tests = b.addRunArtifact(patchbay_tests);

    const test_step = b.step("test", "Run tests");
    test_step.dependOn(&run_exe_tests.step);
    test_step.dependOn(&run_patchbay_tests.step);

    // In-process router microbench. Forces ReleaseSafe since debug numbers
    // are useless for comparing routing-table changes. Run with:
    //   zig build bench-router -- [N] [M] [PUBS]
    const bench_router_mod = b.createModule(.{
        .root_source_file = b.path("src/bench_router.zig"),
        .target = target,
        .optimize = .ReleaseSafe,
        .link_libc = true,
        .imports = &.{
            .{ .name = "xev", .module = xev_mod },
            .{ .name = "manifest", .module = manifest_mod },
            .{ .name = "patchbay", .module = patchbay_mod },
            .{ .name = "nats", .module = nats_dep.module("nats") },
        },
    });
    const bench_router_exe = b.addExecutable(.{
        .name = "bench-router",
        .root_module = bench_router_mod,
    });
    const run_bench_router = b.addRunArtifact(bench_router_exe);
    if (b.args) |args| run_bench_router.addArgs(args);
    const bench_router_step = b.step("bench-router", "Run the in-process router microbench");
    bench_router_step.dependOn(&run_bench_router.step);

    // In-process patchbay microbench. ReleaseSafe for the same reasons.
    //   zig build bench-patchbay -- [MODE] [N] [PUBS]
    const bench_patchbay_mod = b.createModule(.{
        .root_source_file = b.path("src/bench_patchbay.zig"),
        .target = target,
        .optimize = .ReleaseSafe,
        .link_libc = true,
        .imports = &.{
            .{ .name = "patchbay", .module = patchbay_mod },
        },
    });
    const bench_patchbay_exe = b.addExecutable(.{
        .name = "bench-patchbay",
        .root_module = bench_patchbay_mod,
    });
    const run_bench_patchbay = b.addRunArtifact(bench_patchbay_exe);
    if (b.args) |args| run_bench_patchbay.addArgs(args);
    const bench_patchbay_step = b.step("bench-patchbay", "Run the in-process patchbay microbench");
    bench_patchbay_step.dependOn(&run_bench_patchbay.step);

    // In-process mixer microbench.
    //   zig build bench-mixer -- [PATH] [N] [PUBS]
    const bench_mixer_mod = b.createModule(.{
        .root_source_file = b.path("src/bench_mixer.zig"),
        .target = target,
        .optimize = .ReleaseSafe,
        .link_libc = true,
        .imports = &.{
            .{ .name = "xev", .module = xev_mod },
            .{ .name = "manifest", .module = manifest_mod },
            .{ .name = "patchbay", .module = patchbay_mod },
            .{ .name = "nats", .module = nats_dep.module("nats") },
        },
    });
    const bench_mixer_exe = b.addExecutable(.{
        .name = "bench-mixer",
        .root_module = bench_mixer_mod,
    });
    const run_bench_mixer = b.addRunArtifact(bench_mixer_exe);
    if (b.args) |args| run_bench_mixer.addArgs(args);
    const bench_mixer_step = b.step("bench-mixer", "Run the in-process mixer microbench");
    bench_mixer_step.dependOn(&run_bench_mixer.step);
}

