//! Builds orlyscript literals and statements from Zig values.
//!
//! Everything is appended to a caller-supplied `std.ArrayList(u8)`, so a client
//! reuses one buffer for every statement and allocates only when it grows.
//!
//! A Zig value maps to an orlyscript literal like this:
//!
//!     raw("now()")                 verbatim, un-encoded
//!     bool                         true / false
//!     integers, comptime_int       decimal
//!     floats, comptime_float       shortest round-trip decimal, always with a '.' or 'e'
//!     []const u8, *const [N]u8     "quoted, with \ and " escaped"
//!     struct (named fields)        record <{.a: 1, .b: "x"}>, fields in the order declared
//!     slice, array, tuple          list [a, b, ...] (`.{}` is the empty record <{}>; an empty list is an empty slice)
//!     set(slice)                   set {a, b, ...}
//!     ?T                           the value, or `null` is an error (orlyscript has no null literal)
//!     Value                        a runtime-built tree of the above

const std = @import("std");
const Allocator = std.mem.Allocator;
const List = std.ArrayList(u8);

pub const Error = error{
    /// The value has no orlyscript literal: a NaN or infinite float, a null
    /// optional, or a type this module does not know how to encode.
    NotEncodable,
} || Allocator.Error;

/// A value inserted as raw orlyscript, un-encoded, for expressions with no
/// literal form (`raw("now()")`). The caller is responsible for it being valid.
pub const RawText = struct {
    text: []const u8,
};

/// Shorthand for `RawText{ .text = text }`.
pub fn raw(text: []const u8) RawText {
    return .{ .text = text };
}

/// Encodes `items` (a slice, array or tuple) as a set literal `{a, b, ...}`
/// instead of a list.
pub fn SetOf(comptime T: type) type {
    return struct {
        items: T,
        pub const is_orly_set = true;
    };
}

/// Marks `items` to be encoded as a set literal.
pub fn set(items: anytype) SetOf(@TypeOf(items)) {
    return .{ .items = items };
}

/// A value built at run time, for data whose shape is not known at compile
/// time. Anything known at compile time is simpler as a plain Zig value.
pub const Value = union(enum) {
    raw: []const u8,
    boolean: bool,
    int: i64,
    uint: u64,
    float: f64,
    string: []const u8,
    record: []const Field,
    list: []const Value,
    set: []const Value,

    pub const Field = struct {
        name: []const u8,
        value: Value,
    };
};

/// Appends the literal for `v` to `out`.
pub fn write(gpa: Allocator, out: *List, v: anytype) Error!void {
    const T = @TypeOf(v);
    switch (@typeInfo(T)) {
        .bool => try out.appendSlice(gpa, if (v) "true" else "false"),
        .int, .comptime_int => try print(gpa, out, "{d}", .{v}),
        .float, .comptime_float => try writeFloat(gpa, out, v),
        .optional => if (v) |x| try write(gpa, out, x) else return error.NotEncodable,
        .pointer => |p| switch (p.size) {
            .slice => if (p.child == u8) try writeString(gpa, out, v) else try writeSeq(gpa, out, '[', ']', v),
            .one => switch (@typeInfo(p.child)) {
                .array => |a| if (a.child == u8) try writeString(gpa, out, v) else try writeSeq(gpa, out, '[', ']', v),
                else => try write(gpa, out, v.*),
            },
            else => return error.NotEncodable,
        },
        .array => try writeSeq(gpa, out, '[', ']', &v),
        .@"struct" => |s| {
            if (T == RawText) {
                try out.appendSlice(gpa, v.text);
            } else if (@hasDecl(T, "is_orly_set")) {
                try writeSeq(gpa, out, '{', '}', v.items);
            } else if (s.is_tuple and s.field_names.len != 0) { // `.{}` is the empty record
                try writeSeq(gpa, out, '[', ']', v);
            } else {
                try writeRecord(gpa, out, v);
            }
        },
        .@"union" => if (T == Value) try writeValue(gpa, out, v) else return error.NotEncodable,
        else => return error.NotEncodable,
    }
}

pub fn print(gpa: Allocator, out: *List, comptime fmt: []const u8, args: anytype) Allocator.Error!void {
    var buf: [128]u8 = undefined;
    const s = std.fmt.bufPrint(&buf, fmt, args) catch unreachable; // 128 bytes holds any integer
    try out.appendSlice(gpa, s);
}

fn writeFloat(gpa: Allocator, out: *List, x: anytype) Error!void {
    const f: f64 = x;
    if (!std.math.isFinite(f)) return error.NotEncodable;
    var buf: [512]u8 = undefined;
    const s = std.fmt.bufPrint(&buf, "{d}", .{f}) catch return error.NotEncodable;
    try out.appendSlice(gpa, s);
    // An integral float must not read back as an int literal.
    if (std.mem.indexOfAny(u8, s, ".eE") == null) try out.appendSlice(gpa, ".0");
}

