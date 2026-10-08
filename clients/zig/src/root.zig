//! A Zig client for the Orly database over its WebSocket + JSON protocol (see
//! `docs/PROTOCOL.md` in the Orly repo). It owns the connection and session
//! lifecycle, builds orlyscript statements from Zig values, and returns the
//! raw JSON of each result. It is the same protocol the Go, Python and
//! TypeScript clients speak, not the packed binary protocol of the C++ client.
//!
//!     const orly = @import("orly");
//!
//!     var c = try orly.Client.connect(gpa, io, .{});
//!     defer c.close();
//!     _ = try c.newSession();
//!     try c.install("mypkg", 1);
//!     const pov = try c.newPov(.{});
//!     _ = try c.call(pov.slice(), "mypkg", "put", .{ .k = 1, .s = "hi" });
//!     const got = try c.call(pov.slice(), "mypkg", "get", .{ .k = 1 });
//!
//! Memory: a `Client` keeps one buffer for the statement it is building and
//! one for the reply, and reuses both, so a call allocates only when a
//! statement or reply is larger than any before it. The slice a call returns
//! points into the reply buffer and is valid until the next call on the same
//! client; copy it to keep it.
//!
//! Result quirks the engine returns (see `docs/PROTOCOL.md`): integers come
//! back as JSON floats, sets as unordered arrays, variants as `{"Tag": ...}`.

const std = @import("std");
const Io = std.Io;
const Allocator = std.mem.Allocator;

pub const ws = @import("ws.zig");
pub const literal = @import("literal.zig");

pub const raw = literal.raw;
pub const set = literal.set;
pub const Value = literal.Value;

/// The WebSocket endpoint a local `orlyi` listens on.
pub const default_url = "ws://127.0.0.1:8082/";

/// Every refusal the server has a typed status for, plus the client's own
/// failures. After a server error, `Client.detail()` holds the raw reply.
pub const Error = error{
    /// `"status": "insufficient_storage"`: the disk is full. Nothing was
    /// written, reads still work, and the write can be retried once space is
    /// freed.
    InsufficientStorage,
    /// `"status": "insufficient_memory"`: the update pools are down to the
    /// reserve kept for merges. Nothing was written; retry in a few seconds.
    InsufficientMemory,
    /// `"status": "write_too_large"`: the write holds more entries than the
    /// server could ever promote. Not retryable as sent: split the batch.
    WriteTooLarge,
    /// `"status": "read_too_large"`: the call walked more rows, or built more
    /// result memory, than the per-read budget. Read a narrower range.
    ReadTooLarge,
    /// `"status": "remote_compile_disabled"`: a compile statement sent to a
    /// server started without `--allow_remote_compile`. Compile with `orlyc`.
    RemoteCompileDisabled,
    /// `"status": "unauthorized"`: the server requires a token and this client
    /// presented none, or the wrong one. The server closes the connection.
    Unauthorized,
    /// Any other non-`ok` status (`source_error`, `exception`, ...).
    ServerError,
    /// The reply was not `{"status": ..., "result": ...}`.
    BadReply,
    /// The URL was not `ws://host[:port][/path]`.
    InvalidUrl,
    /// A token file or environment variable could not be read.
    BadToken,
    /// The connection could not be made, after all the retries.
    ConnectFailed,
} || ws.Error || Io.Reader.Error || Io.Writer.Error || Io.Cancelable || Allocator.Error || literal.Error;

pub const Options = struct {
    /// `ws://host[:port][/path]`. The host is an IP literal, `localhost` or a
    /// name the system can resolve.
    url: []const u8 = default_url,
    /// The server's shared secret (#710), presented as the first message. Null
    /// falls back to `ORLY_AUTH_TOKEN_FILE` / `ORLY_AUTH_TOKEN` from
    /// `environ_map`; with neither, none is presented.
    token: ?[]const u8 = null,
    /// The environment to read the token from. Zig 0.17 has no global
    /// environment, so pass `init.environ_map` from `main`, or null to skip.
    environ_map: ?*const std.process.Environ.Map = null,
    /// A just-started or loaded `orlyi` can briefly refuse the connection or
    /// time out the handshake. A failed connect is retried this many times,
    /// sleeping `backoff_ms` and doubling each time.
    retries: u32 = 5,
    backoff_ms: u32 = 250,
    /// The largest reply accepted, in bytes.
    max_message: usize = 256 * 1024 * 1024,
};

