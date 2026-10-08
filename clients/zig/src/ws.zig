//! A minimal RFC 6455 WebSocket client: the opening handshake and text-message
//! framing. It works on `std.Io.Reader` / `std.Io.Writer`, so it neither knows
//! nor cares what carries the bytes, and tests drive it over fixed buffers.
//!
//! Only what the Orly protocol needs is here: one text message per statement
//! or reply, no extensions, no subprotocols. Fragmented messages, pings and
//! closes from the server are handled.

const std = @import("std");
const Io = std.Io;
const Allocator = std.mem.Allocator;

pub const guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

pub const Error = error{
    /// The server's reply to the upgrade request was not a valid 101.
    HandshakeFailed,
    /// The server sent bytes that are not a valid WebSocket frame.
    ProtocolError,
    /// The server closed the connection (a close frame, or end of stream).
    ConnectionClosed,
    /// A message was larger than the limit the caller set.
    MessageTooLarge,
};

pub const Opcode = enum(u4) {
    continuation = 0,
    text = 1,
    binary = 2,
    close = 8,
    ping = 9,
    pong = 10,
    _,
};

/// The `Sec-WebSocket-Accept` value for a `Sec-WebSocket-Key`.
pub fn acceptKey(key: []const u8, out: *[28]u8) void {
    var sha1 = std.crypto.hash.Sha1.init(.{});
    sha1.update(key);
    sha1.update(guid);
    var digest: [std.crypto.hash.Sha1.digest_length]u8 = undefined;
    sha1.final(&digest);
    _ = std.base64.standard.Encoder.encode(out, &digest);
}

/// Sends the upgrade request and checks the reply. `key_bytes` is 16 random
/// bytes. Anything the server sends after its headers stays in `reader`.
pub fn handshake(
    reader: *Io.Reader,
    writer: *Io.Writer,
    host_header: []const u8,
    path: []const u8,
    key_bytes: *const [16]u8,
) (Error || Io.Reader.Error || Io.Writer.Error)!void {
    var key: [24]u8 = undefined;
    _ = std.base64.standard.Encoder.encode(&key, key_bytes);
    try writer.print(
        "GET {s} HTTP/1.1\r\nHost: {s}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" ++
            "Sec-WebSocket-Key: {s}\r\nSec-WebSocket-Version: 13\r\n\r\n",
        .{ path, host_header, key },
    );
    try writer.flush();

    const status = try takeLine(reader);
    // "HTTP/1.1 101 Switching Protocols"
    var parts = std.mem.splitScalar(u8, status, ' ');
    _ = parts.next();
    const code = parts.next() orelse return error.HandshakeFailed;
    if (!std.mem.eql(u8, code, "101")) return error.HandshakeFailed;

    var want: [28]u8 = undefined;
    acceptKey(&key, &want);
    var accepted = false;
    while (true) {
        const line = try takeLine(reader);
        if (line.len == 0) break;
        const colon = std.mem.indexOfScalar(u8, line, ':') orelse return error.HandshakeFailed;
        const name = line[0..colon];
        const value = std.mem.trim(u8, line[colon + 1 ..], " \t");
        if (std.ascii.eqlIgnoreCase(name, "sec-websocket-accept")) {
            accepted = std.mem.eql(u8, value, &want);
        }
    }
    if (!accepted) return error.HandshakeFailed;
}

/// One header line without its CRLF. The slice points into the reader's buffer
/// and is valid until the next read.
fn takeLine(reader: *Io.Reader) (Error || Io.Reader.Error)![]const u8 {
    const raw = reader.takeDelimiterInclusive('\n') catch |err| switch (err) {
        error.StreamTooLong => return error.HandshakeFailed,
        error.EndOfStream => return error.ConnectionClosed,
        error.ReadFailed => return error.ReadFailed,
    };
    return std.mem.trimEnd(u8, raw, "\r\n");
}

