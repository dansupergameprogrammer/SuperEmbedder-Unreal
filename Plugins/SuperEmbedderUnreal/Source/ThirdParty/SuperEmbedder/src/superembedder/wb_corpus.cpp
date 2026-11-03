// src/superembedder/wb_corpus.cpp -- TE-147 (U1 slice 2 build): the sixteen
// committed W-B witness documents, compiled into the library so
// semb_wb_digest (witness_abi.h) can derive W-B at semb_encoder_open without
// a file read (SuperEmbedder_Plan.md sec 3.3, D-SLM3932/D-SLM4036: no field is
// computed from a description or a second read of a file another field was
// derived from -- the corpus must be part of the emitting process's own
// compiled bytes, not a runtime file open). TRANSCRIBED VERBATIM from the
// committed test fixture tests/fixtures/wb/wb-corpus.jsonl (TE-145), in the
// fixture's own index order (wb-00..wb-15) -- only the TEXT is compiled in;
// the ids are not part of W-B's own formula (sec 4.4b: "W-B digests documents"
// over the SEAM, never an id -- TE-96 sec 4.7's id-inertness must-accept) so
// this file carries none. tests/w_b_corpus_witness_test.cpp's own achievement
// cell (property a) is this file's pin against the fixture: it recomputes W-B
// independently from tests/fixtures/wb/wb-corpus.jsonl's own text and asserts
// equality against semb_wb_digest's real, compiled-in-corpus-driven output --
// a byte difference between this file and the fixture is a digest mismatch
// there, not a silent drift (Claude/Brunel/te147-slice2-build-2026-09-07.md
// states the executed cross-check).
#include "wb_corpus.h"

namespace semb {

namespace {
constexpr char kWbDoc00[] = "The blacksmith sells iron nails, horseshoes and a dented helm, and will not haggle before the forge is hot.";
constexpr char kWbDoc01[] = "A travelling alchemist offers tinctures for fever and for sleep, priced by the phial and not the dram.";
constexpr char kWbDoc02[] = "The tanner will trade cured hide for salt or for good rope, and swears the salt is worth more.";
constexpr char kWbDoc03[] = "Fresh bread, two coppers a loaf, and the miller wants his due before the week turns again.";
constexpr char kWbDoc04[] = "The fletcher has goose-feather arrows and one yew longbow left, kept behind the counter for a serious buyer.";
constexpr char kWbDoc05[] = "A cooper mends barrels near the well and grumbles about the rain, which has swollen every stave.";
constexpr char kWbDoc06[] = "The apothecary keeps dried nightshade behind the counter, locked, and asks who sent you first.";
constexpr char kWbDoc07[] = "Wool from the north sells poorly this season, the weaver says, and she blames the roads.";
constexpr char kWbDoc08[] = "The stabler charges three coppers a night and feeds the horse oats, or hay for two.";
constexpr char kWbDoc09[] = "A scribe copies letters for a fee and reads them for another, and remembers every name.";
constexpr char kWbDoc10[] = "The potter fired a kiln of bowls and cracked half of them, so the rest cost double.";
constexpr char kWbDoc11[] = "Salted fish, barrels of it, hauled up from the river landing before the market bell rings.";
constexpr char kWbDoc12[] = "The chandler makes tallow candles that smoke and beeswax that does not, and knows what you can afford.";
constexpr char kWbDoc13[] = "A mason quarries stone for the new gatehouse and wants more hands, paid at week's end.";
constexpr char kWbDoc14[] = "The herbalist gathers yarrow at dawn and sells it bundled by the dozen, roots still damp.";
constexpr char kWbDoc15[] = "Ale is thin this year; the brewer blames the barley, and the drinkers blame the brewer.";

}  // namespace

const WbCorpusDocument kWbCorpusDocuments[kWbCorpusDocumentCount] = {
    {kWbDoc00, sizeof(kWbDoc00) - 1},
    {kWbDoc01, sizeof(kWbDoc01) - 1},
    {kWbDoc02, sizeof(kWbDoc02) - 1},
    {kWbDoc03, sizeof(kWbDoc03) - 1},
    {kWbDoc04, sizeof(kWbDoc04) - 1},
    {kWbDoc05, sizeof(kWbDoc05) - 1},
    {kWbDoc06, sizeof(kWbDoc06) - 1},
    {kWbDoc07, sizeof(kWbDoc07) - 1},
    {kWbDoc08, sizeof(kWbDoc08) - 1},
    {kWbDoc09, sizeof(kWbDoc09) - 1},
    {kWbDoc10, sizeof(kWbDoc10) - 1},
    {kWbDoc11, sizeof(kWbDoc11) - 1},
    {kWbDoc12, sizeof(kWbDoc12) - 1},
    {kWbDoc13, sizeof(kWbDoc13) - 1},
    {kWbDoc14, sizeof(kWbDoc14) - 1},
    {kWbDoc15, sizeof(kWbDoc15) - 1},
};

}  // namespace semb

