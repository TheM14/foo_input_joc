// Line-oriented diagnostic log for the component.
//
// Deliberately free of SDK/PFC dependencies so it can be reused by the offline
// harness and so that it is safe to call from any thread the core queries us on.
//
// Path: %JOC_LOG% if set, otherwise <directory of this DLL>\joc_decoder.log.
// The file is truncated once per process, unbuffered, and every line is written
// straight out so the log can be read while foobar2000 is still running.
//
// Size: what is bounded is bytes, not lines.  A line is anywhere between 40 and
// 2048 bytes, so a cap on the number of lines leaves the size of the file
// unpredictable by a factor of fifty.  A file that reaches budget_bytes() is
// moved to <path>.1 -- replacing any previous roll, so there is never a chain of
// them -- and a fresh one is started.  Two files of the budget is therefore the
// most the component ever holds, and the newest lines are always the ones in
// <path>.  Lines written between header_begin() and header_end() are written
// again at the top of every window, so a rolled log still says which build
// produced it.
#pragma once

#include <cstdarg>

namespace joc_log {

// Opens (truncating) the log.  Called lazily by line(); call it explicitly to
// get the banner before anything else in the process logs.
void open();

// Absolute path of the log file, or "" when it could not be opened.
const char* path();

// Bytes one file holds before it is rolled to <path>.1.
unsigned long long budget_bytes();

// printf-style, thread safe, appends one line.
void line(const char* fmt, ...);

// The va_list form, for a wrapper that forwards its own arguments.
void line_v(const char* fmt, va_list args);

// Remembers the lines written in between and writes them again after every roll.
// Meant for the version banner: a handful of lines, written once.
void header_begin();
void header_end();

}  // namespace joc_log
