const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const orly = b.addModule("orly", .{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });

    const tests = b.addTest(.{ .root_module = orly });
    const test_step = b.step("test", "Run the unit tests (no server needed)");
    test_step.dependOn(&b.addRunArtifact(tests).step);

    // The example and the smoke driver are executables that import the module.
    inline for (.{
        .{ "example", "examples/basic.zig", "Build the example program" },
        .{ "smoke", "smoke/smoke.zig", "Build the smoke driver (run by clients/smoke/run-zig.sh)" },
    }) |def| {
        const exe = b.addExecutable(.{
            .name = def[0],
            .root_module = b.createModule(.{
                .root_source_file = b.path(def[1]),
                .target = target,
                .optimize = optimize,
                .imports = &.{.{ .name = "orly", .module = orly }},
            }),
        });
        const install = b.addInstallArtifact(exe, .{});
        b.step(def[0], def[2]).dependOn(&install.step);
        b.getInstallStep().dependOn(&install.step);
    }
}
