#pragma once

// The encoder's lifecycle and the two encode lanes (proposal rev 9, sections 2.1 and 2.2; the draft's 5.2 and
// 5.4), with no Unreal in it. The subsystem owns one instance per process, starts its workers through
// IWorkerThreads (FRunnableThread in the engine, std::thread in the host suite), drains its outbox once per
// game-thread tick, and dispatches each delivery to the caller's delegate.
//
// Lifecycle. Closed -> Opening -> Ready | Failed; Ready -> Closing -> Closed when the last handle is released.
//   - Open() never blocks: the open (20-38 s) runs on a worker as the first unit of work.
//   - Open() with the same model while Opening or Ready joins it and returns another handle (L-2); with another
//     model it refuses DifferentModelOpen. During Closing it refuses EncoderClosing.
//   - The library cannot cancel an open. Releasing the last handle during Opening lets the open finish and then
//     closes, unless a handle joined meanwhile (G-25).
//   - Close cancels every undelivered ticket (Cancelled), waits for the encodes already running, and frees the
//     library handle on a worker, never the game thread.
//
// Tickets. Every EncodeQuery()/EncodeDocuments() returns a ticket, and exactly one of Completed, Refused or
// Cancelled is delivered for it, always through a later Drain() (never inside the issuing call), cache hits and
// refusals included (Q-1, K-2). The exceptions: CancelOwner() returns its Cancelled deliveries directly (the
// world-end path, which must deliver while the world exists), and Shutdown() delivers nothing.
//
// Lanes. Queries always go ahead of documents: a free worker takes the oldest query before any document work.
// At MaxConcurrency >= 2 worker 0 is reserved for queries (W-1); at 1 a query waits for at most the one document
// row already running (W-3). A documents ticket is validated as one unit (ids, duplicates, UTF-8, the token
// bound, before any encode), then encoded one row per unit, so a cancel, an owner's end or shutdown stops it
// between rows and nothing partial is delivered (D-3..D-6).
//
// Queue bound (G-24). At most MaxPendingQueries queries wait (not yet taken by a worker) and at most
// MaxPendingDocumentTickets documents tickets are undelivered at once; one more refuses QueueFull.
//
// Token bounds. Queries are bounded in body tokens by the encoder's own count (R17, on the worker; QueryTooLong
// names the count). Documents are bounded by the same verb, which at SuperEmbedder b52afd0 counts the text as a
// query body: there is no document-kind count yet (proposal section 6, re-audit G-22), so DocumentTooLong's
// detail says which kind was counted. Both lanes also refuse text over MaxTextBytes UTF-8 bytes.