/// Writes one complete, masked frame (a client must mask) and flushes it.
/// `mask` is 4 random bytes. The payload is masked through a small stack
/// buffer, so a large message is not copied.
pub fn writeFrame(
    writer: *Io.Writer,
    opcode: Opcode,
    payload: []const u8,
    mask: [4]u8,
) Io.Writer.Error!void {
    try writer.writeByte(0x80 | @as(u8, @intFromEnum(opcode)));
    if (payload.len < 126) {
        try writer.writeByte(0x80 | @as(u8, @intCast(payload.len)));
    } else if (payload.len <= std.math.maxInt(u16)) {
        try writer.writeByte(0x80 | 126);
        try writer.writeInt(u16, @intCast(payload.len), .big);
    } else {
        try writer.writeByte(0x80 | 127);
        try writer.writeInt(u64, payload.len, .big);
    }
    try writer.writeAll(&mask);

    var chunk: [1024]u8 = undefined;
    var i: usize = 0;
    while (i < payload.len) {
        const n = @min(chunk.len, payload.len - i);
        for (chunk[0..n], payload[i..][0..n], i..) |*dst, src, at| dst.* = src ^ mask[at % 4];
        try writer.writeAll(chunk[0..n]);
        i += n;
    }
    try writer.flush();
}

/// Reads the next data message (text or binary) into `out`, replacing its
/// contents. Pings are answered with a pong (using `pong_mask`), and a close
/// frame ends the connection with `error.ConnectionClosed`. A message longer
/// than `max_len` is `error.MessageTooLarge`, and the stream is then unusable.
pub fn readMessage(
    gpa: Allocator,
    reader: *Io.Reader,
    writer: *Io.Writer,
    out: *std.ArrayList(u8),
    max_len: usize,
    pong_mask: [4]u8,
) (Error || Io.Reader.Error || Io.Writer.Error || Allocator.Error)!void {
    out.clearRetainingCapacity();
    var in_message = false;
    while (true) {
        const b0 = try takeByte(reader);
        const b1 = try takeByte(reader);
        const fin = b0 & 0x80 != 0;
        if (b0 & 0x70 != 0) return error.ProtocolError; // no extensions negotiated
        const opcode: Opcode = @enumFromInt(@as(u4, @truncate(b0)));
        if (b1 & 0x80 != 0) return error.ProtocolError; // servers never mask
        var len: u64 = b1 & 0x7f;
        if (len == 126) {
            len = reader.takeInt(u16, .big) catch |e| return mapRead(e);
        } else if (len == 127) {
            len = reader.takeInt(u64, .big) catch |e| return mapRead(e);
            if (len >> 63 != 0) return error.ProtocolError;
        }

        switch (opcode) {
            .text, .binary, .continuation => {
                if ((opcode == .continuation) != in_message) return error.ProtocolError;
                if (len > max_len -| out.items.len) return error.MessageTooLarge;
                const n: usize = @intCast(len);
                const dst = try out.addManyAsSlice(gpa, n);
                reader.readSliceAll(dst) catch |e| return mapRead(e);
                in_message = !fin;
                if (fin) return;
            },
            .ping, .pong, .close => {
                if (!fin or len > 125) return error.ProtocolError;
                var control: [125]u8 = undefined;
                const body = control[0..@intCast(len)];
                reader.readSliceAll(body) catch |e| return mapRead(e);
                switch (opcode) {
                    .ping => try writeFrame(writer, .pong, body, pong_mask),
                    .close => return error.ConnectionClosed,
                    else => {},
                }
            },
            _ => return error.ProtocolError,
        }
    }
}

fn takeByte(reader: *Io.Reader) (Error || Io.Reader.Error)!u8 {
    return reader.takeByte() catch |e| mapRead(e);
}

fn mapRead(err: Io.Reader.Error) (Error || Io.Reader.Error) {
    return switch (err) {
        error.EndOfStream => error.ConnectionClosed,
        error.ReadFailed => error.ReadFailed,
    };
}

test "accept key matches the RFC 6455 example" {
    var out: [28]u8 = undefined;
    acceptKey("dGhlIHNhbXBsZSBub25jZQ==", &out);
    try std.testing.expectEqualStrings("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", &out);
}

test "frames are masked and sized for 7, 16 and 64 bit lengths" {
    const gpa = std.testing.allocator;
    const mask: [4]u8 = .{ 1, 2, 3, 4 };
    for ([_]usize{ 0, 5, 125, 126, 70_000 }) |len| {
        const payload = try gpa.alloc(u8, len);
        defer gpa.free(payload);
        for (payload, 0..) |*b, i| b.* = @truncate(i *% 7);

        var aw: Io.Writer.Allocating = .init(gpa);
        defer aw.deinit();
        try writeFrame(&aw.writer, .text, payload, mask);
        const wire = aw.written();

        try std.testing.expectEqual(@as(u8, 0x81), wire[0]);
        const head: usize = if (len < 126) 2 else if (len <= 65535) 4 else 10;
        try std.testing.expectEqual(len + head + 4, wire.len);
        try std.testing.expect(wire[1] & 0x80 != 0);
        try std.testing.expectEqualSlices(u8, &mask, wire[head..][0..4]);
        for (wire[head + 4 ..], payload, 0..) |got, want, i| {
            try std.testing.expectEqual(want ^ mask[i % 4], got);
        }
    }
}