/// A session or POV id (a UUID), held inline so it can outlive the reply it
/// came from without an allocation.
pub const Id = struct {
    buf: [64]u8 = undefined,
    len: u8 = 0,

    pub fn slice(self: *const Id) []const u8 {
        return self.buf[0..self.len];
    }
};

pub const PovOptions = struct {
    /// `safe` waits for the write to be durable; `fast` does not. The grammar
    /// has no default, so one is always spelled out (#580).
    safe: bool = true,
    /// A shared POV can be used by other sessions; a private one is not.
    shared: bool = true,
    /// A POV id to create this one `from`.
    parent: ?[]const u8 = null,
};

/// One element of `callMany`: a method of a package and its arguments.
/// (Build a slice or tuple of these.)
pub fn CallOf(comptime Args: type) type {
    return struct { pkg: []const u8, method: []const u8, args: Args };
}

pub const Client = struct {
    gpa: Allocator,
    io: Io,
    stream: Io.net.Stream,
    reader: Io.net.Stream.Reader,
    writer: Io.net.Stream.Writer,
    read_buf: []u8,
    write_buf: []u8,
    /// The statement being built.
    stmt: std.ArrayList(u8) = .empty,
    /// The last reply, whole.
    reply: std.ArrayList(u8) = .empty,
    max_message: usize,
    /// The raw text of the last reply that was an error (empty if none yet).
    err_reply: std.ArrayList(u8) = .empty,

    /// Connects, retrying with exponential backoff, and presents the token if
    /// there is one. The client lives on the heap (its reader and writer point
    /// into it); `close` frees it.
    pub fn connect(gpa: Allocator, io: Io, options: Options) Error!*Client {
        var token_owned: ?[]u8 = null;
        defer if (token_owned) |t| gpa.free(t);
        const token: ?[]const u8 = options.token orelse blk: {
            token_owned = try tokenFromEnv(gpa, io, options.environ_map);
            break :blk token_owned;
        };

        const target = try parseUrl(options.url);
        var delay_ms: i64 = options.backoff_ms;
        var attempt: u32 = 0;
        while (true) : (attempt += 1) {
            if (connectOnce(gpa, io, target, token, options.max_message)) |client| {
                return client;
            } else |err| switch (err) {
                error.Unauthorized => return error.Unauthorized,
                error.OutOfMemory => return error.OutOfMemory,
                error.Canceled => return error.Canceled,
                else => if (attempt >= options.retries) return error.ConnectFailed,
            }
            try io.sleep(.fromMilliseconds(delay_ms), .awake);
            delay_ms *= 2;
        }
    }

    fn connectOnce(gpa: Allocator, io: Io, target: Target, token: ?[]const u8, max_message: usize) !*Client {
        const stream = try dial(io, target);
        errdefer stream.close(io);

        const self = try gpa.create(Client);
        errdefer gpa.destroy(self);
        const read_buf = try gpa.alloc(u8, 16 * 1024);
        errdefer gpa.free(read_buf);
        const write_buf = try gpa.alloc(u8, 16 * 1024);
        errdefer gpa.free(write_buf);
        self.* = .{
            .gpa = gpa,
            .io = io,
            .stream = stream,
            .reader = stream.reader(io, read_buf),
            .writer = stream.writer(io, write_buf),
            .read_buf = read_buf,
            .write_buf = write_buf,
            .max_message = max_message,
        };
        errdefer {
            self.stmt.deinit(gpa);
            self.reply.deinit(gpa);
            self.err_reply.deinit(gpa);
        }

        var key: [16]u8 = undefined;
        io.random(&key);
        try ws.handshake(&self.reader.interface, &self.writer.interface, target.host_header, target.path, &key);

        if (token) |t| if (t.len != 0) try self.authenticate(t);
        return self;
    }

    /// The token is the first message, `{"auth": "<token>"}`. A server with a
    /// token answers only `ok` or `unauthorized`; one without answers with an
    /// error status (it tried to parse the message as a statement) and the
    /// connection carries on, so clients can get the token before the server
    /// starts requiring it. The token never appears in an error or `detail`.
    fn authenticate(self: *Client, token: []const u8) Error!void {
        self.stmt.clearRetainingCapacity();
        try self.stmt.appendSlice(self.gpa, "{\"auth\": ");
        try self.stmt.append(self.gpa, '"');
        for (token) |ch| switch (ch) {
            '"', '\\' => try self.stmt.appendSlice(self.gpa, &.{ '\\', ch }),
            0...0x1f => try literal.print(self.gpa, &self.stmt, "\\u{x:0>4}", .{ch}),
            else => try self.stmt.append(self.gpa, ch),
        };
        try self.stmt.appendSlice(self.gpa, "\"}");
        const reply = self.exchange(self.stmt.items) catch |err| {
            self.stmt.clearRetainingCapacity();
            return err;
        };
        self.stmt.clearRetainingCapacity();
        if (std.mem.eql(u8, reply.status, "unauthorized")) {
            try self.keepError(reply);
            return error.Unauthorized;
        }
    }

    /// Releases the connection and everything the client holds.
    pub fn close(self: *Client) void {
        const gpa = self.gpa;
        self.stream.close(self.io);
        self.stmt.deinit(gpa);
        self.reply.deinit(gpa);
        self.err_reply.deinit(gpa);
        gpa.free(self.read_buf);
        gpa.free(self.write_buf);
        gpa.destroy(self);
    }

    /// The raw reply of the last call that failed with a server error, kept
    /// as detail for the typed error (the server's message is in its
    /// `result`). Valid until the next failure.
    pub fn detail(self: *const Client) []const u8 {
        return self.err_reply.items;
    }

    const Reply = struct { status: []const u8, result: []const u8 };

    fn exchange(self: *Client, text: []const u8) Error!Reply {
        var mask: [4]u8 = undefined;
        self.io.random(&mask);
        try ws.writeFrame(&self.writer.interface, .text, text, mask);
        var pong: [4]u8 = undefined;
        self.io.random(&pong);
        try ws.readMessage(self.gpa, &self.reader.interface, &self.writer.interface, &self.reply, self.max_message, pong);
        return parseReply(self.reply.items);
    }

    fn keepError(self: *Client, reply: Reply) Error!void {
        _ = reply;
        self.err_reply.clearRetainingCapacity();
        try self.err_reply.appendSlice(self.gpa, self.reply.items);
    }

    /// Sends one orlyscript statement and returns its result as raw JSON, or
    /// the typed error for a refusal. The slice is valid until the next call.
    pub fn send(self: *Client, statement: []const u8) Error![]const u8 {
        const reply = try self.exchange(statement);
        if (std.mem.eql(u8, reply.status, "ok")) return reply.result;
        try self.keepError(reply);
        return statusError(reply.status);
    }

    /// Sends the statement built in the client's own buffer.
    fn sendBuilt(self: *Client) Error![]const u8 {
        // `send` reads `reply`, not `stmt`, so the buffer can be passed as is.
        return self.send(self.stmt.items);
    }

    fn begin(self: *Client) void {
        self.stmt.clearRetainingCapacity();
    }

    /// Sends a statement whose result is a JSON string, and returns it as an Id.
    fn sendId(self: *Client, statement: []const u8) Error!Id {
        const result = try self.send(statement);
        return idFrom(result) orelse error.BadReply;
    }

    /// Opens a session on this connection.
    pub fn newSession(self: *Client) Error!Id {
        return self.sendId("new session;");
    }

    /// `install pkg.version;`
    pub fn install(self: *Client, pkg: []const u8, version: u32) Error!void {
        self.begin();
        try literal.print(self.gpa, &self.stmt, "install {s}.{d};", .{ pkg, version });
        _ = try self.sendBuilt();
    }

    /// `uninstall pkg.version;`
    pub fn uninstall(self: *Client, pkg: []const u8, version: u32) Error!void {
        self.begin();
        try literal.print(self.gpa, &self.stmt, "uninstall {s}.{d};", .{ pkg, version });
        _ = try self.sendBuilt();
    }

    /// Creates a POV: `new safe|fast shared|private pov [from {parent}];`.
    pub fn newPov(self: *Client, options: PovOptions) Error!Id {
        self.begin();
        try literal.print(self.gpa, &self.stmt, "new {s} {s} pov", .{
            if (options.safe) "safe" else "fast",
            if (options.shared) "shared" else "private",
        });
        if (options.parent) |p| try literal.print(self.gpa, &self.stmt, " from {{{s}}}", .{p});
        try self.stmt.append(self.gpa, ';');
        const result = try self.sendBuilt();
        return idFrom(result) orelse error.BadReply;
    }

    /// `try {pov} pkg method <{...}>;` with `args` a struct (or a `Value`
    /// record). Pass `.{}` for no arguments.
    pub fn call(self: *Client, pov: []const u8, pkg: []const u8, method: []const u8, args: anytype) Error![]const u8 {
        self.begin();
        try literal.writeCall(self.gpa, &self.stmt, pov, pkg, method, args);
        return self.sendBuilt();
    }

    /// One method, N argument records, folded into one transaction (#253):
    /// `try {pov} pkg method [<{...}>, ...];`. Returns a JSON array of the N
    /// results, in order. All or nothing: one bad record rejects the set, and
    /// every call reads the same pre-batch snapshot.
    pub fn callBatch(self: *Client, pov: []const u8, pkg: []const u8, method: []const u8, args_list: anytype) Error![]const u8 {
        self.begin();
        try literal.writeBatch(self.gpa, &self.stmt, pov, pkg, method, args_list);
        return self.sendBuilt();
    }

    /// Several different methods as one transaction (#255): `calls` is a slice
    /// or tuple of `.{ .pkg, .method, .args }`. Returns a JSON array of the
    /// results, which may differ in type. All or nothing, same snapshot.
    pub fn callMany(self: *Client, pov: []const u8, calls: anytype) Error![]const u8 {
        self.begin();
        try literal.writeMany(self.gpa, &self.stmt, pov, calls);
        return self.sendBuilt();
    }

    /// `pause {pov};` Returns `"paused"`.
    pub fn pause(self: *Client, pov: []const u8) Error![]const u8 {
        self.begin();
        try literal.print(self.gpa, &self.stmt, "pause {{{s}}};", .{pov});
        return self.sendBuilt();
    }

    /// `unpause {pov};` Returns `"unpaused"`.
    pub fn unpause(self: *Client, pov: []const u8) Error![]const u8 {
        self.begin();
        try literal.print(self.gpa, &self.stmt, "unpause {{{s}}};", .{pov});
        return self.sendBuilt();
    }

    /// `exit;` ends the session.
    pub fn exit(self: *Client) Error!void {
        _ = try self.send("exit;");
    }

    /// Keyset paging (#735): calls `pkg method` on `pov` page after page, and
    /// hands each page's rows (a raw JSON array) to `visit`. The method takes
    /// `args` plus a cursor, and returns `<{.rows: [...], .last: ...}>`: a
    /// page of rows and the cursor for the page after it, typically the last
    /// row's key, which the method gives to `keys (T) @ <[...]> after <[...]>`.
    /// `first_cursor` is the first page's cursor, as an orlyscript literal; it
    /// is sent as the argument named `cursor_name`. The server keeps no state
    /// between pages. Paging stops at an empty page, at a page shorter than
    /// `page_size` (if nonzero), or when `visit` returns false.
    ///
    /// The cursor is passed back as the raw JSON the engine returned, so an
    /// integer cursor comes back as `12.0`; pass `.int_cursors = true` to send
    /// integral numbers as ints.
    pub fn pages(
        self: *Client,
        pov: []const u8,
        pkg: []const u8,
        method: []const u8,
        args: anytype,
        opts: PageOptions,
        context: anytype,
        comptime visit: fn (@TypeOf(context), rows: []const u8) bool,
    ) Error!void {
        var cursor: std.ArrayList(u8) = .empty;
        defer cursor.deinit(self.gpa);
        try cursor.appendSlice(self.gpa, opts.first_cursor);
        while (true) {
            // The record is `args` plus the cursor: write args, then splice.
            self.begin();
            try literal.print(self.gpa, &self.stmt, "try {{{s}}} {s} {s} ", .{ pov, pkg, method });
            const at = self.stmt.items.len;
            try literal.write(self.gpa, &self.stmt, args);
            // `args` is `<{...}>`; reopen its closing `}>` to add the cursor.
            std.debug.assert(std.mem.endsWith(u8, self.stmt.items[at..], "}>"));
            self.stmt.shrinkRetainingCapacity(self.stmt.items.len - 2);
            if (self.stmt.items.len > at + 2) try self.stmt.appendSlice(self.gpa, ", ");
            try literal.print(self.gpa, &self.stmt, ".{s}: {s}}}>;", .{ opts.cursor_name, cursor.items });

            const result = try self.sendBuilt();
            const page = splitPage(result) orelse {
                try self.keepError(.{ .status = "bad_page", .result = result });
                return error.BadReply;
            };
            if (page.rows_len == 0) return;
            if (!visit(context, page.rows)) return;
            if (opts.page_size != 0 and page.rows_len < opts.page_size) return;
            cursor.clearRetainingCapacity();
            try appendCursor(self.gpa, &cursor, page.last, opts.int_cursors);
        }
    }
};

