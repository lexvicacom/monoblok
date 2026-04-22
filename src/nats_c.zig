// Hand-written bindings for the subset of nats.c we use.
//
// Not using @cImport(nats.h) because translate-c was flaky under Zig 0.16.0
// when this was written (same issue zigxll-nats hit cross-compiling to MSVC).
// Hand-writing the declarations for what we actually call is cheap — this is
// a small outbound surface (connect, publish, close).

pub const natsConnection = opaque {};
pub const natsOptions = opaque {};

pub const natsStatus = c_int;
pub const NATS_OK: natsStatus = 0;
pub const NATS_ERR: natsStatus = 1;
pub const NATS_NO_MEMORY: natsStatus = 24;

pub extern fn nats_Open(lockSpinCount: i64) natsStatus;
pub extern fn nats_Close() void;
pub extern fn nats_GetVersion() [*c]const u8;
pub extern fn nats_GetLastError(status: ?*natsStatus) [*c]const u8;

pub extern fn natsOptions_Create(newOpts: *?*natsOptions) natsStatus;
pub extern fn natsOptions_Destroy(opts: ?*natsOptions) void;
pub extern fn natsOptions_SetURL(opts: *natsOptions, url: [*c]const u8) natsStatus;
pub extern fn natsOptions_SetServers(opts: *natsOptions, servers: [*c][*c]const u8, serversCount: c_int) natsStatus;
pub extern fn natsOptions_SetName(opts: *natsOptions, name: [*c]const u8) natsStatus;
pub extern fn natsOptions_SetUserInfo(opts: *natsOptions, user: [*c]const u8, password: [*c]const u8) natsStatus;
pub extern fn natsOptions_SetToken(opts: *natsOptions, token: [*c]const u8) natsStatus;
pub extern fn natsOptions_SetUserCredentialsFromFiles(opts: *natsOptions, userOrChainedFile: [*c]const u8, seedFile: [*c]const u8) natsStatus;
pub extern fn natsOptions_SetSecure(opts: *natsOptions, secure: bool) natsStatus;
pub extern fn natsOptions_LoadCATrustedCertificates(opts: *natsOptions, fileName: [*c]const u8) natsStatus;
pub extern fn natsOptions_LoadCertificatesChain(opts: *natsOptions, certsFileName: [*c]const u8, keyFileName: [*c]const u8) natsStatus;
pub extern fn natsOptions_SkipServerVerification(opts: *natsOptions, skip: bool) natsStatus;
pub extern fn natsOptions_SetTimeout(opts: *natsOptions, timeout: i64) natsStatus;
pub extern fn natsOptions_SetPingInterval(opts: *natsOptions, interval: i64) natsStatus;
pub extern fn natsOptions_SetMaxReconnect(opts: *natsOptions, maxReconnect: c_int) natsStatus;
pub extern fn natsOptions_SetReconnectWait(opts: *natsOptions, reconnectWait: i64) natsStatus;

pub extern fn natsConnection_Connect(nc: *?*natsConnection, options: ?*natsOptions) natsStatus;
pub extern fn natsConnection_Close(nc: *natsConnection) void;
pub extern fn natsConnection_Destroy(nc: *natsConnection) void;
pub extern fn natsConnection_Publish(nc: *natsConnection, subj: [*c]const u8, data: ?*const anyopaque, dataLen: c_int) natsStatus;
pub extern fn natsConnection_Flush(nc: *natsConnection) natsStatus;
pub extern fn natsConnection_GetConnectedUrl(nc: *natsConnection, buffer: [*c]u8, bufferSize: usize) natsStatus;
