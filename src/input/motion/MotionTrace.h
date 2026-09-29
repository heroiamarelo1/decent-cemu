#pragma once
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <chrono>
#include <initializer_list>

// Opt-in, append-only capture. Set DECENT_MOTION_TRACE to a path prefix.
// Each record kind uses its own file; output is flushed so a crash keeps evidence.
namespace motion_trace
{
inline std::mutex mutex;
template<class... Args>
inline void write(const char* kind, const char* format, Args... args)
{
    const char* prefix = std::getenv("DECENT_MOTION_TRACE");
    if (!prefix || !*prefix) return;
    std::lock_guard lock(mutex);
    const std::string path = std::string(prefix) + "-" + kind + ".csv";
    if (FILE* file = std::fopen(path.c_str(), "a"))
    {
        std::fprintf(file, format, args...);
        std::fputc('\n', file);
        std::fclose(file);
    }
}
}

// V2 rows: PC monotonic microseconds, channel, PPC PC, PPC LR, values.
// All kinds share this clock so API calls can be aligned with sensor arrivals.
namespace motion_trace
{
inline void values(const char* kind, unsigned channel, unsigned pc, unsigned lr,
                   std::initializer_list<double> fields)
{
    const char* prefix = std::getenv("DECENT_MOTION_TRACE");
    if (!prefix || !*prefix) return;
    const auto time = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard lock(mutex);
    const std::string path = std::string(prefix) + "-" + kind + "-v2.csv";
    if (FILE* file = std::fopen(path.c_str(), "a"))
    {
        std::fprintf(file, "%llu,%u,%08x,%08x", static_cast<unsigned long long>(time), channel, pc, lr);
        for (const double value : fields) std::fprintf(file, ",%.12g", value);
        std::fputc('\n', file);
        std::fclose(file);
    }
}
}
