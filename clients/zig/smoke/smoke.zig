//! The Zig client's smoke driver, run by clients/smoke/run-zig.sh against a
//! live orlyi.
//!
//!     smoke matrix <ws-url>             the new-POV matrix, pause/unpause, batches
//!     smoke auth <ws-url> <token>       the refusals of a token-enabled orlyi
//!     smoke token-env <ws-url>          connects with the token from the environment
//!
//! `matrix` needs clients/smoke/multi.orly installed as package `multi`.

const std = @import("std");
const orly = @import("orly");

var failures: u32 = 0;

fn check(ok: bool, comptime what: []const u8, args: anytype) void {
    if (ok) {
        std.debug.print("  ok   " ++ what ++ "\n", args);
    } else {
        failures += 1;
        std.debug.print("  FAIL " ++ what ++ "\n", args);
    }
}

fn number(json: []const u8) ?f64 {
    return std.fmt.parseFloat(f64, json) catch null;
}

fn isNumber(json: []const u8, want: f64) bool {
    return if (number(json)) |n| n == want else false;
}

pub fn main(init: std.process.Init) !void {
    const gpa = init.gpa;
    const args = try init.minimal.args.toSlice(init.arena.allocator());
    if (args.len < 3) {
        std.debug.print("usage: smoke matrix|auth|token-env <ws-url> [token]\n", .{});
        std.process.exit(2);
    }
    const mode = args[1];
    const url = args[2];

    if (std.mem.eql(u8, mode, "matrix")) {
        try matrix(gpa, init.io, url);
    } else if (std.mem.eql(u8, mode, "auth")) {
        if (args.len < 4) std.process.exit(2);
        try auth(gpa, init.io, url, args[3]);
    } else if (std.mem.eql(u8, mode, "token-env")) {
        try tokenEnv(gpa, init.io, url, init.environ_map);
    } else {
        std.debug.print("unknown mode {s}\n", .{mode});
        std.process.exit(2);
    }
    if (failures != 0) {
        std.debug.print("ZIG SMOKE FAIL: {d} check(s)\n", .{failures});
        std.process.exit(1);
    }
    std.debug.print("ZIG SMOKE OK ({s})\n", .{mode});
}

const PageCount = struct {
    pages: u32 = 0,
    rows: u32 = 0,
    sum: f64 = 0,

    fn visit(self: *PageCount, rows: []const u8) bool {
        self.pages += 1;
        var it = std.mem.tokenizeAny(u8, rows, "[], ");
        while (it.next()) |tok| {
            self.rows += 1;
            self.sum += number(tok) orelse 0;
        }
        return self.pages < 3;
    }
};

