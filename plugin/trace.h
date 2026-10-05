#pragma once
// Device diagnostics: every value MPC sets and what the plugin made of it, appended to
// <dir>/polyforce.log while <dir>/polyforce.trace exists (dir = /tmp, or PF_TRACE_DIR). Create
// the flag file to start and remove it to stop, with MPC running: it is looked for at most once
// a second. The log stops growing at 2 MB. File I/O: never from the audio thread.

namespace pf {

bool tracing();
void trace(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace pf
