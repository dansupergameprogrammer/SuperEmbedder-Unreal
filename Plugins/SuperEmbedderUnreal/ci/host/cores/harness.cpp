#include "harness.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace HostTest
{
	namespace
	{
		std::mutex GMutex;
		int GFailures = 0;
		std::string GFirstFailure;
		std::string GPairDir;
	}

	std::vector<FTestCase>& Registry()
	{
		static std::vector<FTestCase> Cases;
		return Cases;
	}

	void Fail(const char* File, int Line, const std::string& Message)
	{
		std::lock_guard<std::mutex> Lock(GMutex);
		if (GFailures == 0)
		{
			const char* Slash = std::strrchr(File, '/');
			GFirstFailure = std::string(Slash ? Slash + 1 : File) + ":" + std::to_string(Line) + ": " + Message;
		}
		++GFailures;
	}

	const std::string& PairDir()
	{
		return GPairDir;
	}

	int Failures()
	{
		std::lock_guard<std::mutex> Lock(GMutex);
		return GFailures;
	}
}

// Usage: superembedder_cores_host_tests [--pair-dir DIR] [substring]
//   runs the tests whose name contains substring. The pair directory holds SuperEmbedder's U1 synthetic
//   u1_pair_model.sslm and u1_pair_tokenizer.sslm; the real-encode tests fail by name without it.
int main(int Argc, char** Argv)
{
	const char* Filter = nullptr;
	if (const char* Env = std::getenv("SEMB_U1_PAIR_DIR"))
	{
		HostTest::GPairDir = Env;
	}
	for (int I = 1; I < Argc; ++I)
	{
		if (std::strcmp(Argv[I], "--pair-dir") == 0 && I + 1 < Argc)
		{
			HostTest::GPairDir = Argv[++I];
		}
		else
		{
			Filter = Argv[I];
		}
	}
	int Run = 0;
	int Passed = 0;
	int Failed = 0;
	for (const HostTest::FTestCase& Case : HostTest::Registry())
	{
		if (Filter != nullptr && std::strstr(Case.Name, Filter) == nullptr)
		{
			continue;
		}
		{
			std::lock_guard<std::mutex> Lock(HostTest::GMutex);
			HostTest::GFailures = 0;
			HostTest::GFirstFailure.clear();
		}
		Case.Fn();
		++Run;
		const int Failures = HostTest::Failures();
		if (Failures == 0)
		{
			++Passed;
			std::printf("[PASS] %s  (%s)\n", Case.Name, Case.Cells);
		}
		else
		{
			++Failed;
			std::printf("[FAIL] %s  (%s)\n       %d failure(s); first: %s\n", Case.Name, Case.Cells, Failures,
				HostTest::GFirstFailure.c_str());
		}
		std::fflush(stdout);
	}
	std::printf("\n%d tests, %d passed, %d failed\n", Run, Passed, Failed);
	return Failed == 0 && Run > 0 ? 0 : 1;
}
