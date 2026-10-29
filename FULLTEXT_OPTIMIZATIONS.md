# Full-Text Index Performance Optimizations

This document describes the optimizations implemented for the seekdb full-text
index (FTS) subsystem.  The target benchmark is
`tools/benchmark/fts_large_bench.sh`, which measures index build, tokenization
and MATCH query latency.

All changes preserve existing tokenization and retrieval semantics.  The
correctness evidence is listed in section 4.

## 1. Bottleneck analysis

The benchmark exercises three paths:

* **Index build** (`ALTER TABLE ... ADD FULLTEXT INDEX`): every input document
  creates a parser instance and tokenizes the text into `word -> doc_id` rows.
* **Tokenization** (`SELECT tokenize(...)`): same tokenizer called directly from
  SQL.
* **Query** (`MATCH ... AGAINST`): the query string is tokenized, then the
  inverted index is scanned.

Profiling the baseline showed the following recurring costs:

1. **Dictionary re-initialization per document/query.**  Every `ObIKFTParser`
   re-fetched the pinned range handles from the KV cache, allocated one
   `ObFTCacheDict` wrapper per range, and rebuilt the range vector.  The
   built-in IK dictionaries are immutable, so this work is redundant.
2. **Per-token heap allocations in normalization.**  `ObAddWord::casedown_word`
   copied every token into the allocator even when the token was already lower
   case (all CJK text), and `ObStopWordChecker::check_stopword` always ran a
   charset conversion and a second allocation.
3. **Unicode decoding on the character-classification hot path.**  Every
   character went through a full `decode_unicode` plus a chain of block-range
   predicates.
4. **Per-token allocation in the BENG parser.**  Each English token was
   allocated individually from the arena.
5. **Linear range lookup in the IK dictionary.**  `find_first_char_range()`
   scanned the range vector for every CJK character.

## 2. Changes

### 2.1 Shared immutable IK dictionaries

Files: `src/storage/fts/dict/ob_ft_dict_hub.{h,cpp}`,
`src/storage/fts/ob_ik_ft_parser.{h,cpp}`

`ObFTDictHub::get_cached_dict()` builds each built-in dictionary once, keeps the
pinned KV-cache handles and the `ObFTRangeDict` in a long-lived arena, and hands
the same immutable object to every parser.  Building is guarded by a mutex so
that concurrent first use still builds exactly once; the published pointer is
read with an acquire load so the steady state takes no lock at all.  `ObIKFTParser`
now only borrows these dictionaries and no longer owns/frees them.

Before: per document, 3 dictionaries × (range-count KV lookups + wrapper
allocations + vector construction), plus an exclusive lock on the hub for every
fetch (which serialised parser initialisation across index-build threads).
After: 3 acquire loads, with a one-time build per process.

### 2.2 Allocation-free token normalization

Files: `src/storage/fts/ob_fts_stop_word.{h,cpp}`

* `ObAddWord::casedown_word()` lowercases into a small stack buffer first.  If
  the conversion does not change the token (the common case for CJK and
  lowercase ASCII), the original token is kept and no allocator call is made.
  Tokens that do change are copied once with their final size.
* `ObStopWordChecker::check_stopword()` skips `charset_convert()` entirely when
  the token already uses the stop-word set's collation, which avoids one
  allocation and copy per token.
* The per-document `stopword_allocator_` from the earlier change is retained so
  the fallback conversion path also reuses one arena per document.

### 2.3 Fast UTF8MB4 character classification

File: `src/storage/fts/ik/ob_ik_char_util.h`

`classify_utf8mb4_fast()` classifies ASCII letters/digits and the CJK Unified
Ideographs block (`U+4E00..U+9FFF`) directly from the UTF-8 bytes, avoiding the
Unicode decode for the overwhelmingly common characters.  All other characters
(full-width forms, kana, Hangul, compatibility ideographs, ...) fall back to
`do_classify()` so the original classification is bit-for-bit preserved.

An earlier, more aggressive version of this fast path incorrectly treated
full-width Latin letters and digits as "other CJK"; that was caught by the
tokenizer regression corpus and removed.

### 2.4 Binary search over dictionary ranges

File: `src/storage/fts/dict/ob_ft_range_dict.cpp`

`find_first_char_range()` now performs a binary search over the sorted range
vector using a byte-wise comparator for the `UTF8MB4_BIN` collation, instead of
a linear scan with a general collation call per range.  The range vector is
built from sorted dictionary keys, so start/end ranges are monotonic.

### 2.5 Bulk token buffer for the BENG parser

Files: `src/storage/fts/ob_beng_ft_parser.{h,cpp}`

Tokens are copied into a growable contiguous buffer instead of one allocator
call per token.  The buffer is grown by doubling and the previous contents are
preserved because the word map keeps pointers into it until the document is
finished.

### 2.6 Skip the redundant MATCH filter at runtime

File: `src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp`

For a natural-language `MATCH`, the inverted-index iterator already emits
exactly the documents whose score is positive.  The runtime filter produced by
the optimizer for `WHERE MATCH(...)` is `BOOL(MATCH(...))` (a `T_OP_BOOL` with
a single `T_FUN_MATCH_AGAINST` child), which therefore evaluates to true for
every row the iterator returns.

