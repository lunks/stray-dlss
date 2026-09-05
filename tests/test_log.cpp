#include <doctest/doctest.h>

#include "log.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace stray_dlss;

// The logger's file write is asynchronous (log.cpp, 2026-09-05): the user correlated the
// on-screen flicker with the log, line for line, while the present thread was doing fputs +
// fflush for every line. These tests pin what the asynchronous writer must still guarantee:
// every line reaches the file, in per-thread order, nothing is dropped at a realistic volume,
// and shutdown drains before closing.

namespace {

std::vector<std::string> read_lines(const char *path)
{
	std::vector<std::string> out;
	std::ifstream in(path);
	std::string line;
	while (std::getline(in, line))
		out.push_back(line);
	return out;
}

} // namespace

TEST_CASE("async logger: every line from every thread reaches the file, per-thread order kept")
{
	const char *path = "stray-dlss-test-async.log";
	std::remove(path);
	// Other test cases in this binary log too, so every counter is judged as a delta.
	const log::Stats before = log::stats();
	log::init_file_sink_path(path);

	constexpr int kThreads = 4;
	constexpr int kPerThread = 2500;
	std::vector<std::thread> threads;
	for (int t = 0; t < kThreads; ++t)
		threads.emplace_back([t] {
			for (int i = 0; i < kPerThread; ++i)
				STRAY_LOG_INFO("t%d line %d", t, i);
		});
	for (std::thread &th : threads)
		th.join();

	// One ERROR: it must be on disk when write() returns, not merely queued.
	STRAY_LOG_ERROR("the error line");
	const log::Stats mid = log::stats();
	CHECK(mid.written - before.written == static_cast<unsigned long long>(kThreads * kPerThread + 1));
	CHECK(mid.queued_now == 0);

	log::shutdown_file_sink();

	const std::vector<std::string> lines = read_lines(path);
	REQUIRE(lines.size() == static_cast<std::size_t>(kThreads * kPerThread + 1));
	CHECK(lines.back() == "[ERROR] the error line");

	// Per-thread order: each thread's counter must be strictly increasing in file order.
	// Line shape: "[INFO ] t<thread> line <i>" (parsed by hand: sscanf is a warning on MSVC).
	int next[kThreads] = {};
	for (std::size_t k = 0; k + 1 < lines.size(); ++k)
	{
		const std::string &l = lines[k];
		const std::string prefix = "[INFO ] t";
		REQUIRE(l.compare(0, prefix.size(), prefix) == 0);
		const std::size_t sp = l.find(" line ", prefix.size());
		REQUIRE(sp != std::string::npos);
		const int t = std::stoi(l.substr(prefix.size(), sp - prefix.size()));
		const int i = std::stoi(l.substr(sp + 6));
		REQUIRE(t >= 0);
		REQUIRE(t < kThreads);
		CHECK(i == next[t]);
		next[t] = i + 1;
	}

	const log::Stats s = log::stats();
	CHECK(s.dropped == before.dropped);
	CHECK(s.lines - before.lines == static_cast<unsigned long long>(kThreads * kPerThread + 1));
	CHECK(s.written - before.written == s.lines - before.lines);
	std::remove(path);
}

TEST_CASE("log threshold: off refuses everything before formatting, errors included; levels nest")
{
	const log::Stats before = log::stats();
	log::set_threshold(log::Threshold::off);
	CHECK_FALSE(log::enabled(log::Level::error));
	STRAY_LOG_ERROR("must not count");
	STRAY_LOG_INFO("must not count");
	CHECK(log::stats().lines == before.lines);

	log::set_threshold(log::Threshold::warning);
	CHECK(log::enabled(log::Level::error));
	CHECK(log::enabled(log::Level::warning));
	CHECK_FALSE(log::enabled(log::Level::info));
	CHECK_FALSE(log::enabled(log::Level::debug));
	STRAY_LOG_INFO("refused");
	STRAY_LOG_WARN("accepted");
	CHECK(log::stats().lines == before.lines + 1);

	log::set_threshold(log::Threshold::debug); // the default, restored for the other cases
	CHECK(log::threshold() == log::Threshold::debug);
	CHECK(log::enabled(log::Level::debug));
}

TEST_CASE("async logger: writes before init and after shutdown do not crash and are not lost to a queue")
{
	// Before init there is no file and no writer: the call is a no-op that still counts.
	const log::Stats before = log::stats();
	STRAY_LOG_INFO("nowhere to go");
	CHECK(log::stats().lines == before.lines + 1);
	CHECK(log::stats().dropped == before.dropped);
	// Shutting down twice is harmless.
	log::shutdown_file_sink();
	log::shutdown_file_sink();
}