#include "SuperEmbedderCoreTypes.h"
#include "SuperEmbedderEncodeCache.h"
#include "SuperEmbedderEncoder.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SuperEmbedderUe
{
	class IWorkerThreads
	{
	public:
		virtual ~IWorkerThreads() = default;
		// Starts a thread running Body. Called on the game thread by FEncodeService::Start().
		virtual void Launch(const char* Name, std::function<void()> Body) = 0;
		// Joins every launched thread. Called once, by Shutdown(), never from a launched thread.
		virtual void JoinAll() = 0;
	};

	struct FServiceConfig
	{
		uint32_t MaxConcurrency = 1;            // workers and library encode slots, 1-4
		uint32_t MaxQueryTokens = 256;          // body tokens, 1-512
		uint32_t MaxDocumentTokens = 256;       // 1-512
		uint32_t MaxPendingQueries = 64;        // >= 1
		uint32_t MaxPendingDocumentTickets = 8; // >= 1
		uint32_t MaxCacheEntries = 4096;        // 0 disables the cache
		uint32_t MaxTextBytes = 4096;           // the byte ceiling, per query and per row, >= 1

		// Tests only: the must-reject mutants of W-2 (no reserved query worker) and W-4 (one first-in,
		// first-out queue across both lanes). Never set by the subsystem.
		bool bTestDisableQueryReservation = false;
		bool bTestFifoLanes = false;

		EStatus Validate(std::string& Detail) const;
	};

	enum class EEncoderState : uint8_t
	{
		Closed = 0,
		Opening = 1,
		Ready = 2,
		Failed = 3,
		Closing = 4,
	};
	const char* EncoderStateName(EEncoderState State);

	using FTicket = uint64_t;
	using FOwner = uint64_t; // 0 = no owner (an editor utility, a commandlet)

	enum class EOutcome : uint8_t
	{
		Completed = 0,
		Refused = 1,
		Cancelled = 2,
	};

	struct FDocumentRow
	{
		std::string Id;
		std::string Utf8;
	};

	struct FDelivery
	{
		FTicket Ticket = 0;
		FOwner Owner = 0;
		EKind Kind = EKind::Query;
		EOutcome Outcome = EOutcome::Refused;
		EStatus Status = EStatus::Ok;
		std::string Detail;
		std::vector<FEmbedding> Vectors; // one for a query; one per row, in row order, for documents
		std::vector<std::string> Ids; // documents: the row ids, in row order
		bool bFromCache = false;      // a query served whole from the cache
	};

	class FEncodeService
	{
	public:
		FEncodeService(const FServiceConfig& InConfig, std::shared_ptr<IEncoderFactory> InFactory,
			std::shared_ptr<IWorkerThreads> InThreads, std::function<bool()> InIsGameThread);
		~FEncodeService();

		FEncodeService(const FEncodeService&) = delete;
		FEncodeService& operator=(const FEncodeService&) = delete;

		// Validates the config and launches MaxConcurrency workers. InvalidSetting names the field.
		EStatus Start(std::string& Detail);

		// --- Lifecycle (game thread) ---
		EStatus Open(const FModelSource& Source, uint64_t& OutHandle);
		EStatus Release(uint64_t Handle);
		bool IsModelInUse(const std::string& ModelKey) const;
		EEncoderState GetState() const;
		// The last open's outcome: Ok, or the refusal and its detail (a library status name or a file).
		EStatus GetOpenStatus(std::string* Detail = nullptr) const;
		uint32_t GetDims() const;
		bool GetFingerprint(FFingerprint& Out) const;
		uint64_t GetOpenCount() const { return OpenCount.load(); }
		size_t GetHandleCount() const;

		// --- Encode (game thread) ---
		FTicket EncodeQuery(std::string Utf8, FOwner Owner);
		FTicket EncodeDocuments(std::vector<FDocumentRow> Rows, FOwner Owner);
		// A ticket refused at once, for a refusal the caller found before the text reached the core (InvalidText
		// from an FString). Delivered through the next Drain().
		FTicket Refuse(EKind Kind, EStatus Status, std::string Detail, FOwner Owner);
		// Delivers Cancelled once, through the next Drain(). A queued ticket never runs; an in-flight encode
		// finishes and its result is discarded. False for an unknown or already delivered ticket.
		bool Cancel(FTicket Ticket);
		// Cancels every undelivered ticket of Owner and returns their Cancelled deliveries at once.
		std::vector<FDelivery> CancelOwner(FOwner Owner);
		std::vector<FDelivery> Drain();
		bool GetProgress(FTicket Ticket, size_t& OutDone, size_t& OutTotal) const;

		// --- Blocking (worker threads of the caller's own, never the game thread) ---
		EStatus EncodeBlocking(EKind Kind, const std::string& Utf8, FEmbedding& Out, std::string& Detail);

		// Cancels everything queued, delivers nothing more, waits for the units already running (at most one
		// encode or one open per worker), joins the workers and closes the library handle. Idempotent.
		void Shutdown();
		bool IsShutDown() const;

		FEncodeCache& GetCache() { return Cache; }
		const FServiceConfig& GetConfig() const { return Config; }
		uint64_t GetEncodeCount() const { return EncodeCount.load(); }
		size_t GetQueuedQueryCount() const;
		size_t GetPendingDocumentTicketCount() const;

	private:
		struct FWaiter
		{
			std::mutex Mutex;
			std::condition_variable Done;
			bool bDone = false;
			FDelivery Result;
		};

		struct FTicketState
		{
			FTicket Id = 0;
			FOwner Owner = 0;
			EKind Kind = EKind::Query;
			std::string Utf8;               // a query's text
			std::vector<FDocumentRow> Rows; // a documents ticket's rows; immutable once enqueued
			std::vector<std::vector<float>> Results;
			size_t NextRow = 0;
			size_t RowsDone = 0;
			bool bValidated = false;
			bool bValidating = false;
			bool bTaken = false; // a query a worker has taken
			bool bDelivered = false;
			uint32_t UnitsInFlight = 0;
			std::atomic<bool> bCancelled{false};
			std::shared_ptr<FWaiter> Waiter;
		};

		enum class EUnit : uint8_t
		{
			None,
			Open,
			Close,
			Query,
			Validate,
			Row,
		};
		struct FUnit
		{
			EUnit Type = EUnit::None;
			FTicketState* Ticket = nullptr;
			size_t Row = 0;
			IEncoder* Encoder = nullptr;
		};

		void WorkerLoop(uint32_t Index);
		bool TakeUnitLocked(uint32_t Index, FUnit& Out);
		void RunOpen();
		void RunClose();
		void RunQuery(const FUnit& Unit);
		void RunValidate(const FUnit& Unit);
		void RunRow(const FUnit& Unit);

		FTicketState* NewTicketLocked(EKind Kind, FOwner Owner);
		FTicket EnqueueQueryLocked(std::string Utf8, FOwner Owner, std::shared_ptr<FWaiter> Waiter);
		FTicket EnqueueDocumentsLocked(std::vector<FDocumentRow> Rows, FOwner Owner, std::shared_ptr<FWaiter> Waiter);
		// Delivers once. Into Direct when given, else to the ticket's waiter, else into the outbox.
		void DeliverLocked(FTicketState& T, EOutcome Outcome, EStatus Status, std::string Detail,
			std::vector<FEmbedding> Vectors = {}, bool bFromCache = false, std::vector<FDelivery>* Direct = nullptr);
		void RetireIfDoneLocked(FTicketState& T);
		void BeginCloseLocked();
		void EncodeOne(IEncoder& Encoder, EKind Kind, const std::string& Utf8, std::vector<float>& Out,
			EStatus& Status, std::string& Detail, bool& bFromCache);

		FServiceConfig Config;
		std::shared_ptr<IEncoderFactory> Factory;
		std::shared_ptr<IWorkerThreads> Threads;
		std::function<bool()> IsGameThread;
		FEncodeCache Cache;

		mutable std::mutex Mutex;
		std::condition_variable Work;
		bool bStarted = false;
		bool bShutdown = false;
		bool bJoined = false;
		EEncoderState State = EEncoderState::Closed;
		EStatus OpenStatus = EStatus::Ok;
		std::string OpenDetail;
		FModelSource PendingSource;
		std::string ModelKey;
		bool bOpenPending = false;
		bool bClosePending = false;
		std::unique_ptr<IEncoder> Encoder;
		uint32_t Dims = 0;
		FFingerprint Fingerprint{};
		bool bHasFingerprint = false;
		std::unordered_set<uint64_t> Handles;
		uint64_t NextHandle = 1;
		uint32_t EncodeUnitsInFlight = 0;

		FTicket NextTicket = 1;
		std::unordered_map<FTicket, std::unique_ptr<FTicketState>> Tickets;
		std::deque<FTicketState*> QueryQueue;
		std::deque<FTicketState*> DocumentQueue;
		size_t PendingDocumentTickets = 0;
		std::vector<FDelivery> Outbox;

		std::atomic<uint64_t> EncodeCount{0};
		std::atomic<uint64_t> OpenCount{0};
	};
}