pub const PageOptions = struct {
    /// The first page's cursor, an orlyscript literal (`"0"`, `"\"\""`, `"[1, 2]"`).
    first_cursor: []const u8,
    /// The name of the method's cursor argument.
    cursor_name: []const u8 = "last",
    /// Stop at a page shorter than this (0: only at an empty page).
    page_size: usize = 0,
    /// Send integral numbers in the returned cursor back as ints.
    int_cursors: bool = true,
};

const Page = struct { rows: []const u8, rows_len: usize, last: []const u8 };

/// Splits `{"rows": [...], "last": ...}` (either order) into the raw rows
/// array, its length and the raw `last`.
fn splitPage(json: []const u8) ?Page {
    var buf: [4096]u8 = undefined;
    var fba = std.heap.FixedBufferAllocator.init(&buf);
    var scanner = std.json.Scanner.initCompleteInput(fba.allocator(), json);
    defer scanner.deinit();
    if ((scanner.next() catch return null) != .object_begin) return null;
    var page: Page = .{ .rows = "", .rows_len = 0, .last = "" };
    var have_rows = false;
    var have_last = false;
    while (true) {
        const key_tok = scanner.next() catch return null;
        const key = switch (key_tok) {
            .string => |s| s,
            .object_end => break,
            else => return null,
        };
        const start = scanner.cursor;
        const is_rows = std.mem.eql(u8, key, "rows");
        const is_last = std.mem.eql(u8, key, "last");
        var count: usize = 0;
        if (is_rows) {
            if ((scanner.peekNextTokenType() catch return null) != .array_begin) return null;
            _ = scanner.next() catch return null;
            while ((scanner.peekNextTokenType() catch return null) != .array_end) {
                scanner.skipValue() catch return null;
                count += 1;
            }
            _ = scanner.next() catch return null;
        } else {
            scanner.skipValue() catch return null;
        }
        const text = std.mem.trim(u8, json[start..scanner.cursor], ": \t\r\n");
        if (is_rows) {
            page.rows = text;
            page.rows_len = count;
            have_rows = true;
        } else if (is_last) {
            page.last = text;
            have_last = true;
        }
    }
    return if (have_rows and have_last) page else null;
}

