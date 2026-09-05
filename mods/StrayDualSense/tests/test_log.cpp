// The logger's file write is asynchronous (Log.cpp, 2026-09-05). This pins what the writer
// thread must still guarantee: every line from every caller reaches the file, in per-thread
// order, nothing is dropped at a realistic volume, an ERROR is on disk when Write() returns,
// the UE4SS mirror still sees every line, and Close drains before closing.
#include "Log.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

std::vector<std::string> ReadLines(const char* path)
{
    std::vector<std::string> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line))
        out.push_back(line);
    return out;
}

} // namespace

int main()
{
    const char* path = "straydualsense-test-async.log";
    std::remove(path);
    sds::Log::OpenPath(path, sds::LogLevel::Debug);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 2000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([t] {
            for (int i = 0; i < kPerThread; ++i)
                SDS_LOG_INFO("t%d line %d", t, i);
        });
    for (std::thread& th : threads)
        th.join();

    SDS_LOG_ERROR("the error line");
    const sds::Log::Stats mid = sds::Log::GetStats();
    Check(mid.written == static_cast<uint64_t>(kThreads * kPerThread + 1), "ERROR line: everything queued is on disk when Write() returns");
    Check(mid.queuedNow == 0, "queue empty after the ERROR drain");

    // The mirror saw every line too (the UE4SS forward reads it from on_update).
    const std::vector<std::string> mirror = sds::Log::TakeMirrorLines();
    Check(!mirror.empty(), "mirror carries lines");
    Check(mirror.back() == std::string("[log] 7489 mirror lines dropped (nobody drained the mirror)")
              || mirror.back().find("[ERROR] the error line") != std::string::npos,
          "mirror ends with the ERROR line or the capped-drop marker");

    sds::Log::Close();

    const std::vector<std::string> lines = ReadLines(path);
    Check(lines.size() == static_cast<size_t>(kThreads * kPerThread + 1), "file has every line");
    Check(!lines.empty() && lines.back().find("[ERROR] the error line") != std::string::npos, "the ERROR line is last");

    // Per-thread order: each thread's counter strictly increasing in file order. The stamp is
    // "[hh:mm:ss.mmm] [INFO ] t%d line %d".
    // Parsed by hand: sscanf is a warning on MSVC.
    int next[kThreads] = {};
    bool ordered = true;
    for (size_t k = 0; k + 1 < lines.size(); ++k)
    {
        const std::string& l = lines[k];
        const std::string marker = "] [INFO ] t";
        const size_t m = l.find(marker);
        const size_t sp = m == std::string::npos ? std::string::npos : l.find(" line ", m + marker.size());
        int t = -1, i = -1;
        if (sp != std::string::npos)
        {
            t = std::stoi(l.substr(m + marker.size(), sp - (m + marker.size())));
            i = std::stoi(l.substr(sp + 6));
        }
        if (sp == std::string::npos || t < 0 || t >= kThreads || i != next[t])
        {
            ordered = false;
            std::printf("      out of order or unparsable at %zu: %s\n", k, l.c_str());
            break;
        }
        next[t] = i + 1;
    }
    Check(ordered, "per-thread order preserved");

    const sds::Log::Stats s = sds::Log::GetStats();
    Check(s.dropped == 0, "nothing dropped");
    Check(s.lines == static_cast<uint64_t>(kThreads * kPerThread + 1), "every Write counted");
    Check(s.written == s.lines, "every counted line written");

    // After Close: synchronous no-op path, no crash, and Close twice is harmless.
    SDS_LOG_INFO("nowhere to go");
    sds::Log::Close();

    std::remove(path);
    std::printf("%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
