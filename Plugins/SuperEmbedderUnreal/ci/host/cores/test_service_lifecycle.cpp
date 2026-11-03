// The encoder's lifecycle and the per-call contract (sections 2.1 and 2.2; cells L-1, L-2, L-5, L-6, L-9's
// core half, Q-1, Q-3, Q-9, D-2, D-3, D-5, D-6, K-2), over the fake encoder.

#include "support.h"

#include <set>

using namespace SuperEmbedderUe;
using namespace HostSupport;

HOST_TEST(lifecycle_opens_off_the_calling_thread_and_reaches_ready, "L-1")
{
	FServiceConfig C;
	FHarnessService H = MakeFakeService(C, false);
	REQUIRE_TRUE(H.Service != nullptr);
	H.Fake->CloseOpenGate();
	uint64_t Handle = 0;
	EXPECT_EQ(H.Service->GetState(), EEncoderState::Closed);
	REQUIRE_EQ(H.Service->Open(FakeSource("model-a"), Handle), EStatus::Ok);
	// Open() returned while the open itself is still held on a worker.
	EXPECT_EQ(H.Service->GetState(), EEncoderState::Opening);
	REQUIRE_TRUE(H.Fake->WaitOpensStarted(1));
	EXPECT_EQ(H.Service->GetState(), EEncoderState::Opening);
	H.Fake->OpenOpenGate();
	EXPECT_TRUE(WaitUntil([&]() { return H.Service->GetState() == EEncoderState::Ready; }));
	EXPECT_EQ(H.Service->GetDims(), uint32_t(8));
	FFingerprint Fp{};
	EXPECT_TRUE(H.Service->GetFingerprint(Fp));
	EXPECT_TRUE(Fp == FakeFingerprint(1));
}

HOST_TEST(lifecycle_a_failed_open_names_why, "L-1")
{
	FServiceConfig C;
	FHarnessService H = MakeFakeService(C, false);
	H.Fake->OpenStatus = EStatus::OpenFailed;
	uint64_t Handle = 0;
	REQUIRE_EQ(H.Service->Open(FakeSource("model-a"), Handle), EStatus::Ok);
	EXPECT_TRUE(WaitUntil([&]() { return H.Service->GetState() == EEncoderState::Failed; }));
	std::string Detail;
	EXPECT_EQ(H.Service->GetOpenStatus(&Detail), EStatus::OpenFailed);
	EXPECT_EQ(Detail, std::string("SEMB_ARTIFACT_UNREADABLE"));
	// Failed is not terminal: a later open runs again.
	H.Fake->OpenStatus = EStatus::Ok;
	REQUIRE_EQ(H.Service->Open(FakeSource("model-a"), Handle), EStatus::Ok);
	EXPECT_TRUE(WaitUntil([&]() { return H.Service->GetState() == EEncoderState::Ready; }));
	EXPECT_EQ(H.Fake->Opens.load(), 2);
}

HOST_TEST(lifecycle_same_model_joins_another_model_is_refused, "L-2")
{
	FServiceConfig C;
	FHarnessService H = MakeFakeService(C, false);
	H.Fake->CloseOpenGate();
	uint64_t A = 0, B = 0, X = 0, D = 0;
	REQUIRE_EQ(H.Service->Open(FakeSource("model-a"), A), EStatus::Ok);
	EXPECT_EQ(H.Service->Open(FakeSource("model-a"), B), EStatus::Ok); // joins the open in flight
	EXPECT_EQ(H.Service->Open(FakeSource("model-b"), X), EStatus::DifferentModelOpen);
	EXPECT_EQ(X, uint64_t(0));
	H.Fake->OpenOpenGate();
	REQUIRE_TRUE(WaitUntil([&]() { return H.Service->GetState() == EEncoderState::Ready; }));
	EXPECT_EQ(H.Service->Open(FakeSource("model-a"), D), EStatus::Ok); // joins the open encoder
	EXPECT_EQ(H.Service->Open(FakeSource("model-b"), X), EStatus::DifferentModelOpen);
	EXPECT_EQ(H.Service->GetOpenCount(), uint64_t(1));
	EXPECT_EQ(H.Service->GetHandleCount(), size_t(3));
	EXPECT_TRUE(H.Service->IsModelInUse(FakeSource("model-a").Key()));
	EXPECT_TRUE(!H.Service->IsModelInUse(FakeSource("model-b").Key()));
	EXPECT_EQ(H.Service->Release(12345), EStatus::UnknownHandle);
}

