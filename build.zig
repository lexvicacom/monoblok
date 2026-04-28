const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // Bridge (outbound NATS connection via nats.c). Default on; disable with
    // -Dbridge=false if you don't have OpenSSL installed.
    const bridge = b.option(bool, "bridge", "Build the outbound NATS bridge (requires system OpenSSL)") orelse true;

    const libxev_dep = b.dependency("libxev", .{
        .target = target,
        .optimize = optimize,
    });

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

    if (bridge) addNatsC(b, exe_mod);

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

fn addNatsC(b: *std.Build, mod: *std.Build.Module) void {
    const nats_src = b.path("vendor/nats.c/src");

    // Core sources. We drop stan (#if NATS_HAS_STREAMING is never defined, so
    // it's inert) and the win/ subdir. js/jsm/kv/object/micro are compiled
    // because conn.c / sub.c / dispatch.c reference their symbols; we just
    // don't expose those APIs from Zig.
    const core_sources = [_][]const u8{
        "asynccb.c",        "buf.c",         "comsock.c",          "conn.c",
        "crypto.c",         "dispatch.c",    "hash.c",             "js.c",
        "jsm.c",            "kv.c",          "micro.c",            "micro_client.c",
        "micro_endpoint.c", "micro_error.c", "micro_monitoring.c", "micro_request.c",
        "msg.c",            "nats.c",        "natstime.c",         "nkeys.c",
        "nuid.c",           "object.c",      "opts.c",             "parser.c",
        "pub.c",            "srvpool.c",     "stats.c",            "status.c",
        "sub.c",            "timer.c",       "url.c",              "util.c",
    };
    const glib_sources = [_][]const u8{
        "glib/glib.c",               "glib/glib_async_cb.c",
        "glib/glib_dispatch_pool.c", "glib/glib_gc.c",
        "glib/glib_last_error.c",    "glib/glib_ssl.c",
        "glib/glib_timer.c",
    };
    const unix_sources = [_][]const u8{
        "unix/cond.c", "unix/mutex.c", "unix/sock.c", "unix/thread.c",
    };

    const c_flags = [_][]const u8{
        "-D_REENTRANT",
        // _GNU_SOURCE must be set on the command line, not via n-unix.h.
        // Several nats.c sources (js.c, jsm.c, ...) include <ctype.h> /
        // <limits.h> before n-unix.h, so glibc has already latched its
        // feature set by the time the header's #define runs, and
        // asprintf stays hidden. macOS libc exposes asprintf unconditionally,
        // which is why this only breaks the Linux build.
        "-D_GNU_SOURCE",
        "-DNATS_STATIC",
        "-DNATS_HAS_TLS",
        "-Wno-deprecated-declarations",
    };

    mod.addCSourceFiles(.{ .root = nats_src, .files = &core_sources, .flags = &c_flags });
    mod.addCSourceFiles(.{ .root = nats_src, .files = &glib_sources, .flags = &c_flags });
    mod.addCSourceFiles(.{ .root = nats_src, .files = &unix_sources, .flags = &c_flags });

    mod.addIncludePath(nats_src);
    mod.addIncludePath(b.path("vendor/nats.c/src/include"));
    mod.addIncludePath(b.path("vendor/nats.c/src/unix"));
    mod.addIncludePath(b.path("vendor/nats.c/src/glib"));

    // Dyn-link OpenSSL via pkg-config (Homebrew on macOS, pkgconf on Linux).
    mod.linkSystemLibrary("openssl", .{ .use_pkg_config = .force });
}

