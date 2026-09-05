#include "log.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace stray_dlss::log {
namespace {

// THE WRITER IS ASYNCHRONOUS (2026-09-05). Until then every line was fprintf + fflush on the
// CALLING thread under one mutex - the present thread, the RHI threads - and the user
// correlated the on-screen flicker with the log, line for line: a small blink when the 600-frame
// status block (a dozen ~1 KB lines) went out, a large one on the [stall] line. The stall
// detector's own buckets summed to ~2.6 ms of a 40 ms frame; the write was the unmeasured rest.
// Now a caller formats the line and pushes it onto a bounded queue; one writer thread drains the
// queue in batches and writes and flushes once per batch. What the file sink promised still
// holds: lines are complete, in enqueue order, and an ERROR line is on disk before its caller
// continues (a bounded wait for the drain), because the interesting failures are the ones that
// crash next. Before init_file_sink and after shutdown the write is synchronous, as before.
//
// No std::thread / condition_variable at static-init: they live in a heap block created by
// init_file_sink, the same caution as the CMake note on std::mutex's constexpr constructor.
struct Writer
{
	std::condition_variable cv;
	std::condition_variable drained;
	std::thread thread;
	std::vector<std::string> queue;
	bool stop = false;
	bool draining = false;
};

std::mutex g_mutex;
std::FILE *g_file = nullptr;
ExternalSink g_external = nullptr;
Writer *g_writer = nullptr;
Stats g_stats = {};
constexpr std::size_t kMaxQueue = 8192;
constexpr auto kErrorDrainTimeout = std::chrono::milliseconds(200);

const char *level_tag(Level level)
{
	switch (level)
	{
	case Level::error:   return "ERROR";
	case Level::warning: return "WARN ";
	case Level::info:    return "INFO ";
	case Level::debug:   return "DEBUG";
	}
	return "?????";
}

Level level_of(const std::string &line)
{
	if (line.compare(0, 7, "[ERROR]") == 0) return Level::error;
	if (line.compare(0, 7, "[WARN ]") == 0) return Level::warning;
	if (line.compare(0, 7, "[DEBUG]") == 0) return Level::debug;
	return Level::info;
}

std::uint64_t now_ns()
{
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Writes one batch. Called with the mutex NOT held (the batch was swapped out under it).
void write_batch(const std::vector<std::string> &batch, std::FILE *file, ExternalSink external)
{
	for (const std::string &line : batch)
	{
		if (file != nullptr)
			std::fputs(line.c_str(), file);
		if (external != nullptr)
		{
			// The external sink takes (level, message) without the tag and newline we added.
			std::string msg = line.size() > 8 ? line.substr(8) : std::string();
			if (!msg.empty() && msg.back() == '\n')
				msg.pop_back();
			external(level_of(line), msg.c_str());
		}
	}
	if (file != nullptr)
		std::fflush(file);
}

void writer_main(Writer *w)
{
	std::vector<std::string> batch;
	for (;;)
	{
		std::FILE *file;
		ExternalSink external;
		{
			std::unique_lock<std::mutex> lock(g_mutex);
			w->cv.wait(lock, [&] { return w->stop || !w->queue.empty(); });
			if (w->stop && w->queue.empty())
				return;
			batch.swap(w->queue);
			w->queue.clear();
			w->draining = true;
			file = g_file;
			external = g_external;
		}
		const std::uint64_t t0 = now_ns();
		write_batch(batch, file, external);
		const std::uint64_t dt = now_ns() - t0;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_stats.written += batch.size();
			++g_stats.writer_batches;
			g_stats.writer_ns_sum += dt;
			if (dt > g_stats.writer_ns_max)
				g_stats.writer_ns_max = dt;
			if (batch.size() > g_stats.batch_max)
				g_stats.batch_max = batch.size();
			w->draining = false;
		}
		batch.clear();
		w->drained.notify_all();
	}
}

// Both called with g_mutex held.
void start_writer_locked()
{
	if (g_writer != nullptr)
		return;
	g_writer = new Writer();
	g_writer->thread = std::thread(writer_main, g_writer);
}

void open_locked(std::FILE *file)
{
	if (file == nullptr)
		return;
	g_file = file;
	start_writer_locked();
}

} // namespace

