//! Public server module facade.
const core = @import("server/core.zig");

pub const BridgeStats = core.BridgeStats;
pub const Server = core.Server;

pub const default_max_pending_bytes = core.default_max_pending_bytes;
pub const default_ping_interval_ms = core.default_ping_interval_ms;
pub const default_stats_tick_ms = core.default_stats_tick_ms;
