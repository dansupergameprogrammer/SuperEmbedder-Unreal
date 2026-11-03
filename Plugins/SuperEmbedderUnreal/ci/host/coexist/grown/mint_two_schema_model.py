#!/usr/bin/env python3
"""Mints two_schema_model.sslm, the grown-struct fixtures' model: a small synthetic SuperSLM artifact whose
SchemaMasks (SCM1) section carries TWO schemas, so mapping it fills the engine's per-model schema handle vector
(std::vector<sslm_schema_s>) with two entries, and both handles can be looked up and bound to a sequence.

  mint_two_schema_model.py --superslm <SuperSLM checkout at v1.18.1> [--out two_schema_model.sslm]

The minting code is SuperSLM's own (its tools/ and tools/reference_pipeline/ modules, which this plugin does
not vendor), so a SuperSLM checkout at the pinned tag is needed to re-mint. The minted file is committed beside
this script (two_schema_model.sslm, about 135 KB) with its SHA-256 in two_schema_model.sslm.sha256, so a run
needs only the committed file; this script is how it was made, and re-minting at the same tag reproduces it
byte for byte (--check).

The model: SuperSLM's synthetic full-model configuration with the q/k norm gains dropped, damped-greedy
constants on, and a 7-token JSON vocabulary. Both schemas compile the same one-field object schema and differ
only in whether one reachable state q is accepting (grown_accepts_q accepts it, grown_rejects_q does not).
"""
from __future__ import annotations

import argparse
import hashlib
import importlib
import sys
from dataclasses import replace
from pathlib import Path

HERE = Path(__file__).resolve().parent
NAMES = ("grown_accepts_q", "grown_rejects_q")
SCHEMA = {"type": "object", "additionalProperties": False,
          "properties": {"Prompt_Result": {"type": "string"}}, "required": ["Prompt_Result"]}
# SuperSLM tool modules (names as the SuperSLM repository has them at v1.18.1).
SCM1_MODULE = "t2132_build_g5_fixture"
CONFIG_MODULE = "_t2199_s8_synthetic_full_model_fixture"


def mint(superslm: Path) -> bytes:
    tools = superslm / "tools"
    sys.path[:0] = [str(tools), str(tools / "reference_pipeline")]
    C = importlib.import_module("convert_model")
    P = importlib.import_module("pipeline")
    SC = importlib.import_module("sslm_convert_schema")
    F = importlib.import_module("sslm_format")
    serialize_scm1 = importlib.import_module(SCM1_MODULE)._serialize_scm1
    build_config = importlib.import_module(CONFIG_MODULE).build_config

    cfg = build_config()
    model = P.fixture_model(cfg)
    drop = (".q_norm.gain", ".k_norm.gain")
    model = replace(model, weights={k: v for k, v in model.weights.items() if not k.endswith(drop)},
                    weight_scales={k: v for k, v in model.weight_scales.items() if not k.endswith(drop)})
    sections, _ = C.build_sections(model, enable_damped_greedy=True)
    vocab = [b'{"Prompt_Result":', b'"', b'a', b'\\', b'n', b'"}', b'}']
    vocab += [f"<unused-{i}>".encode() for i in range(len(vocab), cfg.vocab_size)]
    base = SC.compile_schema_to_mask_pages(SCHEMA, vocab, special_ids=frozenset())
    q = base.step(base.start, 0)
    if q == base.start or base.is_accepting(q):
        raise SystemExit(f"q={q} is not a reachable non-accepting state; the schema compiler changed")
    accepts = SC.MaskPages(base.vocab, base.transitions, base.start, base.accepting | {q})
    rejects = SC.MaskPages(base.vocab, base.transitions, base.start, base.accepting - {q})
    sections.append(F.Section(F.SectionType.SCHEMA_MASKS,
                              serialize_scm1([(NAMES[0], accepts), (NAMES[1], rejects)], cfg.vocab_size)))
    data, _ = F.build_artifact(sections, flags=C.artifact_flags_for_model(model) | F.DAMPED_GREEDY_CONSTANTS_FLAG)
    return data


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--superslm", required=True, type=Path)
    ap.add_argument("--out", type=Path, default=HERE / "two_schema_model.sslm")
    ap.add_argument("--check", action="store_true", help="re-mint and compare with the committed file")
    a = ap.parse_args()
    data = mint(a.superslm.resolve())
    digest = hashlib.sha256(data).hexdigest()
    if a.check:
        want = (HERE / "two_schema_model.sslm.sha256").read_text().split()[0]
        print(f"re-minted {len(data)} bytes, sha256 {digest}; committed {want}")
        return 0 if digest == want else 1
    a.out.write_bytes(data)
    (a.out.parent / (a.out.name + ".sha256")).write_text(f"{digest}  {a.out.name}\n", newline="\n")
    print(f"wrote {a.out} ({len(data)} bytes, sha256 {digest})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