/// Appends `s` as a quoted orlyscript string, escaping as the engine's lexer
/// expects: `\` and `"` with a backslash, and control characters as `\n`,
/// `\r`, `\t` or `\xNN` (the lexer refuses them raw).
pub fn writeString(gpa: Allocator, out: *List, s: []const u8) Allocator.Error!void {
    try out.append(gpa, '"');
    var start: usize = 0;
    for (s, 0..) |c, i| {
        const esc: ?[]const u8 = switch (c) {
            '\\' => "\\\\",
            '"' => "\\\"",
            '\n' => "\\n",
            '\r' => "\\r",
            '\t' => "\\t",
            else => null,
        };
        if (esc == null and c >= 0x20 and c != 0x7f) continue;
        try out.appendSlice(gpa, s[start..i]);
        if (esc) |e| {
            try out.appendSlice(gpa, e);
        } else {
            try print(gpa, out, "\\x{x:0>2}", .{c});
        }
        start = i + 1;
    }
    try out.appendSlice(gpa, s[start..]);
    try out.append(gpa, '"');
}

fn writeSeq(gpa: Allocator, out: *List, open: u8, close: u8, items: anytype) Error!void {
    try out.append(gpa, open);
    const I = @TypeOf(items);
    if (@typeInfo(I) == .@"struct") { // a tuple: element types differ
        inline for (items, 0..) |item, i| {
            if (i != 0) try out.appendSlice(gpa, ", ");
            try write(gpa, out, item);
        }
    } else {
        for (items, 0..) |item, i| {
            if (i != 0) try out.appendSlice(gpa, ", ");
            try write(gpa, out, item);
        }
    }
    try out.append(gpa, close);
}

fn writeRecord(gpa: Allocator, out: *List, v: anytype) Error!void {
    try out.appendSlice(gpa, "<{");
    inline for (@typeInfo(@TypeOf(v)).@"struct".field_names, 0..) |name, i| {
        if (i != 0) try out.appendSlice(gpa, ", ");
        try out.append(gpa, '.');
        try out.appendSlice(gpa, name);
        try out.appendSlice(gpa, ": ");
        try write(gpa, out, @field(v, name));
    }
    try out.appendSlice(gpa, "}>");
}

fn writeValue(gpa: Allocator, out: *List, v: Value) Error!void {
    switch (v) {
        .raw => |s| try out.appendSlice(gpa, s),
        .boolean => |b| try write(gpa, out, b),
        .int => |n| try write(gpa, out, n),
        .uint => |n| try write(gpa, out, n),
        .float => |f| try write(gpa, out, f),
        .string => |s| try writeString(gpa, out, s),
        .record => |fields| {
            try out.appendSlice(gpa, "<{");
            for (fields, 0..) |f, i| {
                if (i != 0) try out.appendSlice(gpa, ", ");
                try out.append(gpa, '.');
                try out.appendSlice(gpa, f.name);
                try out.appendSlice(gpa, ": ");
                try writeValue(gpa, out, f.value);
            }
            try out.appendSlice(gpa, "}>");
        },
        .list => |items| try writeSeq(gpa, out, '[', ']', items),
        .set => |items| try writeSeq(gpa, out, '{', '}', items),
    }
}

/// `try {pov} pkg method <args>;`
pub fn writeCall(gpa: Allocator, out: *List, pov: []const u8, pkg: []const u8, method: []const u8, args: anytype) Error!void {
    try print(gpa, out, "try {{{s}}} {s} {s} ", .{ pov, pkg, method });
    try write(gpa, out, args);
    try out.append(gpa, ';');
}

/// `try {pov} pkg method [args1, args2, ...];` -- `args_list` is a slice,
/// array or tuple of argument records, at least one.
pub fn writeBatch(gpa: Allocator, out: *List, pov: []const u8, pkg: []const u8, method: []const u8, args_list: anytype) Error!void {
    if (lenOf(args_list) == 0) return error.NotEncodable;
    try print(gpa, out, "try {{{s}}} {s} {s} ", .{ pov, pkg, method });
    try writeSeq(gpa, out, '[', ']', args_list);
    try out.append(gpa, ';');
}

/// `try {pov} [pkg1 method1 args1, pkg2 method2 args2, ...];` -- `calls` is a
/// slice, array or tuple of structs with `pkg`, `method` and `args` fields,
/// at least one.
pub fn writeMany(gpa: Allocator, out: *List, pov: []const u8, calls: anytype) Error!void {
    if (lenOf(calls) == 0) return error.NotEncodable;
    try print(gpa, out, "try {{{s}}} [", .{pov});
    if (@typeInfo(@TypeOf(calls)) == .@"struct") {
        inline for (calls, 0..) |call, i| {
            if (i != 0) try out.appendSlice(gpa, ", ");
            try writeOneOfMany(gpa, out, call);
        }
    } else {
        for (calls, 0..) |call, i| {
            if (i != 0) try out.appendSlice(gpa, ", ");
            try writeOneOfMany(gpa, out, call);
        }
    }
    try out.appendSlice(gpa, "];");
}

