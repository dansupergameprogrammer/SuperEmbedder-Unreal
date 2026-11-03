#include "support.h"

#include "SuperEmbedderSha256.h"

#include <cstdio>
#include <sys/stat.h>

namespace HostSupport
{
	void FStdThreads::Launch(const char* /*Name*/, std::function<void()> Body)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		Threads.emplace_back(static_cast<std::function<void()>&&>(Body));
	}

	void FStdThreads::JoinAll()
	{
		std::vector<std::thread> ToJoin;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			ToJoin.swap(Threads);
		}
		for (std::thread& T : ToJoin)
		{
			T.join();
		}
	}

	bool FStdFiles::Exists(const std::string& Path) const
	{
		struct stat St;
		return stat(Path.c_str(), &St) == 0 && S_ISREG(St.st_mode);
	}

	bool FStdFiles::Read(const std::string& Path, uint64_t MaxBytes, std::vector<uint8_t>& Out) const
	{
		Out.clear();
		std::FILE* F = std::fopen(Path.c_str(), "rb");
		if (F == nullptr)
		{
			return false;
		}
		uint8_t Buffer[65536];
		for (;;)
		{
			size_t Want = sizeof(Buffer);
			if (MaxBytes > 0 && MaxBytes - Out.size() < Want)
			{
				Want = static_cast<size_t>(MaxBytes - Out.size());
			}
			if (Want == 0)
			{
				break;
			}
			const size_t Got = std::fread(Buffer, 1, Want, F);
			Out.insert(Out.end(), Buffer, Buffer + Got);
			if (Got < Want)
			{
				break;
			}
		}
		const bool bOk = std::ferror(F) == 0;
		std::fclose(F);
		return bOk;
	}

	void FFakeState::CloseOpenGate()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		bOpenGateClosed = true;
	}
	void FFakeState::OpenOpenGate()
	{
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			bOpenGateClosed = false;
		}
		Changed.notify_all();
	}
	void FFakeState::CloseEncodeGate()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		bEncodeGateClosed = true;
	}
	void FFakeState::OpenEncodeGate()
	{
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			bEncodeGateClosed = false;
		}
		Changed.notify_all();
	}
	void FFakeState::SetHoldDocuments(bool bHold)
	{
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			bHoldDocuments = bHold;
		}
		Changed.notify_all();
	}
	bool FFakeState::WaitEncodesStarted(int N, int TimeoutMs)
	{
		std::unique_lock<std::mutex> Lock(Mutex);
		return Changed.wait_for(Lock, std::chrono::milliseconds(TimeoutMs), [&]() { return EncodesStarted >= N; });
	}
	bool FFakeState::WaitOpensStarted(int N, int TimeoutMs)
	{
		std::unique_lock<std::mutex> Lock(Mutex);
		return Changed.wait_for(Lock, std::chrono::milliseconds(TimeoutMs), [&]() { return OpensStarted >= N; });
	}
	std::vector<std::string> FFakeState::LogCopy()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return Log;
	}

	FFingerprint FakeFingerprint(uint8_t Seed)
	{
		FFingerprint F{};
		for (size_t I = 0; I < F.size(); ++I)
		{
			F[I] = static_cast<uint8_t>(Seed * 31u + I);
		}
		return F;
	}

	std::vector<float> FakeVector(EKind Kind, const std::string& Text, uint32_t Dims)
	{
		std::string Pre(1, Kind == EKind::Query ? 'Q' : 'D');
		Pre += Text;
		std::vector<float> V(Dims);
		FFingerprint H = Sha256Of(Pre.data(), Pre.size());
		for (uint32_t I = 0; I < Dims; ++I)
		{
			if (I % 32 == 0 && I > 0)
			{
				H = Sha256Of(H.data(), H.size());
			}
			V[I] = (static_cast<float>(H[I % 32]) - 127.5f) / 128.0f;
		}
		return V;
	}

	namespace
	{
		class FFakeEncoder final : public IEncoder
		{
		public:
			FFakeEncoder(std::shared_ptr<FFakeState> InState)
				: State(static_cast<std::shared_ptr<FFakeState>&&>(InState)), Fp(FakeFingerprint(State->FingerprintSeed))
			{
				++State->LiveEncoders;
			}
			~FFakeEncoder() override
			{
				std::lock_guard<std::mutex> Lock(State->Mutex);
				State->LastCloseThread = std::this_thread::get_id();
				--State->LiveEncoders;
			}
			uint32_t Dims() const override { return State->Dims; }
			const FFingerprint& Fingerprint() const override { return Fp; }
			EStatus Encode(EKind Kind, const std::string& Utf8, std::vector<float>& Out, std::string& /*Lib*/) override
			{
				{
					std::unique_lock<std::mutex> Lock(State->Mutex);
					State->Log.push_back(std::string(Kind == EKind::Query ? "Q:" : "D:") + Utf8);
					++State->EncodesStarted;
					State->Changed.notify_all();
					State->Changed.wait(Lock, [&]() {
						return !State->bEncodeGateClosed && !(Kind == EKind::Document && State->bHoldDocuments);
					});
				}
				if (State->OnEncode)
				{
					State->OnEncode();
				}
				if (State->EncodeDelayMs > 0)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(State->EncodeDelayMs));
				}
				++State->Encodes;
				Out = FakeVector(Kind, Utf8, State->Dims);
				return EStatus::Ok;
			}
			EStatus CountQueryBodyTokens(const std::string& Utf8, int64_t& OutTokens, std::string& /*Lib*/) override
			{
				auto It = State->TokenOverrides.find(Utf8);
				OutTokens = It != State->TokenOverrides.end() ? It->second : static_cast<int64_t>(Utf8.size());
				return EStatus::Ok;
			}

		private:
			std::shared_ptr<FFakeState> State;
			FFingerprint Fp;
		};
	}

	FOpenResult FFakeFactory::Open(const FModelSource& /*Source*/, uint32_t /*MaxConcurrency*/)
	{
		{
			std::unique_lock<std::mutex> Lock(State->Mutex);
			++State->OpensStarted;
			State->Changed.notify_all();
			State->Changed.wait(Lock, [&]() { return !State->bOpenGateClosed; });
		}
		++State->Opens;
		FOpenResult R;
		R.Status = State->OpenStatus;
		if (R.Status == EStatus::Ok)
		{
			R.Encoder.reset(new FFakeEncoder(State));
		}
		else
		{
			R.Detail = "SEMB_ARTIFACT_UNREADABLE";
		}
		return R;
	}

	FModelSource FakeSource(const std::string& Name)
	{
		FModelSource S;
		S.ModelPath = Name + ".sslm";
		S.TokenizerPath = Name + "-tokenizer.sslm";
		return S;
	}

	std::function<bool()> MainThreadPredicate()
	{
		const std::thread::id Main = std::this_thread::get_id();
		return [Main]() { return std::this_thread::get_id() == Main; };
	}

	FHarnessService MakeFakeService(const FServiceConfig& Config, bool bOpen)
	{
		FHarnessService H;
		H.Fake = std::make_shared<FFakeState>();
		H.Service.reset(new FEncodeService(Config, std::make_shared<FFakeFactory>(H.Fake), std::make_shared<FStdThreads>(),
			MainThreadPredicate()));
		std::string Detail;
		if (H.Service->Start(Detail) != EStatus::Ok)
		{
			HostTest::Fail(__FILE__, __LINE__, "Start failed: " + Detail);
			return H;
		}
		if (bOpen)
		{
			if (H.Service->Open(FakeSource("model-a"), H.Handle) != EStatus::Ok ||
				!WaitUntil([&]() { return H.Service->GetState() == EEncoderState::Ready; }))
			{
				HostTest::Fail(__FILE__, __LINE__, "fake open did not reach Ready");
			}
		}
		return H;
	}

	FModelSource PairSource()
	{
		FModelSource S;
		S.ModelPath = HostTest::PairDir() + "/u1_pair_model.sslm";
		S.TokenizerPath = HostTest::PairDir() + "/u1_pair_tokenizer.sslm";
		return S;
	}

	std::unique_ptr<FEncodeService> MakeRealService(const FServiceConfig& Config, uint64_t& OutHandle)
	{
		if (HostTest::PairDir().empty())
		{
			HostTest::Fail(__FILE__, __LINE__, "SEMB_UE_HOST_PAIR_MISSING: pass --pair-dir (the U1 synthetic pair)");
			return nullptr;
		}
		std::unique_ptr<FEncodeService> S(new FEncodeService(Config,
			std::make_shared<FSembEncoderFactory>(std::make_shared<FStdFiles>()), std::make_shared<FStdThreads>(),
			MainThreadPredicate()));
		std::string Detail;
		if (S->Start(Detail) != EStatus::Ok || S->Open(PairSource(), OutHandle) != EStatus::Ok)
		{
			HostTest::Fail(__FILE__, __LINE__, "real service did not start: " + Detail);
			return nullptr;
		}
		WaitUntil([&]() { return S->GetState() != EEncoderState::Opening; }, 60000);
		if (S->GetState() != EEncoderState::Ready)
		{
			std::string OpenDetail;
			const EStatus St = S->GetOpenStatus(&OpenDetail);
			HostTest::Fail(__FILE__, __LINE__, std::string("real open failed: ") + StatusName(St) + " " + OpenDetail);
			return nullptr;
		}
		return S;
	}

	bool WaitUntil(const std::function<bool()>& Pred, int TimeoutMs)
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMs);
		while (!Pred())
		{
			if (std::chrono::steady_clock::now() > Deadline)
			{
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return true;
	}

	bool WaitDelivery(FEncodeService& S, FTicket Ticket, FDelivery& Out, int TimeoutMs, std::vector<FDelivery>* Others)
	{
		bool bFound = false;
		WaitUntil([&]() {
			for (FDelivery& D : S.Drain())
			{
				if (D.Ticket == Ticket && !bFound)
				{
					Out = static_cast<FDelivery&&>(D);
					bFound = true;
				}
				else if (Others != nullptr)
				{
					Others->push_back(static_cast<FDelivery&&>(D));
				}
			}
			return bFound;
		}, TimeoutMs);
		return bFound;
	}

	std::vector<FDelivery> DrainCount(FEncodeService& S, size_t N, int TimeoutMs)
	{
		std::vector<FDelivery> All;
		WaitUntil([&]() {
			for (FDelivery& D : S.Drain())
			{
				All.push_back(static_cast<FDelivery&&>(D));
			}
			return All.size() >= N;
		}, TimeoutMs);
		return All;
	}

	FEmbedding CheckedVector(std::vector<float> Values, uint8_t Seed)
	{
		return FEmbedding::Checked(static_cast<std::vector<float>&&>(Values), FakeFingerprint(Seed), EKind::Document);
	}

	std::vector<uint8_t> ReadFile(const std::string& Path)
	{
		std::vector<uint8_t> Out;
		FStdFiles().Read(Path, 0, Out);
		return Out;
	}
}