HOST_TEST(lifecycle_last_release_closes_on_a_worker, "the 2.1 consumers rule, L-7's core half")
{
	FServiceConfig C;
	FHarnessService H = MakeFakeService(C);
	uint64_t Second = 0;
	REQUIRE_EQ(H.Service->Open(FakeSource("model-a"), Second), EStatus::Ok);
	EXPECT_EQ(H.Service->Release(H.Handle), EStatus::Ok);
	EXPECT_EQ(H.Service->GetState(), EEncoderState::Ready);
	EXPECT_EQ(H.Service->Release(Second), EStatus::Ok);
	EXPECT_TRUE(WaitUntil([&]() { return H.Service->GetState() == EEncoderState::Closed; }));
	EXPECT_EQ(H.Fake->LiveEncoders.load(), 0);
	EXPECT_TRUE(H.Fake->LastCloseThread != std::this_thread::get_id());
	// A re-open after the close opens again.
	REQUIRE_EQ(H.Service->Open(FakeSource("model-b"), Second), EStatus::Ok);
	EXPECT_TRUE(WaitUntil([&]() { return H.Service->GetState() == EEncoderState::Ready; }));
	EXPECT_EQ(H.Service->GetOpenCount(), uint64_t(2));
}

HOST_TEST(lifecycle_release_during_opening_closes_after_the_open, "L-9 (core), G-25")
{
	FServiceConfig C;
	FHarnessService H = MakeFakeService(C, false);
	H.Fake->CloseOpenGate();
	uint64_t A = 0;
	REQUIRE_EQ(H.Service->Open(FakeSource("model-a"), A), EStatus::Ok);
	REQUIRE_TRUE(H.Fake->WaitOpensStarted(1));
	EXPECT_EQ(H.Service->Release(A), EStatus::Ok);
	EXPECT_EQ(H.Service->GetState(), EEncoderState::Opening);
	H.Fake->OpenOpenGate();
	EXPECT_TRUE(WaitUntil([&]() { return H.Service->GetState() == EEncoderState::Closed; }));
	EXPECT_EQ(H.Fake->LiveEncoders.load(), 0);
	// The same during Opening with a handle that joins meanwhile: it stays Ready.
	H.Fake->CloseOpenGate();
	REQUIRE_EQ(H.Service->Open(FakeSource("model-a"), A), EStatus::Ok);
	REQUIRE_TRUE(H.Fake->WaitOpensStarted(2));
	uint64_t B = 0;
	EXPECT_EQ(H.Service->Release(A), EStatus::Ok);
	EXPECT_EQ(H.Service->Open(FakeSource("model-a"), B), EStatus::Ok);
	H.Fake->OpenOpenGate();
	EXPECT_TRUE(WaitUntil([&]() { return H.Service->GetState() == EEncoderState::Ready; }));
}