fn writeOneOfMany(gpa: Allocator, out: *List, call: anytype) Error!void {
    try print(gpa, out, "{s} {s} ", .{ call.pkg, call.method });
    try write(gpa, out, call.args);
}

fn lenOf(x: anytype) usize {
    return switch (@typeInfo(@TypeOf(x))) {
        .@"struct" => |s| s.field_names.len,
        .pointer => |p| if (p.size == .one) @typeInfo(p.child).array.len else x.len,
        else => x.len,
    };
}

fn expectLiteral(want: []const u8, v: anytype) !void {
    const gpa = std.testing.allocator;
    var out: List = .empty;
    defer out.deinit(gpa);
    try write(gpa, &out, v);
    try std.testing.expectEqualStrings(want, out.items);
}

test "scalars" {
    try expectLiteral("true", true);
    try expectLiteral("-42", @as(i64, -42));
    try expectLiteral("7", 7);
    try expectLiteral("18446744073709551615", @as(u64, std.math.maxInt(u64)));
    try expectLiteral("1.5", 1.5);
    try expectLiteral("2.0", @as(f64, 2));
    try expectLiteral("\"hi\"", "hi");
    try expectLiteral("\"a\\\"b\\\\c\"", "a\"b\\c");
    try expectLiteral("\"l1\\nl2\\t\\r\\x01\\x7f\"", "l1\nl2\t\r\x01\x7f");
    try expectLiteral("\"\"", "");
    try expectLiteral("now()", raw("now()"));
    try expectLiteral("5", @as(?u8, 5));
}

test "records, lists and sets" {
    try expectLiteral("<{.k: 1, .s: \"hi\"}>", .{ .k = 1, .s = "hi" });
    try expectLiteral("<{}>", .{});
    try expectLiteral("[1, 2, 3]", [_]u8{ 1, 2, 3 });
    try expectLiteral("[1, \"two\"]", .{ 1, "two" });
    const xs: []const i32 = &.{ 4, 5 };
    try expectLiteral("[4, 5]", xs);
    try expectLiteral("{1, 2}", set([_]u8{ 1, 2 }));
    try expectLiteral("<{.tags: {\"a\", \"b\"}, .nested: <{.n: 1}>}>", .{ .tags = set(.{ "a", "b" }), .nested = .{ .n = 1 } });
}

test "runtime values" {
    const v: Value = .{ .record = &.{
        .{ .name = "n", .value = .{ .int = 3 } },
        .{ .name = "xs", .value = .{ .list = &.{ .{ .string = "a" }, .{ .boolean = false } } } },
        .{ .name = "s", .value = .{ .set = &.{ .{ .uint = 1 } } } },
    } };
    try expectLiteral("<{.n: 3, .xs: [\"a\", false], .s: {1}}>", v);
}

test "what has no literal is refused" {
    const gpa = std.testing.allocator;
    var out: List = .empty;
    defer out.deinit(gpa);
    try std.testing.expectError(error.NotEncodable, write(gpa, &out, std.math.nan(f64)));
    try std.testing.expectError(error.NotEncodable, write(gpa, &out, std.math.inf(f64)));
    try std.testing.expectError(error.NotEncodable, write(gpa, &out, @as(?u8, null)));
    try std.testing.expectError(error.NotEncodable, write(gpa, &out, {}));
}

test "statements" {
    const gpa = std.testing.allocator;
    var out: List = .empty;
    defer out.deinit(gpa);

    try writeCall(gpa, &out, "pov-1", "multi", "write_val", .{ .n = 1, .x = 10 });
    try std.testing.expectEqualStrings("try {pov-1} multi write_val <{.n: 1, .x: 10}>;", out.items);

    out.clearRetainingCapacity();
    try writeBatch(gpa, &out, "p", "multi", "write_val", .{ .{ .n = 1, .x = 10 }, .{ .n = 2, .x = 20 } });
    try std.testing.expectEqualStrings("try {p} multi write_val [<{.n: 1, .x: 10}>, <{.n: 2, .x: 20}>];", out.items);

    out.clearRetainingCapacity();
    const recs = [_]struct { n: i32, x: i32 }{ .{ .n = 1, .x = 2 }, .{ .n = 3, .x = 4 } };
    try writeBatch(gpa, &out, "p", "m", "w", recs);
    try std.testing.expectEqualStrings("try {p} m w [<{.n: 1, .x: 2}>, <{.n: 3, .x: 4}>];", out.items);

    out.clearRetainingCapacity();
    try writeMany(gpa, &out, "p", .{
        .{ .pkg = "multi", .method = "write_val", .args = .{ .n = 1, .x = 10 } },
        .{ .pkg = "multi", .method = "write_name", .args = .{ .n = 1, .s = "alpha" } },
    });
    try std.testing.expectEqualStrings(
        "try {p} [multi write_val <{.n: 1, .x: 10}>, multi write_name <{.n: 1, .s: \"alpha\"}>];",
        out.items,
    );

    const empty: []const u8 = &.{};
    try std.testing.expectError(error.NotEncodable, writeBatch(gpa, &out, "p", "m", "w", empty));
}
