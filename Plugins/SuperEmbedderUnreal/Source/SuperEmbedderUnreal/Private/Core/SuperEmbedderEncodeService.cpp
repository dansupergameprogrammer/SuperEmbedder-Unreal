#include "SuperEmbedderEncodeService.h"

#include "SuperEmbedderText.h"

#include <algorithm>

namespace SuperEmbedderUe
{
	namespace
	{
		// The service whose worker this thread is, so a blocking encode from a worker refuses instead of
		// deadlocking at MaxConcurrency 1.
		thread_local const void* tWorkerOf = nullptr;

		template <typename T>
		void EraseOne(std::deque<T*>& Queue, T* Item)
		{
			auto It = std::find(Queue.begin(), Queue.end(), Item);
			if (It != Queue.end())
			{
				Queue.erase(It);
			}
		}
	}

	const char* EncoderStateName(EEncoderState State)
	{
		switch (State)
		{
		case EEncoderState::Closed: return "Closed";
		case EEncoderState::Opening: return "Opening";
		case EEncoderState::Ready: return "Ready";
		case EEncoderState::Failed: return "Failed";
		case EEncoderState::Closing: return "Closing";
		}
		return "Unknown";
	}

	EStatus FServiceConfig::Validate(std::string& Detail) const
	{
		if (MaxConcurrency < 1 || MaxConcurrency > 4)
		{
			Detail = "MaxConcurrency must be 1-4";
			return EStatus::InvalidSetting;
		}
		if (MaxQueryTokens < 1 || MaxQueryTokens > 512)
		{
			Detail = "MaxQueryTokens must be 1-512";
			return EStatus::InvalidSetting;
		}
		if (MaxDocumentTokens < 1 || MaxDocumentTokens > 512)
		{
			Detail = "MaxDocumentTokens must be 1-512";
			return EStatus::InvalidSetting;
		}
		if (MaxPendingQueries < 1 || MaxPendingDocumentTickets < 1)
		{
			Detail = "MaxPendingQueries and MaxPendingDocumentTickets must be at least 1";
			return EStatus::InvalidSetting;
		}
		if (MaxTextBytes < 1)
		{
			Detail = "MaxTextBytes must be at least 1";
			return EStatus::InvalidSetting;
		}
		return EStatus::Ok;
	}

	FEncodeService::FEncodeService(const FServiceConfig& InConfig, std::shared_ptr<IEncoderFactory> InFactory,
		std::shared_ptr<IWorkerThreads> InThreads, std::function<bool()> InIsGameThread)
		: Config(InConfig)
		, Factory(static_cast<std::shared_ptr<IEncoderFactory>&&>(InFactory))
		, Threads(static_cast<std::shared_ptr<IWorkerThreads>&&>(InThreads))
		, IsGameThread(static_cast<std::function<bool()>&&>(InIsGameThread))
		, Cache(InConfig.MaxCacheEntries)
	{
	}

	FEncodeService::~FEncodeService()
	{
		Shutdown();
	}

	EStatus FEncodeService::Start(std::string& Detail)
	{
		const EStatus Valid = Config.Validate(Detail);
		if (Valid != EStatus::Ok)
		{
			return Valid;
		}
		std::lock_guard<std::mutex> Lock(Mutex);
		if (bShutdown)
		{
			return EStatus::ShuttingDown;
		}
		if (bStarted)
		{
			return EStatus::Ok;
		}
		bStarted = true;
		static const char* const kNames[4] = {"SuperEmbedder Worker 0", "SuperEmbedder Worker 1",
			"SuperEmbedder Worker 2", "SuperEmbedder Worker 3"};
		for (uint32_t I = 0; I < Config.MaxConcurrency; ++I)
		{
			Threads->Launch(kNames[I], [this, I]() { WorkerLoop(I); });
		}
		return EStatus::Ok;
	}

	// --- Lifecycle ---------------------------------------------------------------------------------------------