HOST_TEST(lifecycle_shutdown_during_opening_waits_for_the_open, "L-9 (core)")
{
	FServiceConfig C;
	FHarnessService H = MakeFakeService(C, false);
	H.Fake->CloseOpenGate();
	uint64_t A = 0;
	REQUIRE_EQ(H.Service->Open(FakeSource("model-a"), A), EStatus::Ok);
	REQUIRE_TRUE(H.Fake->WaitOpensStarted(1));
	std::thread Release([&]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		H.Fake->OpenOpenGate();
	});
	const auto T0 = std::chrono::steady_clock::now();
	H.Service->Shutdown();
	const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
	Release.join();
	EXPECT_GE(Ms, 40.0);
	EXPECT_EQ(H.Service->GetState(), EEncoderState::Closed);
	EXPECT_EQ(H.Fake->LiveEncoders.load(), 0);
	EXPECT_TRUE(H.Service->Drain().empty());
}

HOST_TEST(lifecycle_every_encode_before_ready_refuses_on_the_next_drain, "L-6")
{
	FServiceConfig C;
	FHarnessService H = MakeFakeService(C, false);
	// Closed.
	const FTicket T1 = H.Service->EncodeQuery("hello", 0);
	const FTicket T2 = H.Service->EncodeDocuments({{"a", "x"}}, 0);
	// Opening.
	H.Fake->CloseOpenGate();
	uint64_t A = 0;
	REQUIRE_EQ(H.Service->Open(FakeSource("model-a"), A), EStatus::Ok);
	const FTicket T3 = H.Service->EncodeQuery("hello", 0);
	EXPECT_EQ(H.Service->GetQueuedQueryCount(), size_t(0));
	EXPECT_EQ(H.Service->GetPendingDocumentTicketCount(), size_t(0));
	std::vector<FDelivery> D = H.Service->Drain();
	REQUIRE_EQ(D.size(), size_t(3));
	for (const FDelivery& X : D)
	{
		EXPECT_EQ(X.Outcome, EOutcome::Refused);
		EXPECT_EQ(X.Status, EStatus::EncoderNotReady);
	}
	EXPECT_EQ(D[0].Ticket, T1);
	EXPECT_EQ(D[1].Ticket, T2);
	EXPECT_EQ(D[2].Ticket, T3);
	EXPECT_EQ(D[2].Detail, std::string("Opening"));
	// Failed.
	H.Fake->OpenStatus = EStatus::OpenFailed;
	H.Fake->OpenOpenGate();
	REQUIRE_TRUE(WaitUntil([&]() { return H.Service->GetState() == EEncoderState::Failed; }));
	H.Service->EncodeQuery("hello", 0);
	D = H.Service->Drain();
	REQUIRE_EQ(D.size(), size_t(1));
	EXPECT_EQ(D[0].Status, EStatus::EncoderNotReady);
	EXPECT_EQ(H.Fake->Encodes.load(), 0);
}

HOST_TEST(contract_exactly_one_delivery_per_ticket_never_inside_the_call, "Q-1, K-2")
{
	FServiceConfig C;
	C.MaxConcurrency = 2;
	C.MaxPendingQueries = 2000;
	C.MaxPendingDocumentTickets = 2000;
	FHarnessService H = MakeFakeService(C);
	std::set<FTicket> Issued;
	size_t InCall = 0;
	for (int I = 0; I < 1000; ++I)
	{
		FTicket T = 0;
		switch (I % 7)
		{
		case 0: T = H.Service->EncodeQuery("text " + std::to_string(I % 50), 0); break; // repeats become hits
		case 1: T = H.Service->EncodeQuery("", 0); break;                               // EmptyText
		case 2: T = H.Service->EncodeQuery("bad\xFF", 0); break;                         // InvalidUtf8
		case 3: T = H.Service->EncodeDocuments({{"a", "x"}, {"b", "y " + std::to_string(I)}}, 0); break;
		case 4: T = H.Service->EncodeDocuments({{"a", "x"}, {"a", "y"}}, 0); break;     // DuplicateId
		case 5: T = H.Service->Refuse(EKind::Query, EStatus::InvalidText, "", 0); break;
		default:
			T = H.Service->EncodeQuery("cancel me " + std::to_string(I), 0);
			H.Service->Cancel(T);
			break;
		}
		// Every outcome, refusals and hits included, waits for the next drain: nothing is delivered by the
		// call itself, so the issuing code's continuation runs first (the re-entrancy half of Q-1).
		InCall += Issued.count(T);
		Issued.insert(T);
	}
	EXPECT_EQ(InCall, size_t(0));
	EXPECT_EQ(Issued.size(), size_t(1000));
	std::vector<FDelivery> All = DrainCount(*H.Service, 1000, 30000);
	EXPECT_EQ(All.size(), size_t(1000));
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_TRUE(H.Service->Drain().empty());
	std::set<FTicket> Seen;
	size_t Hits = 0;
	for (const FDelivery& D : All)
	{
		EXPECT_TRUE(Issued.count(D.Ticket) == 1);
		EXPECT_TRUE(Seen.insert(D.Ticket).second);
		Hits += D.bFromCache ? 1 : 0;
	}
	EXPECT_GT(Hits, size_t(0));
}

