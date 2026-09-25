// Line-oriented diagnostic log for the component.
//
// Deliberately free of SDK/PFC dependencies so it can be reused by the offline
// harness and so that it is safe to call from any thread the core queries us on.
//
// Path: %JOC_LOG% if set, otherwise <directory of this DLL>\joc_decoder.log.
// The file is truncated once per process, line-buffered, and every line is
// flushed so the log can be read while foobar2000 is still running.
#pragma once

namespace joc_log {

// Opens (truncating) the log.  Called lazily by line(); call it explicitly to
// get the banner before anything else in the process logs.
void open();

// Absolute path of the log file, or "" when it could not be opened.
const char* path();

// printf-style, thread safe, appends one line.
void line(const char* fmt, ...);

}  // namespace joc_log