test "readMessage reassembles fragments and answers a ping" {
    const gpa = std.testing.allocator;
    // "hel" (text, not final) / ping "p" / "lo" (continuation, final).
    const wire = [_]u8{ 0x01, 3, 'h', 'e', 'l' } ++ [_]u8{ 0x89, 1, 'p' } ++ [_]u8{ 0x80, 2, 'l', 'o' };
    var reader: Io.Reader = .fixed(&wire);
    var sent: [64]u8 = undefined;
    var writer: Io.Writer = .fixed(&sent);
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    try readMessage(gpa, &reader, &writer, &out, 1024, .{ 0, 0, 0, 0 });
    try std.testing.expectEqualStrings("hello", out.items);
    // The pong: FIN|pong, masked, length 1, mask 0, payload 'p'.
    try std.testing.expectEqualSlices(u8, &.{ 0x8a, 0x81, 0, 0, 0, 0, 'p' }, writer.buffered());
}

test "readMessage refuses a too-large message, a masked frame and a close" {
    const gpa = std.testing.allocator;
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(gpa);
    var sent: [16]u8 = undefined;

    var big: Io.Reader = .fixed(&.{ 0x81, 10, 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x' });
    var w1: Io.Writer = .fixed(&sent);
    try std.testing.expectError(error.MessageTooLarge, readMessage(gpa, &big, &w1, &out, 4, .{ 0, 0, 0, 0 }));

    var masked: Io.Reader = .fixed(&.{ 0x81, 0x81, 0, 0, 0, 0, 'x' });
    var w2: Io.Writer = .fixed(&sent);
    try std.testing.expectError(error.ProtocolError, readMessage(gpa, &masked, &w2, &out, 4, .{ 0, 0, 0, 0 }));

    var close: Io.Reader = .fixed(&.{ 0x88, 2, 0x03, 0xf0 });
    var w3: Io.Writer = .fixed(&sent);
    try std.testing.expectError(error.ConnectionClosed, readMessage(gpa, &close, &w3, &out, 4, .{ 0, 0, 0, 0 }));

    var eof: Io.Reader = .fixed(&.{});
    var w4: Io.Writer = .fixed(&sent);
    try std.testing.expectError(error.ConnectionClosed, readMessage(gpa, &eof, &w4, &out, 4, .{ 0, 0, 0, 0 }));
}

test "handshake checks the status and the accept key" {
    const key_bytes: [16]u8 = .{ 0x74, 0x68, 0x65, 0x20, 0x73, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x20, 0x6e, 0x6f, 0x6e, 0x63, 0x65 };
    var sent: [512]u8 = undefined;

    var ok: Io.Reader = .fixed("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n" ++
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\nrest");
    var w1: Io.Writer = .fixed(&sent);
    try handshake(&ok, &w1, "127.0.0.1:8082", "/", &key_bytes);
    try std.testing.expect(std.mem.startsWith(u8, w1.buffered(), "GET / HTTP/1.1\r\nHost: 127.0.0.1:8082\r\n"));
    try std.testing.expect(std.mem.indexOf(u8, w1.buffered(), "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") != null);
    try std.testing.expectEqualStrings("rest", try ok.take(4)); // left for the framing

    var wrong_key: Io.Reader = .fixed("HTTP/1.1 101 x\r\nSec-WebSocket-Accept: nope\r\n\r\n");
    var w2: Io.Writer = .fixed(&sent);
    try std.testing.expectError(error.HandshakeFailed, handshake(&wrong_key, &w2, "h", "/", &key_bytes));

    var refused: Io.Reader = .fixed("HTTP/1.1 400 Bad Request\r\n\r\n");
    var w3: Io.Writer = .fixed(&sent);
    try std.testing.expectError(error.HandshakeFailed, handshake(&refused, &w3, "h", "/", &key_bytes));
}