HOST_TEST(contract_blocking_refuses_on_the_game_thread, "Q-3")
{
	FServiceConfig C;
	FHarnessService H = MakeFakeService(C);
	FEmbedding V;
	std::string Detail;
	EXPECT_EQ(H.Service->EncodeBlocking(EKind::Query, "hello", V, Detail), EStatus::GameThreadBlockingRefused);
	EXPECT_EQ(H.Fake->Encodes.load(), 0);
	EStatus OnWorker = EStatus::Count;
	std::thread T([&]() { OnWorker = H.Service->EncodeBlocking(EKind::Query, "hello", V, Detail); });
	T.join();
	EXPECT_EQ(OnWorker, EStatus::Ok);
	EXPECT_TRUE(V.bChecked);
	EXPECT_TRUE(V.Values == FakeVector(EKind::Query, "hello", 8));
	// From inside one of the service's own workers it refuses rather than deadlock.
	EStatus FromWorker = EStatus::Count;
	FEncodeService* Svc = H.Service.get();
	H.Fake->OnEncode = [&FromWorker, Svc]() {
		FEmbedding Inner;
		std::string D;
		FromWorker = Svc->EncodeBlocking(EKind::Query, "inner", Inner, D);
	};
	FDelivery Out;
	REQUIRE_TRUE(WaitDelivery(*H.Service, H.Service->EncodeQuery("outer", 0), Out));
	H.Fake->OnEncode = nullptr;
	EXPECT_EQ(FromWorker, EStatus::WorkerThreadBlockingRefused);
}