/// Appends `last` as a cursor literal: a number with no fraction is written
/// as an int, because the engine returns ints as floats and a float is not an
/// int key.
fn appendCursor(gpa: Allocator, out: *std.ArrayList(u8), last: []const u8, int_cursors: bool) Allocator.Error!void {
    if (int_cursors) {
        if (std.fmt.parseFloat(f64, last)) |f| {
            if (std.math.isFinite(f) and f == @floor(f) and @abs(f) < 9.0e15) {
                try literal.print(gpa, out, "{d}", .{@as(i64, @intFromFloat(f))});
                return;
            }
        } else |_| {}
    }
    try out.appendSlice(gpa, last);
}

const Target = struct {
    host: []const u8,
    port: u16,
    path: []const u8,
    host_header: []const u8,
};

fn parseUrl(url: []const u8) Error!Target {
    const prefix = "ws://";
    if (!std.mem.startsWith(u8, url, prefix)) return error.InvalidUrl;
    const rest = url[prefix.len..];
    const slash = std.mem.indexOfScalar(u8, rest, '/');
    const authority = rest[0 .. slash orelse rest.len];
    const path = if (slash) |s| rest[s..] else "/";
    if (authority.len == 0) return error.InvalidUrl;
    var host = authority;
    var port: u16 = 80;
    if (authority[0] == '[') { // [v6]:port
        const end = std.mem.indexOfScalar(u8, authority, ']') orelse return error.InvalidUrl;
        host = authority[1..end];
        if (end + 1 < authority.len) {
            if (authority[end + 1] != ':') return error.InvalidUrl;
            port = std.fmt.parseInt(u16, authority[end + 2 ..], 10) catch return error.InvalidUrl;
        }
    } else if (std.mem.lastIndexOfScalar(u8, authority, ':')) |colon| {
        host = authority[0..colon];
        port = std.fmt.parseInt(u16, authority[colon + 1 ..], 10) catch return error.InvalidUrl;
    }
    if (host.len == 0) return error.InvalidUrl;
    return .{ .host = host, .port = port, .path = path, .host_header = authority };
}

