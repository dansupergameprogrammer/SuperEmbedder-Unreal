#pragma once

// Host-suite support: std::thread workers, a std::FILE reader, a fake encoder with gates and an encode log,
// and helpers that pump a service's outbox the way the subsystem's tick does.

#include "harness.h"

#include "SuperEmbedderCoreTypes.h"
#include "SuperEmbedderEncodeService.h"
#include "SuperEmbedderEncoder.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace SuperEmbedderUe
{
	inline std::string HostTestName(EStatus S) { return StatusName(S); }
	inline std::string HostTestName(EOutcome O)
	{
		return O == EOutcome::Completed ? "Completed" : O == EOutcome::Refused ? "Refused" : "Cancelled";
	}
	inline std::string HostTestName(EEncoderState S) { return EncoderStateName(S); }
}

namespace HostSupport
{
	using namespace SuperEmbedderUe;

	class FStdThreads final : public IWorkerThreads
	{
	public:
		~FStdThreads() override { JoinAll(); }
		void Launch(const char* Name, std::function<void()> Body) override;
		void JoinAll() override;

	private:
		std::mutex Mutex;
		std::vector<std::thread> Threads;
	};

	class FStdFiles final : public IFileReader
	{
	public:
		bool Exists(const std::string& Path) const override;
		bool Read(const std::string& Path, uint64_t MaxBytes, std::vector<uint8_t>& Out) const override;
	};

	// The fake encoder's behaviour, shared by every encoder its factory opens.
	struct FFakeState
	{
		uint32_t Dims = 8;
		uint8_t FingerprintSeed = 1;
		EStatus OpenStatus = EStatus::Ok;
		int EncodeDelayMs = 0;
		std::map<std::string, int64_t> TokenOverrides; // text -> count; otherwise one token per byte

		std::mutex Mutex;
		std::condition_variable Changed;
		bool bOpenGateClosed = false;
		bool bEncodeGateClosed = false;
		bool bHoldDocuments = false; // document encodes wait while set; queries pass
		int EncodesStarted = 0;
		int OpensStarted = 0;
		std::vector<std::string> Log; // "Q:text" or "D:text", in the order the encodes started
		std::atomic<int> Encodes{0};
		std::atomic<int> Opens{0};
		std::atomic<int> LiveEncoders{0};
		std::thread::id LastCloseThread;
		std::function<void()> OnEncode; // runs inside an encode, on the worker

		void CloseOpenGate();
		void OpenOpenGate();
		void CloseEncodeGate();
		void OpenEncodeGate();
		void SetHoldDocuments(bool bHold);
		bool WaitEncodesStarted(int N, int TimeoutMs = 5000);
		bool WaitOpensStarted(int N, int TimeoutMs = 5000);
		std::vector<std::string> LogCopy();
	};

	FFingerprint FakeFingerprint(uint8_t Seed);
	// The fake's vector for a text: a pure function of (kind, text, dims), never zero.
	std::vector<float> FakeVector(EKind Kind, const std::string& Text, uint32_t Dims);

	class FFakeFactory final : public IEncoderFactory
	{
	public:
		explicit FFakeFactory(std::shared_ptr<FFakeState> InState) : State(static_cast<std::shared_ptr<FFakeState>&&>(InState)) {}
		FOpenResult Open(const FModelSource& Source, uint32_t MaxConcurrency) override;
		std::shared_ptr<FFakeState> State;
	};

	FModelSource FakeSource(const std::string& Name);

	// The test's main thread stands in for the game thread.
	std::function<bool()> MainThreadPredicate();

	struct FHarnessService
	{
		std::shared_ptr<FFakeState> Fake;
		std::unique_ptr<FEncodeService> Service;
		uint64_t Handle = 0;
	};
	// A started service over the fake, opened with FakeSource("model-a") and waited to Ready unless bOpen is false.
	FHarnessService MakeFakeService(const FServiceConfig& Config, bool bOpen = true);

	// A started service over the real library and the U1 pair, opened and waited to Ready. Null (with a named
	// failure recorded) when the pair is missing or the open fails.
	std::unique_ptr<FEncodeService> MakeRealService(const FServiceConfig& Config, uint64_t& OutHandle);
	FModelSource PairSource();

	bool WaitUntil(const std::function<bool()>& Pred, int TimeoutMs = 10000);
	// Drains (as the subsystem's tick does) until Ticket is delivered; the other deliveries go to Others.
	bool WaitDelivery(FEncodeService& S, FTicket Ticket, FDelivery& Out, int TimeoutMs = 20000,
		std::vector<FDelivery>* Others = nullptr);
	// Drains until N deliveries have arrived in total.
	std::vector<FDelivery> DrainCount(FEncodeService& S, size_t N, int TimeoutMs = 20000);

	FEmbedding CheckedVector(std::vector<float> Values, uint8_t Seed = 1);
	std::vector<uint8_t> ReadFile(const std::string& Path);
}