The retrieval engine detects that expression shape and passes a null filter to
the sparse retrieval iterator, skipping the per-row filter evaluation.
Relevance calculation and projection are untouched, so `ORDER BY MATCH(...)`
and score projection still work.  A score predicate such as
`MATCH(...) > 0.5` builds a comparison expression and keeps the filter;
boolean/phrase/ES modes are excluded by the mode check; the top-k block-max
path keeps the filter for pruning.

## 3. Expected effect per benchmark category

| Change | Build | Tokenize | Query |
| --- | --- | --- | --- |
| Shared dictionaries | yes | yes | yes |
| Allocation-free normalization | yes | yes | yes |
| UTF8MB4 classification | yes | yes | yes |
| Binary range search | yes | yes | yes |
| BENG bulk buffer | BENG build | BENG tokenize | - |
| Runtime MATCH filter skip | - | - | yes |

## 4. Correctness

The following checks were run against a release build of the modified tree and
against the unmodified baseline binary:

* `TOKENIZE()` output was compared for 15 inputs covering default/`ik`/`beng`/
  `ngram`/`space` parsers, pure Chinese, pure English, mixed text, upper case,
  full-width Latin/digits, empty strings and punctuation.  The two outputs are
  byte-identical.
* `MATCH ... AGAINST` hit counts for the four benchmark queries are identical
  (`8001`, `11000`, `7332`, `20`).
* Small end-to-end table with `WITH PARSER ik` and `WITH PARSER beng` indexes
  returns identical `COUNT(*)` results for Chinese, English and mixed queries.
* An extended matrix on the 20,000-document table was compared line by line
  against the baseline binary and is byte-identical: phrase search in boolean
  mode, `+term -term`, `WITH QUERY EXPANSION`, `MATCH(...) > 0.5`,
  `MATCH AND id > ...`, `MATCH OR MATCH`, `MATCH` in the select list with
  `ORDER BY`, `ORDER BY MATCH ... LIMIT`, `GROUP BY` over a MATCH subquery,
  `LIKE` combined with `MATCH`, and a prepared statement with a bound
  `AGAINST(?)` parameter.  This covers the cases that must keep the filter
  (boolean/phrase/expansion/score predicates) as well as the cases that skip
  it.

## 5. Measurements

Environment: Ubuntu 24.04 under WSL2, Intel Core Ultra 5 125H (18 threads),
10 GB RAM, 20,000 documents, release build (`RelWithDebInfo`, clang 17).
Timings are wall clock; CPU is the sum over all server threads.  Both variants
were measured back-to-back with the same data and server parameters.

| Phase | Baseline wall | Optimized wall | Δ wall | Baseline CPU | Optimized CPU | Δ CPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| build_ik_all | 15.581 s | 15.060 s | -3.3% | 24.07 s | 21.00 s | -12.8% |
| build_ik_content | 11.842 s | 11.059 s | -6.6% | 15.09 s | 13.32 s | -11.7% |
| build_beng_en | 6.105 s | 6.100 s | -0.1% | 7.06 s | 6.60 s | -6.5% |
| tokenize_ik ×3000 | 1.362 s | 1.238 s | -9.1% | - | - | - |
| tokenize_beng ×3000 | 1.073 s | 0.912 s | -15.0% | 0.75 s | 0.08 s | -89% |
| query_cn ×200 | 0.786 s | 0.736 s | -6.4% | 0.67 s | 0.70 s | +4% |
| query_beng ×200 | 1.002 s | 0.947 s | -5.5% | 0.89 s | 0.84 s | -6% |
| query_mixed ×200 | 0.952 s | 0.712 s | -25.2% | 0.87 s | 0.68 s | -22% |
| query_limit ×200 | 1.122 s | 0.962 s | -14.3% | 0.99 s | 0.85 s | -14% |

Paired benchmark runs (baseline and optimized, back-to-back, identical
configuration):

| Metric | Baseline | Optimized | Δ |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 15.068 | 13.705 | -9.0% |
| build_ik_content_sec | 10.621 | 10.138 | -4.5% |
| build_beng_en_sec | 6.003 | 5.495 | -8.5% |
| build_total_sec | 31.714 | 29.354 | -7.4% |
| tokenize_ik_avg_ms | 0.4676 | 0.3854 | -17.6% |
| tokenize_beng_avg_ms | 0.3413 | 0.1465 | -57.1%* |
| query_cn_avg_ms | 3.1135 | 2.9059 | -6.7% |
| query_beng_avg_ms | 4.8411 | 4.6593 | -3.8% |
| query_mixed_avg_ms | 3.5731 | 3.3686 | -5.7% |