fn dial(io: Io, target: Target) !Io.net.Stream {
    const opts: Io.net.IpAddress.ConnectOptions = .{ .mode = .stream };
    if (Io.net.IpAddress.parse(target.host, target.port)) |addr| {
        return addr.connect(io, opts);
    } else |_| {}
    const name = Io.net.HostName.init(target.host) catch return error.InvalidUrl;
    return name.connect(io, target.port, opts);
}

/// `ORLY_AUTH_TOKEN_FILE` (its contents, less a trailing newline) or else
/// `ORLY_AUTH_TOKEN`, or null if neither is set. The caller frees the result.
pub fn tokenFromEnv(gpa: Allocator, io: Io, environ_map: ?*const std.process.Environ.Map) Error!?[]u8 {
    const env = environ_map orelse return null;
    if (env.get("ORLY_AUTH_TOKEN_FILE")) |path| {
        if (path.len != 0) {
            const bytes = Io.Dir.cwd().readFileAlloc(io, path, gpa, .limited(64 * 1024)) catch |err| switch (err) {
                error.OutOfMemory => return error.OutOfMemory,
                else => return error.BadToken,
            };
            const trimmed = std.mem.trimEnd(u8, bytes, "\r\n");
            if (trimmed.len == bytes.len) return bytes;
            defer gpa.free(bytes);
            return try gpa.dupe(u8, trimmed);
        }
    }
    if (env.get("ORLY_AUTH_TOKEN")) |t| return try gpa.dupe(u8, t);
    return null;
}

