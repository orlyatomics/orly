//! A small program: install a package, open a POV, write and read.
//!
//!     zig build example
//!     ./zig-out/bin/example ws://127.0.0.1:8082/ mypkg
//!
//! `mypkg` must define `put = ...` taking `.k` and `.s`, and `get` taking `.k`
//! (see clients/smoke/multi.orly for the shape: `write_val` / `read_val`).
//! Set ORLY_AUTH_TOKEN (or ORLY_AUTH_TOKEN_FILE) if the server requires a token.

const std = @import("std");
const orly = @import("orly");

pub fn main(init: std.process.Init) !void {
    const gpa = init.gpa;
    const args = try init.minimal.args.toSlice(init.arena.allocator());
    const url = if (args.len > 1) args[1] else orly.default_url;
    const pkg = if (args.len > 2) args[2] else "multi";

    var out_buf: [1024]u8 = undefined;
    var out = std.Io.File.stdout().writerStreaming(init.io, &out_buf);
    const w = &out.interface;

    // The token comes from ORLY_AUTH_TOKEN(_FILE) because we pass the environment.
    const c = try orly.Client.connect(gpa, init.io, .{ .url = url, .environ_map = init.environ_map });
    defer c.close();

    const session = try c.newSession();
    try w.print("session {s}\n", .{session.slice()});
    try c.install(pkg, 1);

    const pov = try c.newPov(.{}); // new safe shared pov;
    _ = try c.call(pov.slice(), pkg, "write_val", .{ .n = 1, .x = 42 });
    const got = try c.call(pov.slice(), pkg, "read_val", .{ .n = 1 });
    try w.print("read_val 1 -> {s}\n", .{got});

    // One transaction, N records (#253): all or nothing.
    _ = try c.callBatch(pov.slice(), pkg, "write_val", .{ .{ .n = 2, .x = 20 }, .{ .n = 3, .x = 30 } });
    try w.flush();
}