HOST_TEST(contract_queue_bound_and_cancel, "Q-9")
{
	FServiceConfig C;
	C.MaxConcurrency = 1;
	C.MaxPendingQueries = 4;
	C.MaxPendingDocumentTickets = 2;
	FHarnessService H = MakeFakeService(C);
	H.Fake->CloseEncodeGate();
	const FTicket InFlight = H.Service->EncodeQuery("q0", 0);
	REQUIRE_TRUE(H.Fake->WaitEncodesStarted(1));
	std::vector<FTicket> Queued;
	for (int I = 1; I <= 4; ++I)
	{
		Queued.push_back(H.Service->EncodeQuery("q" + std::to_string(I), 0));
	}
	const FTicket Over = H.Service->EncodeQuery("q5", 0);
	const FTicket D1 = H.Service->EncodeDocuments({{"a", "x"}}, 0);
	const FTicket D2 = H.Service->EncodeDocuments({{"a", "y"}}, 0);
	const FTicket D3 = H.Service->EncodeDocuments({{"a", "z"}}, 0);
	EXPECT_EQ(H.Service->GetQueuedQueryCount(), size_t(4));
	// Cancel one queued query and the in-flight one.
	EXPECT_TRUE(H.Service->Cancel(Queued[1]));
	EXPECT_TRUE(!H.Service->Cancel(Queued[1])); // once
	EXPECT_TRUE(H.Service->Cancel(InFlight));
	std::vector<FDelivery> D = H.Service->Drain();
	REQUIRE_EQ(D.size(), size_t(4));
	EXPECT_EQ(D[0].Ticket, Over);
	EXPECT_EQ(D[0].Status, EStatus::QueueFull);
	EXPECT_EQ(D[1].Ticket, D3);
	EXPECT_EQ(D[1].Status, EStatus::QueueFull);
	EXPECT_EQ(D[2].Ticket, Queued[1]);
	EXPECT_EQ(D[2].Outcome, EOutcome::Cancelled);
	EXPECT_EQ(D[3].Ticket, InFlight);
	EXPECT_EQ(D[3].Outcome, EOutcome::Cancelled);
	H.Fake->OpenEncodeGate();
	// The rest complete; the in-flight result is discarded; the cancelled queued one never encodes.
	std::vector<FDelivery> Rest = DrainCount(*H.Service, 5);
	EXPECT_EQ(Rest.size(), size_t(5));
	for (const FDelivery& X : Rest)
	{
		EXPECT_EQ(X.Outcome, EOutcome::Completed);
		EXPECT_TRUE(X.Ticket != InFlight && X.Ticket != Queued[1]);
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	EXPECT_TRUE(H.Service->Drain().empty());
	for (const std::string& L : H.Fake->LogCopy())
	{
		EXPECT_NE(L, std::string("Q:q2"));
	}
	(void)D1;
	(void)D2;
}

HOST_TEST(documents_ticket_refusals_encode_nothing, "D-2, D-3")
{
	FServiceConfig C;
	C.MaxDocumentTokens = 10;
	FHarnessService H = MakeFakeService(C);
	FDelivery Out;
	REQUIRE_TRUE(WaitDelivery(*H.Service, H.Service->EncodeDocuments({{"a", "x"}, {"b", "y"}, {"a", "z"}}, 0), Out));
	EXPECT_EQ(Out.Status, EStatus::DuplicateId);
	EXPECT_EQ(Out.Detail, std::string("row 2 id 'a'"));
	REQUIRE_TRUE(WaitDelivery(*H.Service, H.Service->EncodeDocuments({}, 0), Out));
	EXPECT_EQ(Out.Status, EStatus::EmptyTicket);
	REQUIRE_TRUE(WaitDelivery(*H.Service, H.Service->EncodeDocuments({{"a", "x"}, {"", "y"}}, 0), Out));
	EXPECT_EQ(Out.Status, EStatus::InvalidId);
	REQUIRE_TRUE(WaitDelivery(*H.Service, H.Service->EncodeDocuments({{"a", "x"}, {"b", "\xC3"}}, 0), Out));
	EXPECT_EQ(Out.Status, EStatus::InvalidUtf8);
	REQUIRE_TRUE(WaitDelivery(*H.Service, H.Service->EncodeDocuments({{"a", "x"}, {"b", ""}}, 0), Out));
	EXPECT_EQ(Out.Status, EStatus::EmptyText);
	// The fake counts one token per byte: 10 is at the bound, 11 one over. The over-long row is last, so a
	// check that ran per row while encoding would already have encoded the others.
	REQUIRE_TRUE(WaitDelivery(*H.Service, H.Service->EncodeDocuments({{"a", "0123456789"}, {"b", "0123456789x"}}, 0), Out));
	EXPECT_EQ(Out.Status, EStatus::DocumentTooLong);
	EXPECT_TRUE(Out.Detail.find("id 'b'") != std::string::npos);
	EXPECT_EQ(H.Fake->Encodes.load(), 0);
	REQUIRE_TRUE(WaitDelivery(*H.Service, H.Service->EncodeDocuments({{"a", "0123456789"}}, 0), Out));
	EXPECT_EQ(Out.Outcome, EOutcome::Completed);
	EXPECT_EQ(H.Fake->Encodes.load(), 1);
}

HOST_TEST(query_token_bound_is_counted_on_the_worker, "Q-4 (the bound's edge, over the fake's count)")
{
	FServiceConfig C;
	C.MaxQueryTokens = 256;
	FHarnessService H = MakeFakeService(C);
	H.Fake->TokenOverrides["at"] = 256;
	H.Fake->TokenOverrides["over"] = 257;
	FDelivery Out;
	REQUIRE_TRUE(WaitDelivery(*H.Service, H.Service->EncodeQuery("at", 0), Out));
	EXPECT_EQ(Out.Outcome, EOutcome::Completed);
	REQUIRE_TRUE(WaitDelivery(*H.Service, H.Service->EncodeQuery("over", 0), Out));
	EXPECT_EQ(Out.Status, EStatus::QueryTooLong);
	EXPECT_EQ(H.Fake->Encodes.load(), 1);
	REQUIRE_TRUE(WaitDelivery(*H.Service, H.Service->EncodeQuery(std::string(4097, 'x'), 0), Out));
	EXPECT_EQ(Out.Status, EStatus::QueryTooLong); // the byte ceiling at enqueue
	FServiceConfig Bad;
	Bad.MaxQueryTokens = 513;
	std::string Detail;
	EXPECT_EQ(Bad.Validate(Detail), EStatus::InvalidSetting);
	Bad.MaxQueryTokens = 0;
	EXPECT_EQ(Bad.Validate(Detail), EStatus::InvalidSetting);
	Bad = FServiceConfig();
	Bad.MaxConcurrency = 5;
	EXPECT_EQ(Bad.Validate(Detail), EStatus::InvalidSetting);
}

HOST_TEST(documents_owner_end_cancels_between_rows, "D-5, L-4's core half")
{
	FServiceConfig C;
	C.MaxConcurrency = 2;
	FHarnessService H = MakeFakeService(C);
	H.Fake->EncodeDelayMs = 2;
	std::vector<FDocumentRow> Rows;
	for (int I = 0; I < 500; ++I)
	{
		Rows.push_back({"r" + std::to_string(I), "row text " + std::to_string(I)});
	}
	const FTicket Mine = H.Service->EncodeDocuments(Rows, 7);
	const FTicket Other = H.Service->EncodeQuery("another world's query", 8);
	REQUIRE_TRUE(H.Fake->WaitEncodesStarted(5));
	std::vector<FDelivery> Direct = H.Service->CancelOwner(7);
	const int AtCancel = H.Fake->Encodes.load();
	REQUIRE_EQ(Direct.size(), size_t(1));
	EXPECT_EQ(Direct[0].Ticket, Mine);
	EXPECT_EQ(Direct[0].Outcome, EOutcome::Cancelled);
	FDelivery Out;
	REQUIRE_TRUE(WaitDelivery(*H.Service, Other, Out));
	EXPECT_EQ(Out.Outcome, EOutcome::Completed);
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_LE(H.Fake->Encodes.load() - AtCancel, 2); // at most one row per worker after the cancel
	for (const FDelivery& D : H.Service->Drain())
	{
		EXPECT_NE(D.Ticket, Mine);
	}
	EXPECT_EQ(H.Service->GetState(), EEncoderState::Ready);
}

HOST_TEST(documents_cancel_racing_completion_delivers_once, "D-6")
{
	FServiceConfig C;
	C.MaxConcurrency = 2;
	FHarnessService H = MakeFakeService(C);
	for (int Round = 0; Round < 200; ++Round)
	{
		const FTicket T = H.Service->EncodeDocuments({{"a", "race " + std::to_string(Round)}}, 0);
		if (Round % 3 != 0)
		{
			std::this_thread::sleep_for(std::chrono::microseconds(Round * 3));
		}
		H.Service->Cancel(T);
		size_t Count = 0;
		WaitUntil([&]() {
			for (const FDelivery& D : H.Service->Drain())
			{
				Count += D.Ticket == T ? 1 : 0;
			}
			return Count > 0;
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		for (const FDelivery& D : H.Service->Drain())
		{
			Count += D.Ticket == T ? 1 : 0;
		}
		REQUIRE_EQ(Count, size_t(1));
	}
}

HOST_TEST(shutdown_waits_for_one_unit_per_worker_and_delivers_nothing, "L-5, D-4")
{
	FServiceConfig C;
	C.MaxConcurrency = 2;
	FHarnessService H = MakeFakeService(C);
	H.Fake->EncodeDelayMs = 20;
	std::vector<FDocumentRow> Rows;
	for (int I = 0; I < 500; ++I)
	{
		Rows.push_back({"r" + std::to_string(I), "row " + std::to_string(I)});
	}
	H.Service->EncodeDocuments(Rows, 0);
	H.Service->EncodeQuery("a query in flight", 0);
	REQUIRE_TRUE(H.Fake->WaitEncodesStarted(3));
	const int Before = H.Fake->EncodesStarted;
	const auto T0 = std::chrono::steady_clock::now();
	H.Service->Shutdown();
	const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
	EXPECT_LE(H.Fake->EncodesStarted - Before, 0);   // nothing new starts once shutdown begins
	EXPECT_LT(Ms, 20.0 + 1000.0);                    // one encode per worker plus 1 s
	EXPECT_TRUE(H.Service->Drain().empty());
	EXPECT_EQ(H.Fake->LiveEncoders.load(), 0);
	EXPECT_EQ(H.Service->GetState(), EEncoderState::Closed);
	// After shutdown every call refuses by name and nothing is delivered.
	H.Service->EncodeQuery("late", 0);
	EXPECT_TRUE(H.Service->Drain().empty());
	uint64_t Handle = 0;
	EXPECT_EQ(H.Service->Open(FakeSource("model-a"), Handle), EStatus::ShuttingDown);
}

HOST_TEST(cache_hits_are_delivered_on_the_next_drain_once, "K-2, K-5 (service)")
{
	FServiceConfig C;
	C.MaxConcurrency = 2;
	FHarnessService H = MakeFakeService(C);
	H.Fake->EncodeDelayMs = 30;
	// Two concurrent misses on one key make one encode, and both get equal bytes.
	const FTicket A = H.Service->EncodeQuery("same text", 0);
	const FTicket B = H.Service->EncodeQuery("same text", 0);
	std::vector<FDelivery> Both = DrainCount(*H.Service, 2);
	REQUIRE_EQ(Both.size(), size_t(2));
	EXPECT_EQ(H.Fake->Encodes.load(), 1);
	EXPECT_TRUE(Both[0].Vectors[0].Values == Both[1].Vectors[0].Values);
	EXPECT_TRUE((Both[0].Ticket == A && Both[1].Ticket == B) || (Both[0].Ticket == B && Both[1].Ticket == A));
	// A later hit is served at enqueue, but still delivered by the next drain, once.
	const FTicket Hit = H.Service->EncodeQuery("same text", 0);
	std::vector<FDelivery> D = H.Service->Drain();
	REQUIRE_EQ(D.size(), size_t(1));
	EXPECT_EQ(D[0].Ticket, Hit);
	EXPECT_TRUE(D[0].bFromCache);
	EXPECT_TRUE(H.Service->Drain().empty());
	EXPECT_EQ(H.Fake->Encodes.load(), 1);
	// The document kind of the same text is its own entry (K-1).
	FDelivery Doc;
	REQUIRE_TRUE(WaitDelivery(*H.Service, H.Service->EncodeDocuments({{"x", "same text"}}, 0), Doc));
	EXPECT_EQ(H.Fake->Encodes.load(), 2);
	EXPECT_TRUE(Doc.Vectors[0].Values == FakeVector(EKind::Document, "same text", 8));
}
