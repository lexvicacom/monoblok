//! Patchbay: an s-expression DSL for routing and conditioning streams of
//! (subject, payload) pairs. Input is a source string; output is calls to a
//! caller-supplied publisher. No transport, no I/O loop, no NATS.
//!
//! Public surface:
//!   - `eval`     compile (`loadRules`) + evaluate (`run`) patchbay forms
//!   - `subject`  subject validation + wildcard matching
//!   - `sexpr`    s-expression parser, used by callers that want to share
//!                the same source file for their own top-level forms
//!                (monoblok's bridge config, for example)

pub const eval = @import("eval.zig");
pub const builtins = @import("builtins.zig");
pub const subject = @import("subject.zig");
pub const sexpr = @import("sexpr.zig");
pub const kernel = @import("kernel.zig");

test {
    _ = eval;
    _ = builtins;
    _ = subject;
    _ = sexpr;
    _ = kernel;
}