Final run including the runtime filter skip (2.6) and the lock-free dictionary
fast path, same host and configuration: `build_total` 21.868 s,
`build_ik_all` 9.735 s, `build_ik_content` 7.600 s, `tokenize_ik` 0.3086 ms,
`query_cn` 1.9992 ms, `query_beng` 2.8765 ms, `query_limit` 3.0648 ms.
Publishing the shared dictionaries with an atomic release store instead of
taking `cached_dict_mutex_` on every fetch cut `build_total` from 28.9 s to
21.9 s and `tokenize_ik` from 0.391 ms to 0.309 ms on the same host, which
confirms that the exclusive lock was serialising parser initialisation during
parallel index build.
The `query_mixed` sample in the earlier run was invalidated by a backward clock
jump (one negative sample), so the 400-query A/B table below is the reliable
number for mixed queries.

\* The optimized `tokenize_beng` value is at the SQL-loop noise floor on this
host (the `SELECT 1` baseline in the same run was 0.3756 ms), so the BENG
tokenizer itself is effectively free; the percentage should not be read as a
precise speedup.

`query_limit` is omitted from the paired table: its 3-sample average varied
between 2.5 ms and 5.5 ms across runs (standard deviation larger than 2 ms) on
this host, so it is not a usable signal at this sample count.  The CPU
measurement above (which is less sensitive to host clock jumps) shows the same
downward trend for it.

The host used for these runs has a noisy clock and shared CPU with the Windows
host, so individual query samples vary by up to ±30%.  The CPU column is the
more stable measure of the work removed; it consistently shows a double-digit
reduction for index build and a 14–22% reduction for the scan-heavy queries.

Runtime filter-skip A/B (section 2.6; 400 queries per phase, 20,000 documents,
back-to-back on the same host):

| Query shape | Baseline | Filter skip | Δ |
| --- | ---: | ---: | ---: |
| two tokens, 8001 hits | 4.25 ms | 3.33 ms | -21.6% |
| mixed CN/EN, 7332 hits | 4.43 ms | 4.16 ms | -6.2% |
| one token, 4668 hits | 2.10 ms | 1.82 ms | -13.2% |
| no hits | 1.65 ms | 0.92 ms | -44.2% |

`perf` confirms the change takes effect: `ObSRDaaTIterImpl::filter_on_demand`
used to be 3.7% of query self time and is no longer present in the profile.

The full semantic corpus (section 4, 14 query shapes including boolean mode,
score projection, explicit relevance ordering and mixed AND/OR) produced a
**zero-line diff** against the baseline binary, and the benchmark hit counts
remain `8001 / 11000 / 7332 / 20`.

## 6. Rejected experiment

A planner-level change that skipped BM25 for filter-only MATCH predicates
(`SELECT COUNT(*) ... WHERE MATCH(...)`, where the score cannot affect the
result) was prototyped.  It correctly produced `calc_relevance=false` in the
plan, but the runtime iterator path stalled on the first COUNT query in this
version (the upstream code keeps an unfinished TODO there), so the change was
reverted and is **not** part of the submission.  Relevance calculation is left
enabled; the shipped query optimization (2.6) skips only the redundant filter
and keeps the original relevance execution path.

## 7. Files changed

```
src/storage/fts/ob_ik_ft_parser.cpp
src/storage/fts/ob_ik_ft_parser.h
src/storage/fts/dict/ob_ft_dict_hub.cpp
src/storage/fts/dict/ob_ft_dict_hub.h
src/storage/fts/dict/ob_ft_range_dict.cpp
src/storage/fts/ik/ob_ik_char_util.h
src/storage/fts/ob_beng_ft_parser.cpp
src/storage/fts/ob_beng_ft_parser.h
src/storage/fts/ob_fts_stop_word.cpp
src/storage/fts/ob_fts_stop_word.h
src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp
```

A unified diff against the competition baseline is available as
`fts_fulltext_optimization.patch` in the repository root.

## 8. Reproduction

```bash
# Build
./build.sh release --init -j 8

# Run the benchmark (20000 rows, 3 samples)
LABEL=after ./tools/benchmark/fts_large_bench.sh

# Score against the checked-in CI baseline
python3 tools/benchmark/fts_large_bench_score.py bench_report.txt
```

## 9. Build throughput notes (this host)

Incremental build time on the WSL2 host was dominated by header fan-out, not
by CPU throttling (a CPU burn measured 2.05 s wall / 2.02 s user):

* Touching a widely included header such as `ob_das_ir_define.h` recompiles
  roughly 20 SQL unity translation units, each taking minutes.  The shipped
  query change is intentionally confined to a single `.cpp`; rebuilding it
  plus the code-generator unity and link takes about 3–4 minutes.
* When a header is reverted to content that is already ABI-compatible with the
  existing objects, `touch -d '<old date>' <header>` prevents a pointless full
  recompilation.  This is only safe when the content/layout really is
  unchanged.
* The repository CMake build requires unity builds
  (`OB_ENABLE_UNITY`), so reducing per-TU size is not an option; minimizing
  header edits is the effective lever.
* For repeated edit/build cycles, wiring `ccache` as
  `CMAKE_CXX_COMPILER_LAUNCHER` and keeping the WSL distro warm (a persistent
  session, AC power/high-performance plan) removes most of the remaining
  latency.
