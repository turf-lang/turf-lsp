#pragma once

#include <string>

// Process-wide diagnostic logger. LSP transports the protocol itself over
// stdout, so nothing besides framed JSON-RPC messages may ever be written
// there -- a single stray std::cout from a debug print corrupts the stream
// for the rest of the session (the client's Content-Length framing gets out
// of sync and every subsequent message is garbage). Everything here goes to
// stderr and, if TURF_LSP_LOG names a writable path, also to that file --
// gives an editor-launched server (whose stderr often isn't visible without
// the client's own log viewer) somewhere durable to look.
namespace Logger {

enum class Level { Error, Warn, Info, Debug };

// Reads TURF_LSP_LOG (file path) and TURF_LSP_LOG_LEVEL
// (error|warn|info|debug, default info) once. Safe to call more than once;
// only the first call has effect.
void init();

void log(Level level, const std::string& message);

inline void error(const std::string& message) { log(Level::Error, message); }
inline void warn(const std::string& message) { log(Level::Warn, message); }
inline void info(const std::string& message) { log(Level::Info, message); }
inline void debug(const std::string& message) { log(Level::Debug, message); }

} // namespace Logger
