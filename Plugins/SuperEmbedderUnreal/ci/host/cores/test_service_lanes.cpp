// The two lanes (section 2.2, audit Ms-11; cells W-1 to W-4), deterministic through the fake's encode gate.

#include "support.h"

using namespace SuperEmbedderUe;
using namespace HostSupport;

namespace
{
	std::vector<FDocumentRow> Corpus(int N)
	{
		std::vector<FDocumentRow> Rows;
		for (int I = 0; I < N; ++I)
		{
			Rows.push_back({"r" + std::to_string(I), "row " + std::to_string(I)});
		}
		return Rows;
	}

	// At MaxConcurrency 2, with every document row held: does a query complete while the rows are held?
	bool QueryPassesHeldRows(bool bMutantNoReservation)
	{
		FServiceConfig C;
		C.MaxConcurrency = 2;
		C.bTestDisableQueryReservation = bMutantNoReservation;
		FHarnessService H = MakeFakeService(C);
		H.Fake->SetHoldDocuments(true);
		H.Service->EncodeDocuments(Corpus(500), 0);
		// The validation unit runs, then row encodes start and wait: one worker without the mutant, two with it.
		H.Fake->WaitEncodesStarted(bMutantNoReservation ? 2 : 1);
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		const FTicket Q = H.Service->EncodeQuery("player input", 0);
		FDelivery Out;
		const bool bDone = WaitDelivery(*H.Service, Q, Out, 300);
		H.Fake->SetHoldDocuments(false);
		H.Service->Shutdown();
		return bDone && Out.Outcome == EOutcome::Completed;
	}

	// At MaxConcurrency 1: how many document rows started after the query was issued and before it ran?
	int RowsAheadOfQuery(bool bMutantFifo)
	{
		FServiceConfig C;
		C.MaxConcurrency = 1;
		C.bTestFifoLanes = bMutantFifo;
		FHarnessService H = MakeFakeService(C);
		H.Fake->EncodeDelayMs = 1;
		H.Service->EncodeDocuments(Corpus(60), 0);
		H.Fake->WaitEncodesStarted(3);
		const size_t AtIssue = H.Fake->LogCopy().size();
		FDelivery Out;
		WaitDelivery(*H.Service, H.Service->EncodeQuery("player input", 0), Out, 10000);
		const std::vector<std::string> L = H.Fake->LogCopy();
		int Ahead = 0;
		for (size_t I = AtIssue; I < L.size() && L[I] != "Q:player input"; ++I)
		{
			++Ahead;
		}
		H.Service->Shutdown();
		return Ahead;
	}
}

HOST_TEST(lanes_reserved_worker_runs_a_query_past_running_documents, "W-1, W-2")
{
	EXPECT_TRUE(QueryPassesHeldRows(false));
	// The mutant: no reserved worker. Both workers take document rows, and the query waits behind them.
	EXPECT_TRUE(!QueryPassesHeldRows(true));
}

HOST_TEST(lanes_at_one_worker_a_query_waits_for_at_most_one_row, "W-3, W-4")
{
	EXPECT_LE(RowsAheadOfQuery(false), 1);
	// The mutant: one first-in, first-out queue. The query waits for every remaining row.
	EXPECT_GT(RowsAheadOfQuery(true), 10);
}

HOST_TEST(lanes_documents_complete_in_row_order_with_ids, "D-1 (fake), O-2's order half")
{
	FServiceConfig C;
	C.MaxConcurrency = 4;
	FHarnessService H = MakeFakeService(C);
	FDelivery Out;
	REQUIRE_TRUE(WaitDelivery(*H.Service, H.Service->EncodeDocuments(Corpus(500), 0), Out));
	REQUIRE_EQ(Out.Outcome, EOutcome::Completed);
	REQUIRE_EQ(Out.Vectors.size(), size_t(500));
	REQUIRE_EQ(Out.Ids.size(), size_t(500));
	for (int I = 0; I < 500; ++I)
	{
		EXPECT_EQ(Out.Ids[static_cast<size_t>(I)], "r" + std::to_string(I));
		EXPECT_TRUE(Out.Vectors[static_cast<size_t>(I)].Values == FakeVector(EKind::Document, "row " + std::to_string(I), 8));
		EXPECT_TRUE(Out.Vectors[static_cast<size_t>(I)].Kind == EKind::Document);
	}
	size_t Done = 0, Total = 0;
	EXPECT_TRUE(!H.Service->GetProgress(Out.Ticket, Done, Total)); // retired once delivered
}

HOST_TEST(lanes_documents_enqueue_is_cheap_on_the_calling_thread, "D-8 (core)")
{
	FServiceConfig C;
	FHarnessService H = MakeFakeService(C);
	std::vector<FDocumentRow> Rows = Corpus(500);
	const auto T0 = std::chrono::steady_clock::now();
	const FTicket T = H.Service->EncodeDocuments(static_cast<std::vector<FDocumentRow>&&>(Rows), 0);
	const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
	EXPECT_LT(Ms, 5.0);
	size_t Done = 0, Total = 0;
	EXPECT_TRUE(H.Service->GetProgress(T, Done, Total));
	EXPECT_EQ(Total, size_t(500));
}