fn statusError(status: []const u8) Error {
    const map = .{
        .{ "insufficient_storage", error.InsufficientStorage },
        .{ "insufficient_memory", error.InsufficientMemory },
        .{ "write_too_large", error.WriteTooLarge },
        .{ "read_too_large", error.ReadTooLarge },
        .{ "remote_compile_disabled", error.RemoteCompileDisabled },
        .{ "unauthorized", error.Unauthorized },
    };
    inline for (map) |m| if (std.mem.eql(u8, status, m[0])) return m[1];
    return error.ServerError;
}

/// Splits `{"status": "...", "result": <json>}` into the status and the raw
/// text of the result (empty if the reply has none).
fn parseReply(json: []const u8) Error!Client.Reply {
    var buf: [4096]u8 = undefined;
    var fba = std.heap.FixedBufferAllocator.init(&buf);
    var scanner = std.json.Scanner.initCompleteInput(fba.allocator(), json);
    defer scanner.deinit();
    if ((scanner.next() catch return error.BadReply) != .object_begin) return error.BadReply;
    var status: ?[]const u8 = null;
    var result: []const u8 = "";
    while (true) {
        const key = switch (scanner.next() catch return error.BadReply) {
            .string => |s| s,
            .object_end => break,
            else => return error.BadReply,
        };
        const is_status = std.mem.eql(u8, key, "status");
        const is_result = std.mem.eql(u8, key, "result");
        const start = scanner.cursor;
        if (is_status) {
            switch (scanner.next() catch return error.BadReply) {
                .string => |s| status = s,
                else => return error.BadReply,
            }
        } else {
            scanner.skipValue() catch return error.BadReply;
        }
        if (is_result) result = std.mem.trim(u8, json[start..scanner.cursor], ": \t\r\n");
    }
    return .{ .status = status orelse return error.BadReply, .result = result };
}