fn matrix(gpa: std.mem.Allocator, io: std.Io, url: []const u8) !void {
    const c = try orly.Client.connect(gpa, io, .{ .url = url, .retries = 10 });
    defer c.close();
    _ = try c.newSession();
    try c.install("multi", 1);

    // Every POV flavour, with and without a parent (#580), written and read back.
    var n: i64 = 0;
    for ([_]bool{ true, false }) |safe| {
        for ([_]bool{ true, false }) |shared| {
            for ([_]bool{ false, true }) |with_parent| {
                n += 1;
                var parent: orly.Id = undefined;
                if (with_parent) parent = try c.newPov(.{});
                const pov = try c.newPov(.{
                    .safe = safe,
                    .shared = shared,
                    .parent = if (with_parent) parent.slice() else null,
                });
                _ = try c.call(pov.slice(), "multi", "write_val", .{ .n = n, .x = n * 11 });
                const got = try c.call(pov.slice(), "multi", "read_val", .{ .n = n });
                check(isNumber(got, @floatFromInt(n * 11)), "new pov safe={} shared={} parent={}: read {s}", .{ safe, shared, with_parent, got });
            }
        }
    }

    // pause and unpause reach the server in its own syntax; the POV still works after.
    const parent = try c.newPov(.{});
    const pov = try c.newPov(.{ .parent = parent.slice() });
    check(std.mem.eql(u8, try c.pause(pov.slice()), "\"paused\""), "pause -> \"paused\"", .{});
    check(std.mem.eql(u8, try c.unpause(pov.slice()), "\"unpaused\""), "unpause -> \"unpaused\"", .{});
    _ = try c.call(pov.slice(), "multi", "write_val", .{ .n = 1000, .x = 7 });
    check(isNumber(try c.call(pov.slice(), "multi", "read_val", .{ .n = 1000 }), 7), "write and read after unpause", .{});

    const p = (try c.newPov(.{})).slice();
    var pov_buf = orly.Id{};
    @memcpy(pov_buf.buf[0..p.len], p);
    pov_buf.len = @intCast(p.len);
    const b = pov_buf.slice();

    // A mixed batch: results keep their own types, and every write lands.
    const mixed = try c.callMany(b, .{
        .{ .pkg = "multi", .method = "write_val", .args = .{ .n = 1, .x = 10 } },
        .{ .pkg = "multi", .method = "write_name", .args = .{ .n = 1, .s = "alpha" } },
        .{ .pkg = "multi", .method = "write_val", .args = .{ .n = 2, .x = 20 } },
    });
    check(std.mem.eql(u8, mixed, "[true,\"named\",true]") or std.mem.eql(u8, mixed, "[true, \"named\", true]"), "mixed batch results {s}", .{mixed});
    check(isNumber(try c.call(b, "multi", "read_val", .{ .n = 2 }), 20), "mixed batch write landed", .{});
    const name = try c.call(b, "multi", "read_name", .{ .n = 1 });
    check(std.mem.eql(u8, name, "\"alpha\""), "mixed batch string write landed: {s}", .{name});

    // A batch with a bad call fails as a whole and leaves nothing behind.
    if (c.callMany(b, .{
        .{ .pkg = "multi", .method = "write_val", .args = .{ .n = 300, .x = 30 } },
        .{ .pkg = "multi", .method = "no_such_method", .args = .{} },
    })) |_| {
        check(false, "a batch with a bad call was accepted", .{});
    } else |err| {
        check(err == error.ServerError, "failing batch -> ServerError ({s})", .{@errorName(err)});
        check(c.detail().len != 0, "the raw reply is kept as detail: {s}", .{c.detail()});
    }
    const left = try c.call(b, "multi", "read_val", .{ .n = 300 });
    check(std.mem.eql(u8, left, "null"), "a failed batch left nothing behind (read {s})", .{left});

    // Same-method batch (#253).
    const same = try c.callBatch(b, "multi", "write_val", .{ .{ .n = 5, .x = 50 }, .{ .n = 6, .x = 60 } });
    check(std.mem.startsWith(u8, same, "[true"), "same-method batch {s}", .{same});
    check(isNumber(try c.call(b, "multi", "read_val", .{ .n = 6 }), 60), "same-method batch write landed", .{});

    // A string with quotes, backslashes and a newline survives the round trip.
    _ = try c.call(b, "multi", "write_name", .{ .n = 9, .s = "a\"b\\c\nd" });
    const tricky = try c.call(b, "multi", "read_name", .{ .n = 9 });
    check(std.mem.eql(u8, tricky, "\"a\\\"b\\\\c\\nd\""), "escaped string round trip: {s}", .{tricky});

    // Keyset paging (#735): pages() sends the cursor back as an int.
    var seen = PageCount{};
    try c.pages(b, "multi", "page", .{}, .{ .first_cursor = "0", .page_size = 2 }, &seen, PageCount.visit);
    check(seen.pages == 3 and seen.rows == 6 and seen.sum == 21, "pages: {d} pages, {d} rows, sum {d}", .{ seen.pages, seen.rows, seen.sum });

    // A statement that does not compile is a plain server error, with detail.
    if (c.send("this is not orlyscript;")) |_| {
        check(false, "garbage was accepted", .{});
    } else |err| check(err == error.ServerError, "garbage statement -> ServerError ({s})", .{@errorName(err)});
}

fn auth(gpa: std.mem.Allocator, io: std.Io, url: []const u8, token: []const u8) !void {
    const wrong = "0123456789abcdef-not-the-token";

    // No token: connecting works (nothing is sent), the first statement is refused.
    {
        const c = try orly.Client.connect(gpa, io, .{ .url = url, .retries = 0 });
        defer c.close();
        if (c.newSession()) |_| {
            check(false, "a connection with no token was accepted", .{});
        } else |err| {
            check(err == error.Unauthorized, "no token -> Unauthorized ({s})", .{@errorName(err)});
            check(std.mem.indexOf(u8, c.detail(), "unauthorized") != null, "the raw refusal is the detail: {s}", .{c.detail()});
        }
    }
    // Wrong token: refused at connect, and never echoed.
    if (orly.Client.connect(gpa, io, .{ .url = url, .token = wrong, .retries = 0 })) |c| {
        c.close();
        check(false, "a wrong token was accepted", .{});
    } else |err| check(err == error.Unauthorized, "wrong token -> Unauthorized ({s})", .{@errorName(err)});
    // Right token.
    {
        const c = try orly.Client.connect(gpa, io, .{ .url = url, .token = token, .retries = 0 });
        defer c.close();
        const s = try c.newSession();
        check(s.len != 0, "right token -> new session", .{});
        const echoed = try c.send("echo 'hello';");
        check(std.mem.eql(u8, echoed, "\"hello\""), "right token -> statements run: {s}", .{echoed});
        // Compiling remotely is off by default (#705): its own typed error.
        if (c.send("compile \"x = 42;\";")) |_| {
            check(false, "a remote compile was accepted", .{});
        } else |err| check(err == error.RemoteCompileDisabled, "remote compile -> RemoteCompileDisabled ({s})", .{@errorName(err)});
    }
}

fn tokenEnv(gpa: std.mem.Allocator, io: std.Io, url: []const u8, env: *const std.process.Environ.Map) !void {
    const c = try orly.Client.connect(gpa, io, .{ .url = url, .environ_map = env, .retries = 0 });
    defer c.close();
    const s = try c.newSession();
    check(s.len != 0, "token from the environment -> new session", .{});
}
