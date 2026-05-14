//! Shim that re-exports libxev's epoll backend as if it were the default `xev`.
//! Used when `-Dforce-epoll=true` is set in build.zig, so the resulting binary
//! never calls io_uring syscalls (useful when the container's seccomp profile
//! blocks them, e.g. Docker 29.x default profile).

const xev = @import("xev");
const Epoll = xev.Epoll;

pub const backend = Epoll.backend;
pub const Sys = Epoll.Sys;
pub const Loop = Epoll.Loop;
pub const Completion = Epoll.Completion;
pub const Result = Epoll.Result;
pub const ReadBuffer = Epoll.ReadBuffer;
pub const WriteBuffer = Epoll.WriteBuffer;
pub const Options = Epoll.Options;
pub const RunMode = Epoll.RunMode;
pub const Callback = Epoll.Callback;
pub const CallbackAction = Epoll.CallbackAction;
pub const CompletionState = Epoll.CompletionState;
pub const AcceptError = Epoll.AcceptError;
pub const CancelError = Epoll.CancelError;
pub const CloseError = Epoll.CloseError;
pub const ConnectError = Epoll.ConnectError;
pub const ShutdownError = Epoll.ShutdownError;
pub const WriteError = Epoll.WriteError;
pub const ReadError = Epoll.ReadError;
pub const PollError = Epoll.PollError;
pub const PollEvent = Epoll.PollEvent;
pub const WriteQueue = Epoll.WriteQueue;
pub const WriteRequest = Epoll.WriteRequest;
pub const Async = Epoll.Async;
pub const File = Epoll.File;
pub const Process = Epoll.Process;
pub const Stream = Epoll.Stream;
pub const Timer = Epoll.Timer;
pub const TCP = Epoll.TCP;
pub const UDP = Epoll.UDP;
