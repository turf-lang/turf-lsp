#include "Logger.hpp"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace {

std::mutex g_logMutex;
bool g_initialized = false;
std::ofstream g_logFile;
Logger::Level g_minLevel = Logger::Level::Info;

const char* levelName(Logger::Level level) {
    switch (level) {
        case Logger::Level::Error: return "ERROR";
        case Logger::Level::Warn:  return "WARN";
        case Logger::Level::Info:  return "INFO";
        case Logger::Level::Debug: return "DEBUG";
    }
    return "?";
}

Logger::Level parseLevel(const std::string& s) {
    if (s == "error") return Logger::Level::Error;
    if (s == "warn") return Logger::Level::Warn;
    if (s == "debug") return Logger::Level::Debug;
    return Logger::Level::Info;
}

std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&t), "%H:%M:%S");
    return oss.str();
}

} // namespace

namespace Logger {

void init() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_initialized) return;
    g_initialized = true;

    if (const char* levelEnv = std::getenv("TURF_LSP_LOG_LEVEL")) {
        g_minLevel = parseLevel(levelEnv);
    }
    if (const char* pathEnv = std::getenv("TURF_LSP_LOG")) {
        g_logFile.open(pathEnv, std::ios::out | std::ios::app);
        if (!g_logFile.is_open()) {
            std::cerr << "[turf-lsp] could not open TURF_LSP_LOG path: " << pathEnv << "\n";
        }
    }
}

void log(Level level, const std::string& message) {
    // init() is idempotent and cheap after the first call; calling it here
    // means every log site works even if a caller forgets the explicit
    // init() at startup (main.cpp still calls it once, early, so the level/
    // file are settled before the first real log line).
    init();

    if (static_cast<int>(level) > static_cast<int>(g_minLevel)) return;

    std::lock_guard<std::mutex> lock(g_logMutex);
    std::string line = "[" + timestamp() + "] [" + levelName(level) + "] " + message;
    std::cerr << line << "\n";
    if (g_logFile.is_open()) {
        g_logFile << line << std::endl;
    }
}

} // namespace Logger