void init_file_sink()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_file != nullptr)
		return;

	// Deliberately a fixed name next to the game executable's working directory rather than
	// anything clever: the user has to be able to find and paste it.
	std::FILE *f = nullptr;
#ifdef _WIN32
	if (fopen_s(&f, "stray-dlss.log", "w") != 0)
		f = nullptr;
#else
	f = std::fopen("stray-dlss.log", "w");
#endif
	open_locked(f);
}

void init_file_sink_path(const char *path)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_file != nullptr || path == nullptr)
		return;
	std::FILE *f = nullptr;
#ifdef _WIN32
	if (fopen_s(&f, path, "w") != 0)
		f = nullptr;
#else
	f = std::fopen(path, "w");
#endif
	open_locked(f);
}

#ifdef _WIN32
void init_file_sink(const wchar_t *path)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_file != nullptr || path == nullptr)
		return;
	std::FILE *f = nullptr;
	if (_wfopen_s(&f, path, L"w") != 0)
		f = nullptr;
	open_locked(f);
}
#endif

void shutdown_file_sink()
{
	// The plugin's Stop() runs from the mod destructor at process teardown, which may be under
	// the DLL loader lock. A thread cannot be JOINED there (its exit needs the same lock), so:
	// wait for the drain on the condition variable, bounded, then detach the thread; with `stop`
	// set and the queue empty it returns on its next iteration, and the process is exiting.
	Writer *w = nullptr;
	{
		std::unique_lock<std::mutex> lock(g_mutex);
		w = g_writer;
		g_writer = nullptr; // new writes go synchronous from here
		if (w != nullptr)
		{
			w->stop = true;
			w->cv.notify_all();
			w->drained.wait_for(lock, std::chrono::milliseconds(500), [&] { return w->queue.empty() && !w->draining; });
			if (!w->queue.empty())
			{
				// The writer did not get to it in time: write the rest here, synchronously.
				std::vector<std::string> rest;
				rest.swap(w->queue);
				lock.unlock();
				write_batch(rest, g_file, g_external);
				lock.lock();
			}
		}
		if (g_file != nullptr)
		{
			std::fclose(g_file);
			g_file = nullptr;
		}
	}
	if (w != nullptr)
	{
		if (w->thread.joinable())
			w->thread.detach();
		// `w` is intentionally leaked: the detached thread may still be on its way out of
		// writer_main and touches w->cv / w->stop on the way. A few hundred bytes at exit.
	}
}

void set_external_sink(ExternalSink sink)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_external = sink;
}

Stats stats()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	Stats s = g_stats;
	s.queued_now = g_writer != nullptr ? static_cast<unsigned long long>(g_writer->queue.size()) : 0ull;
	return s;
}

void write(Level level, const char *message)
{
	const std::uint64_t t0 = now_ns();
	std::string line;
	line.reserve(std::strlen(message) + 10);
	line += '[';
	line += level_tag(level);
	line += "] ";
	line += message;
	line += '\n';

	std::unique_lock<std::mutex> lock(g_mutex);
	++g_stats.lines;
	if (g_writer == nullptr)
	{
		// No writer (before init, or after shutdown): synchronous, as it always was.
		if (g_file != nullptr)
		{
			std::fputs(line.c_str(), g_file);
			std::fflush(g_file);
		}
		if (g_external != nullptr)
			g_external(level, message);
		return;
	}
	if (g_writer->queue.size() >= kMaxQueue)
	{
		++g_stats.dropped;
		return;
	}
	g_writer->queue.push_back(std::move(line));
	Writer *w = g_writer;
	if (level == Level::error)
	{
		// Bounded wait for the drain: an ERROR is worth a stall, a lost ERROR is not.
		w->cv.notify_one();
		w->drained.wait_for(lock, kErrorDrainTimeout, [&] { return w->queue.empty() && !w->draining; });
		const std::uint64_t dt = now_ns() - t0;
		if (dt > g_stats.caller_ns_max)
			g_stats.caller_ns_max = dt;
		return;
	}
	const std::uint64_t dt = now_ns() - t0;
	if (dt > g_stats.caller_ns_max)
		g_stats.caller_ns_max = dt;
	lock.unlock();
	w->cv.notify_one();
}

void writef(Level level, const char *format, ...)
{
	char buffer[2048];

	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	write(level, buffer);
}

} // namespace stray_dlss::log