/// The string inside a JSON string result, as an Id (ids hold no escapes).
fn idFrom(result: []const u8) ?Id {
    if (result.len < 2 or result[0] != '"' or result[result.len - 1] != '"') return null;
    const inner = result[1 .. result.len - 1];
    if (inner.len > 64 or std.mem.indexOfScalar(u8, inner, '\\') != null) return null;
    var id: Id = .{ .len = @intCast(inner.len) };
    @memcpy(id.buf[0..inner.len], inner);
    return id;
}

test {
    _ = ws;
    _ = literal;
}

test "replies split into status and raw result" {
    const r = try parseReply("{\"status\": \"ok\", \"result\": [1, {\"a\": \"x\"}, 2.5]}");
    try std.testing.expectEqualStrings("ok", r.status);
    try std.testing.expectEqualStrings("[1, {\"a\": \"x\"}, 2.5]", r.result);
    const s = try parseReply("{\"result\":\"nope\",\"status\":\"write_too_large\"}");
    try std.testing.expectEqualStrings("write_too_large", s.status);
    try std.testing.expectEqualStrings("\"nope\"", s.result);
    const n = try parseReply("{\"status\":\"ok\",\"result\":12}");
    try std.testing.expectEqualStrings("12", n.result);
    try std.testing.expectError(error.BadReply, parseReply("[]"));
    try std.testing.expectError(error.BadReply, parseReply("{\"result\": 1}"));
}

test "each refusal has its own error" {
    try std.testing.expectEqual(error.InsufficientMemory, statusError("insufficient_memory"));
    try std.testing.expectEqual(error.InsufficientStorage, statusError("insufficient_storage"));
    try std.testing.expectEqual(error.WriteTooLarge, statusError("write_too_large"));
    try std.testing.expectEqual(error.ReadTooLarge, statusError("read_too_large"));
    try std.testing.expectEqual(error.Unauthorized, statusError("unauthorized"));
    try std.testing.expectEqual(error.RemoteCompileDisabled, statusError("remote_compile_disabled"));
    try std.testing.expectEqual(error.ServerError, statusError("source_error"));
}

test "urls" {
    const t = try parseUrl("ws://127.0.0.1:8082/");
    try std.testing.expectEqualStrings("127.0.0.1", t.host);
    try std.testing.expectEqual(@as(u16, 8082), t.port);
    try std.testing.expectEqualStrings("/", t.path);
    const u = try parseUrl("ws://localhost");
    try std.testing.expectEqual(@as(u16, 80), u.port);
    try std.testing.expectEqualStrings("/", u.path);
    const v = try parseUrl("ws://[::1]:9/x");
    try std.testing.expectEqualStrings("::1", v.host);
    try std.testing.expectEqual(@as(u16, 9), v.port);
    try std.testing.expectError(error.InvalidUrl, parseUrl("http://x/"));
}

test "ids" {
    const id = idFrom("\"0123-abc\"").?;
    try std.testing.expectEqualStrings("0123-abc", id.slice());
    try std.testing.expect(idFrom("12") == null);
}

test "pages split" {
    const p = splitPage("{\"rows\": [1, [2, 3], \"x\"], \"last\": 3.0}").?;
    try std.testing.expectEqualStrings("[1, [2, 3], \"x\"]", p.rows);
    try std.testing.expectEqual(@as(usize, 3), p.rows_len);
    try std.testing.expectEqualStrings("3.0", p.last);
    try std.testing.expect(splitPage("{\"rows\": []}") == null);
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(std.testing.allocator);
    try appendCursor(std.testing.allocator, &out, "3.0", true);
    try std.testing.expectEqualStrings("3", out.items);
    out.clearRetainingCapacity();
    try appendCursor(std.testing.allocator, &out, "\"k\"", true);
    try std.testing.expectEqualStrings("\"k\"", out.items);
}