	EStatus FEncodeService::Open(const FModelSource& Source, uint64_t& OutHandle)
	{
		OutHandle = 0;
		const std::string Key = Source.Key();
		std::lock_guard<std::mutex> Lock(Mutex);
		if (bShutdown)
		{
			return EStatus::ShuttingDown;
		}
		if (!bStarted)
		{
			return EStatus::EncoderNotReady;
		}
		switch (State)
		{
		case EEncoderState::Opening:
		case EEncoderState::Ready:
			if (Key != ModelKey)
			{
				return EStatus::DifferentModelOpen;
			}
			break; // join
		case EEncoderState::Closing:
			return EStatus::EncoderClosing;
		case EEncoderState::Closed:
		case EEncoderState::Failed:
			State = EEncoderState::Opening;
			ModelKey = Key;
			PendingSource = Source;
			bOpenPending = true;
			OpenStatus = EStatus::Ok;
			OpenDetail.clear();
			Handles.clear();
			Work.notify_all();
			break;
		}
		OutHandle = NextHandle++;
		Handles.insert(OutHandle);
		return EStatus::Ok;
	}

	EStatus FEncodeService::Release(uint64_t Handle)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (Handles.erase(Handle) == 0)
		{
			return EStatus::UnknownHandle;
		}
		if (Handles.empty() && State == EEncoderState::Ready && !bShutdown)
		{
			BeginCloseLocked();
		}
		// During Opening the open finishes first; RunOpen() then closes when no handle is left.
		return EStatus::Ok;
	}

	void FEncodeService::BeginCloseLocked()
	{
		State = EEncoderState::Closing;
		bClosePending = true;
		std::vector<FTicketState*> Live;
		for (auto& Pair : Tickets)
		{
			if (!Pair.second->bDelivered)
			{
				Live.push_back(Pair.second.get());
			}
		}
		for (FTicketState* T : Live)
		{
			T->bCancelled = true;
			DeliverLocked(*T, EOutcome::Cancelled, EStatus::Cancelled, "the encoder is closing");
		}
		Work.notify_all();
	}

	bool FEncodeService::IsModelInUse(const std::string& Key) const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return (State == EEncoderState::Opening || State == EEncoderState::Ready || State == EEncoderState::Closing) &&
			Key == ModelKey;
	}

	EEncoderState FEncodeService::GetState() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return State;
	}

	EStatus FEncodeService::GetOpenStatus(std::string* Detail) const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (Detail != nullptr)
		{
			*Detail = OpenDetail;
		}
		return OpenStatus;
	}

	uint32_t FEncodeService::GetDims() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return State == EEncoderState::Ready ? Dims : 0;
	}

	bool FEncodeService::GetFingerprint(FFingerprint& Out) const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		if (State != EEncoderState::Ready || !bHasFingerprint)
		{
			return false;
		}
		Out = Fingerprint;
		return true;
	}

	size_t FEncodeService::GetHandleCount() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return Handles.size();
	}

	// --- Tickets -----------------------------------------------------------------------------------------------

	FEncodeService::FTicketState* FEncodeService::NewTicketLocked(EKind Kind, FOwner Owner)
	{
		std::unique_ptr<FTicketState> T(new FTicketState());
		T->Id = NextTicket++;
		T->Kind = Kind;
		T->Owner = Owner;
		FTicketState* Raw = T.get();
		Tickets.emplace(Raw->Id, static_cast<std::unique_ptr<FTicketState>&&>(T));
		return Raw;
	}

	void FEncodeService::DeliverLocked(FTicketState& T, EOutcome Outcome, EStatus Status, std::string Detail,
		std::vector<FEmbedding> Vectors, bool bFromCache, std::vector<FDelivery>* Direct)
	{
		if (T.bDelivered)
		{
			return;
		}
		T.bDelivered = true;
		if (T.Kind == EKind::Query)
		{
			if (!T.bTaken)
			{
				EraseOne(QueryQueue, &T);
			}
		}
		else
		{
			EraseOne(DocumentQueue, &T);
			if (PendingDocumentTickets > 0 && !T.Rows.empty())
			{
				--PendingDocumentTickets;
			}
		}
		FDelivery D;
		D.Ticket = T.Id;
		D.Owner = T.Owner;
		D.Kind = T.Kind;
		D.Outcome = Outcome;
		D.Status = Status;
		D.Detail = static_cast<std::string&&>(Detail);
		D.Vectors = static_cast<std::vector<FEmbedding>&&>(Vectors);
		D.bFromCache = bFromCache;
		if (T.Kind == EKind::Document && Outcome == EOutcome::Completed)
		{
			D.Ids.reserve(T.Rows.size());
			for (const FDocumentRow& R : T.Rows)
			{
				D.Ids.push_back(R.Id);
			}
		}
		if (T.Waiter)
		{
			std::shared_ptr<FWaiter> W = T.Waiter;
			{
				std::lock_guard<std::mutex> WLock(W->Mutex);
				W->Result = static_cast<FDelivery&&>(D);
				W->bDone = true;
			}
			W->Done.notify_all();
		}
		else if (Direct != nullptr)
		{
			Direct->push_back(static_cast<FDelivery&&>(D));
		}
		else if (!bShutdown)
		{
			Outbox.push_back(static_cast<FDelivery&&>(D));
		}
		RetireIfDoneLocked(T);
	}

	void FEncodeService::RetireIfDoneLocked(FTicketState& T)
	{
		if (T.bDelivered && T.UnitsInFlight == 0)
		{
			Tickets.erase(T.Id); // destroys T
		}
	}

	FTicket FEncodeService::Refuse(EKind Kind, EStatus Status, std::string Detail, FOwner Owner)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		FTicketState* T = NewTicketLocked(Kind, Owner);
		const FTicket Id = T->Id;
		DeliverLocked(*T, EOutcome::Refused, Status, static_cast<std::string&&>(Detail));
		return Id;
	}

	FTicket FEncodeService::EncodeQuery(std::string Utf8, FOwner Owner)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return EnqueueQueryLocked(static_cast<std::string&&>(Utf8), Owner, nullptr);
	}

	FTicket FEncodeService::EnqueueQueryLocked(std::string Utf8, FOwner Owner, std::shared_ptr<FWaiter> Waiter)
	{
		FTicketState* T = NewTicketLocked(EKind::Query, Owner);
		T->Waiter = static_cast<std::shared_ptr<FWaiter>&&>(Waiter);
		const FTicket Id = T->Id;
		if (bShutdown)
		{
			DeliverLocked(*T, EOutcome::Refused, EStatus::ShuttingDown, "");
			return Id;
		}
		if (State != EEncoderState::Ready)
		{
			DeliverLocked(*T, EOutcome::Refused, EStatus::EncoderNotReady, EncoderStateName(State));
			return Id;
		}
		if (Utf8.size() > Config.MaxTextBytes)
		{
			DeliverLocked(*T, EOutcome::Refused, EStatus::QueryTooLong,
				std::to_string(Utf8.size()) + " bytes, over the " + std::to_string(Config.MaxTextBytes) + "-byte ceiling");
			return Id;
		}
		if (!IsValidUtf8(Utf8.data(), Utf8.size()))
		{
			DeliverLocked(*T, EOutcome::Refused, EStatus::InvalidUtf8, "");
			return Id;
		}
		if (Utf8.empty())
		{
			DeliverLocked(*T, EOutcome::Refused, EStatus::EmptyText, "");
			return Id;
		}
		// A hit is delivered on the next Drain(), like every other outcome (K-2).
		std::vector<float> Hit;
		if (Cache.Find(FCacheKey::Of(EKind::Query, Utf8), Hit))
		{
			std::vector<FEmbedding> V;
			V.push_back(FEmbedding::Checked(static_cast<std::vector<float>&&>(Hit), Fingerprint, EKind::Query));
			DeliverLocked(*T, EOutcome::Completed, EStatus::Ok, "", static_cast<std::vector<FEmbedding>&&>(V), true);
			return Id;
		}
		if (QueryQueue.size() >= Config.MaxPendingQueries)
		{
			DeliverLocked(*T, EOutcome::Refused, EStatus::QueueFull,
				std::to_string(QueryQueue.size()) + " queries waiting");
			return Id;
		}
		T->Utf8 = static_cast<std::string&&>(Utf8);
		QueryQueue.push_back(T);
		Work.notify_all();
		return Id;
	}

	FTicket FEncodeService::EncodeDocuments(std::vector<FDocumentRow> Rows, FOwner Owner)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return EnqueueDocumentsLocked(static_cast<std::vector<FDocumentRow>&&>(Rows), Owner, nullptr);
	}

	FTicket FEncodeService::EnqueueDocumentsLocked(std::vector<FDocumentRow> Rows, FOwner Owner, std::shared_ptr<FWaiter> Waiter)
	{
		FTicketState* T = NewTicketLocked(EKind::Document, Owner);
		T->Waiter = static_cast<std::shared_ptr<FWaiter>&&>(Waiter);
		const FTicket Id = T->Id;
		if (bShutdown)
		{
			DeliverLocked(*T, EOutcome::Refused, EStatus::ShuttingDown, "");
			return Id;
		}
		if (State != EEncoderState::Ready)
		{
			DeliverLocked(*T, EOutcome::Refused, EStatus::EncoderNotReady, EncoderStateName(State));
			return Id;
		}
		if (Rows.empty())
		{
			DeliverLocked(*T, EOutcome::Refused, EStatus::EmptyTicket, "");
			return Id;
		}
		if (PendingDocumentTickets >= Config.MaxPendingDocumentTickets)
		{
			DeliverLocked(*T, EOutcome::Refused, EStatus::QueueFull,
				std::to_string(PendingDocumentTickets) + " documents tickets pending");
			return Id;
		}
		// The enqueue only moves the rows; every per-row check runs on a worker (G-29).
		T->Rows = static_cast<std::vector<FDocumentRow>&&>(Rows);
		T->Results.resize(T->Rows.size());
		++PendingDocumentTickets;
		DocumentQueue.push_back(T);
		Work.notify_all();
		return Id;
	}

	bool FEncodeService::Cancel(FTicket Ticket)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		auto It = Tickets.find(Ticket);
		if (It == Tickets.end() || It->second->bDelivered)
		{
			return false;
		}
		FTicketState& T = *It->second;
		T.bCancelled = true;
		DeliverLocked(T, EOutcome::Cancelled, EStatus::Cancelled, "cancelled");
		Work.notify_all();
		return true;
	}

	std::vector<FDelivery> FEncodeService::CancelOwner(FOwner Owner)
	{
		std::vector<FDelivery> Out;
		std::lock_guard<std::mutex> Lock(Mutex);
		std::vector<FTicketState*> Match;
		for (auto& Pair : Tickets)
		{
			if (Pair.second->Owner == Owner && !Pair.second->bDelivered)
			{
				Match.push_back(Pair.second.get());
			}
		}
		std::sort(Match.begin(), Match.end(), [](const FTicketState* A, const FTicketState* B) { return A->Id < B->Id; });
		for (FTicketState* T : Match)
		{
			T->bCancelled = true;
			DeliverLocked(*T, EOutcome::Cancelled, EStatus::Cancelled, "the owner ended", {}, false, &Out);
		}
		// A refusal already in the outbox for this owner is dropped too: the owner is gone.
		Outbox.erase(std::remove_if(Outbox.begin(), Outbox.end(), [Owner](const FDelivery& D) { return D.Owner == Owner; }),
			Outbox.end());
		Work.notify_all();
		return Out;
	}

	std::vector<FDelivery> FEncodeService::Drain()
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		std::vector<FDelivery> Out;
		Out.swap(Outbox);
		return Out;
	}

	bool FEncodeService::GetProgress(FTicket Ticket, size_t& OutDone, size_t& OutTotal) const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		auto It = Tickets.find(Ticket);
		if (It == Tickets.end())
		{
			return false;
		}
		OutDone = It->second->Kind == EKind::Document ? It->second->RowsDone : 0;
		OutTotal = It->second->Kind == EKind::Document ? It->second->Rows.size() : 1;
		return true;
	}

	size_t FEncodeService::GetQueuedQueryCount() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return QueryQueue.size();
	}

	size_t FEncodeService::GetPendingDocumentTicketCount() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return PendingDocumentTickets;
	}

	EStatus FEncodeService::EncodeBlocking(EKind Kind, const std::string& Utf8, FEmbedding& Out, std::string& Detail)
	{
		if (IsGameThread && IsGameThread())
		{
			return EStatus::GameThreadBlockingRefused;
		}
		if (tWorkerOf == this)
		{
			return EStatus::WorkerThreadBlockingRefused;
		}
		std::shared_ptr<FWaiter> W = std::make_shared<FWaiter>();
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			if (Kind == EKind::Query)
			{
				EnqueueQueryLocked(Utf8, 0, W);
			}
			else
			{
				std::vector<FDocumentRow> Rows(1);
				Rows[0].Id = "blocking";
				Rows[0].Utf8 = Utf8;
				EnqueueDocumentsLocked(static_cast<std::vector<FDocumentRow>&&>(Rows), 0, W);
			}
		}
		std::unique_lock<std::mutex> WLock(W->Mutex);
		W->Done.wait(WLock, [&W]() { return W->bDone; });
		Detail = W->Result.Detail;
		if (W->Result.Outcome == EOutcome::Completed && !W->Result.Vectors.empty())
		{
			Out = W->Result.Vectors[0];
			return EStatus::Ok;
		}
		return W->Result.Status;
	}

	// --- Shutdown ----------------------------------------------------------------------------------------------

	void FEncodeService::Shutdown()
	{
		std::unique_ptr<IEncoder> ToClose;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			if (bJoined)
			{
				return;
			}
			bShutdown = true;
			Outbox.clear();
			std::vector<FTicketState*> Live;
			for (auto& Pair : Tickets)
			{
				if (!Pair.second->bDelivered)
				{
					Live.push_back(Pair.second.get());
				}
			}
			for (FTicketState* T : Live)
			{
				T->bCancelled = true;
				// Only blocking waiters hear of it (they would otherwise wait forever); no delegate fires.
				DeliverLocked(*T, EOutcome::Refused, EStatus::ShuttingDown, "");
			}
			Outbox.clear();
			Work.notify_all();
		}
		if (bStarted)
		{
			Threads->JoinAll();
		}
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			bJoined = true;
			ToClose = static_cast<std::unique_ptr<IEncoder>&&>(Encoder);
			State = EEncoderState::Closed;
			Handles.clear();
			Tickets.clear();
			QueryQueue.clear();
			DocumentQueue.clear();
			PendingDocumentTickets = 0;
		}
		ToClose.reset();
	}

	bool FEncodeService::IsShutDown() const
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return bShutdown;
	}

	// --- Workers -----------------------------------------------------------------------------------------------

	bool FEncodeService::TakeUnitLocked(uint32_t Index, FUnit& Out)
	{
		if (bOpenPending)
		{
			bOpenPending = false;
			Out.Type = EUnit::Open;
			return true;
		}
		if (bClosePending && EncodeUnitsInFlight == 0)
		{
			bClosePending = false;
			Out.Type = EUnit::Close;
			return true;
		}
		if (State != EEncoderState::Ready || !Encoder)
		{
			return false;
		}
		const bool bReserved = Index == 0 && Config.MaxConcurrency >= 2 && !Config.bTestDisableQueryReservation;

		// The oldest document work that is free to take, if any.
		FTicketState* Doc = nullptr;
		bool bDocValidate = false;
		if (!bReserved)
		{
			for (FTicketState* T : DocumentQueue)
			{
				if (T->bCancelled || T->bDelivered)
				{
					continue;
				}
				if (!T->bValidated && !T->bValidating)
				{
					Doc = T;
					bDocValidate = true;
					break;
				}
				if (T->bValidated && T->NextRow < T->Rows.size())
				{
					Doc = T;
					break;
				}
			}
		}
		FTicketState* Query = QueryQueue.empty() ? nullptr : QueryQueue.front();
		bool bTakeQuery = Query != nullptr;
		if (bTakeQuery && Doc != nullptr && Config.bTestFifoLanes)
		{
			bTakeQuery = Query->Id < Doc->Id; // the W-4 mutant: first in, first out across both lanes
		}
		if (bTakeQuery)
		{
			QueryQueue.pop_front();
			Query->bTaken = true;
			++Query->UnitsInFlight;
			++EncodeUnitsInFlight;
			Out.Type = EUnit::Query;
			Out.Ticket = Query;
			Out.Encoder = Encoder.get();
			return true;
		}
		if (Doc == nullptr)
		{
			return false;
		}
		++Doc->UnitsInFlight;
		++EncodeUnitsInFlight;
		Out.Ticket = Doc;
		Out.Encoder = Encoder.get();
		if (bDocValidate)
		{
			Doc->bValidating = true;
			Out.Type = EUnit::Validate;
		}
		else
		{
			Out.Type = EUnit::Row;
			Out.Row = Doc->NextRow++;
		}
		return true;
	}

	void FEncodeService::WorkerLoop(uint32_t Index)
	{
		tWorkerOf = this;
		for (;;)
		{
			FUnit Unit;
			{
				std::unique_lock<std::mutex> Lock(Mutex);
				Work.wait(Lock, [&]() { return bShutdown || TakeUnitLocked(Index, Unit); });
				if (Unit.Type == EUnit::None)
				{
					break; // shutdown with nothing taken
				}
			}
			switch (Unit.Type)
			{
			case EUnit::Open: RunOpen(); break;
			case EUnit::Close: RunClose(); break;
			case EUnit::Query: RunQuery(Unit); break;
			case EUnit::Validate: RunValidate(Unit); break;
			case EUnit::Row: RunRow(Unit); break;
			case EUnit::None: break;
			}
		}
		tWorkerOf = nullptr;
	}

	void FEncodeService::RunOpen()
	{
		FModelSource Source;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			Source = PendingSource;
		}
		OpenCount.fetch_add(1);
		FOpenResult R = Factory->Open(Source, Config.MaxConcurrency);
		std::unique_ptr<IEncoder> Discard;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			PendingSource = FModelSource();
			if (bShutdown)
			{
				Discard = static_cast<std::unique_ptr<IEncoder>&&>(R.Encoder);
			}
			else if (R.Status != EStatus::Ok || !R.Encoder)
			{
				State = EEncoderState::Failed;
				OpenStatus = R.Status == EStatus::Ok ? EStatus::OpenFailed : R.Status;
				OpenDetail = R.Detail;
				Handles.clear();
				ModelKey.clear();
			}
			else
			{
				Encoder = static_cast<std::unique_ptr<IEncoder>&&>(R.Encoder);
				Dims = Encoder->Dims();
				Fingerprint = Encoder->Fingerprint();
				bHasFingerprint = true;
				Cache.BindFingerprint(Fingerprint); // K-7: another fingerprint clears it
				State = EEncoderState::Ready;
				OpenStatus = EStatus::Ok;
				if (Handles.empty())
				{
					BeginCloseLocked(); // every handle was released while the open ran (G-25)
				}
			}
			Work.notify_all();
		}
		Discard.reset();
	}

	void FEncodeService::RunClose()
	{
		std::unique_ptr<IEncoder> ToClose;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			ToClose = static_cast<std::unique_ptr<IEncoder>&&>(Encoder);
		}
		ToClose.reset(); // the library close, off the game thread
		std::lock_guard<std::mutex> Lock(Mutex);
		if (!bShutdown)
		{
			State = EEncoderState::Closed;
			ModelKey.clear();
			bHasFingerprint = false;
			Dims = 0;
		}
		Work.notify_all();
	}

	void FEncodeService::EncodeOne(IEncoder& Enc, EKind Kind, const std::string& Utf8, std::vector<float>& Out,
		EStatus& Status, std::string& Detail, bool& bFromCache)
	{
		bFromCache = false;
		const FCacheKey Key = FCacheKey::Of(Kind, Utf8);
		if (Config.MaxCacheEntries > 0 && Cache.Acquire(Key, Out) == FEncodeCache::EAcquire::Hit)
		{
			bFromCache = true;
			Status = EStatus::Ok;
			return;
		}
		EncodeCount.fetch_add(1);
		Status = Enc.Encode(Kind, Utf8, Out, Detail);
		if (Config.MaxCacheEntries > 0)
		{
			if (Status == EStatus::Ok)
			{
				Cache.Fill(Key, Out);
			}
			else
			{
				Cache.Abandon(Key);
			}
		}
	}

	void FEncodeService::RunQuery(const FUnit& Unit)
	{
		FTicketState& T = *Unit.Ticket;
		EStatus Status = EStatus::Ok;
		std::string Detail;
		std::vector<float> Values;
		bool bFromCache = false;
		int64_t Tokens = 0;
		if (!T.bCancelled)
		{
			Status = Unit.Encoder->CountQueryBodyTokens(T.Utf8, Tokens, Detail);
			if (Status == EStatus::Ok && Tokens > static_cast<int64_t>(Config.MaxQueryTokens))
			{
				Status = EStatus::QueryTooLong;
				Detail = std::to_string(Tokens) + " body tokens, over MaxQueryTokens " + std::to_string(Config.MaxQueryTokens);
			}
			if (Status == EStatus::Ok && !T.bCancelled)
			{
				EncodeOne(*Unit.Encoder, EKind::Query, T.Utf8, Values, Status, Detail, bFromCache);
			}
		}
		std::lock_guard<std::mutex> Lock(Mutex);
		--EncodeUnitsInFlight;
		--T.UnitsInFlight;
		if (T.bDelivered)
		{
			RetireIfDoneLocked(T); // cancelled in flight: the result is discarded
		}
		else if (Status == EStatus::Ok)
		{
			std::vector<FEmbedding> V;
			V.push_back(FEmbedding::Checked(static_cast<std::vector<float>&&>(Values), Fingerprint, EKind::Query));
			DeliverLocked(T, EOutcome::Completed, EStatus::Ok, "", static_cast<std::vector<FEmbedding>&&>(V), bFromCache);
		}
		else
		{
			DeliverLocked(T, EOutcome::Refused, Status, Detail);
		}
		Work.notify_all();
	}

	void FEncodeService::RunValidate(const FUnit& Unit)
	{
		FTicketState& T = *Unit.Ticket;
		EStatus Status = EStatus::Ok;
		std::string Detail;
		std::unordered_set<std::string> Seen;
		for (size_t I = 0; I < T.Rows.size() && Status == EStatus::Ok; ++I)
		{
			if (T.bCancelled)
			{
				break;
			}
			const FDocumentRow& R = T.Rows[I];
			const std::string Where = "row " + std::to_string(I) + " id '" + R.Id + "'";
			if (CheckId(R.Id) != EStatus::Ok)
			{
				Status = EStatus::InvalidId;
				Detail = "row " + std::to_string(I);
			}
			else if (!Seen.insert(R.Id).second)
			{
				Status = EStatus::DuplicateId;
				Detail = Where;
			}
			else if (R.Utf8.size() > Config.MaxTextBytes)
			{
				Status = EStatus::DocumentTooLong;
				Detail = Where + ": " + std::to_string(R.Utf8.size()) + " bytes, over the " +
					std::to_string(Config.MaxTextBytes) + "-byte ceiling";
			}
			else if (!IsValidUtf8(R.Utf8.data(), R.Utf8.size()))
			{
				Status = EStatus::InvalidUtf8;
				Detail = Where;
			}
			else if (R.Utf8.empty())
			{
				Status = EStatus::EmptyText;
				Detail = Where;
			}
			else
			{
				int64_t Tokens = 0;
				std::string Lib;
				const EStatus C = Unit.Encoder->CountQueryBodyTokens(R.Utf8, Tokens, Lib);
				if (C != EStatus::Ok)
				{
					Status = C;
					Detail = Where + ": " + Lib;
				}
				else if (Tokens > static_cast<int64_t>(Config.MaxDocumentTokens))
				{
					Status = EStatus::DocumentTooLong;
					Detail = Where + ": " + std::to_string(Tokens) + " tokens (counted as a query body; no document-kind count at this SuperEmbedder release), over MaxDocumentTokens " +
						std::to_string(Config.MaxDocumentTokens);
				}
			}
		}
		std::lock_guard<std::mutex> Lock(Mutex);
		--EncodeUnitsInFlight;
		--T.UnitsInFlight;
		T.bValidating = false;
		if (T.bDelivered)
		{
			RetireIfDoneLocked(T);
		}
		else if (Status != EStatus::Ok)
		{
			DeliverLocked(T, EOutcome::Refused, Status, Detail);
		}
		else
		{
			T.bValidated = true;
		}
		Work.notify_all();
	}

	void FEncodeService::RunRow(const FUnit& Unit)
	{
		FTicketState& T = *Unit.Ticket;
		EStatus Status = EStatus::Ok;
		std::string Detail;
		std::vector<float> Values;
		bool bFromCache = false;
		const bool bStop = T.bCancelled;
		if (!bStop)
		{
			EncodeOne(*Unit.Encoder, EKind::Document, T.Rows[Unit.Row].Utf8, Values, Status, Detail, bFromCache);
		}
		std::lock_guard<std::mutex> Lock(Mutex);
		--EncodeUnitsInFlight;
		--T.UnitsInFlight;
		if (T.bDelivered || bStop)
		{
			RetireIfDoneLocked(T);
		}
		else if (Status != EStatus::Ok)
		{
			DeliverLocked(T, EOutcome::Refused, Status,
				"row " + std::to_string(Unit.Row) + " id '" + T.Rows[Unit.Row].Id + "': " + Detail);
		}
		else
		{
			T.Results[Unit.Row] = static_cast<std::vector<float>&&>(Values);
			++T.RowsDone;
			if (T.RowsDone == T.Rows.size())
			{
				std::vector<FEmbedding> V;
				V.reserve(T.Rows.size());
				for (std::vector<float>& R : T.Results)
				{
					V.push_back(FEmbedding::Checked(static_cast<std::vector<float>&&>(R), Fingerprint, EKind::Document));
				}
				DeliverLocked(T, EOutcome::Completed, EStatus::Ok, "", static_cast<std::vector<FEmbedding>&&>(V));
			}
		}
		Work.notify_all();
	}
}
