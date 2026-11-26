/*
 * Copyright (c) 2025-2026 Tiger Data, Inc.
 * Licensed under the PostgreSQL License. See LICENSE for details.
 *
 * bmw.c - Block-Max WAND and windowed MaxScore for top-k queries
 */
#include <postgres.h>

#include <float.h>
#include <math.h>
#include <miscadmin.h>
#include <port/pg_bitutils.h>
#include <utils/float.h>
#include <utils/hsearch.h>
#include <utils/memutils.h>

#include "constants.h"
#include "index/metapage.h"
#include "index/source.h"
#include "memtable/chain_source.h"
#include "scoring/bm25.h"
#include "scoring/bmw.h"
#include "segment/alive_bitset.h"
#include "segment/compression.h"
#include "segment/fieldnorm.h"
#include "segment/io.h"

/*
 * ------------------------------------------------------------
 * Top-K Min-Heap Implementation
 * ------------------------------------------------------------
 */

void
tp_topk_init(TpTopKHeap *heap, int k, MemoryContext ctx)
{
	MemoryContext old_ctx = MemoryContextSwitchTo(ctx);

	heap->ctids		 = palloc(k * sizeof(ItemPointerData));
	heap->seg_blocks = palloc(k * sizeof(BlockNumber));
	heap->doc_ids	 = palloc(k * sizeof(uint32));
	heap->scores	 = palloc(k * sizeof(float4));
	heap->capacity	 = k;
	heap->size		 = 0;

	MemoryContextSwitchTo(old_ctx);
}

void
tp_topk_free(TpTopKHeap *heap)
{
	if (heap->ctids)
	{
		pfree(heap->ctids);
		heap->ctids = NULL;
	}
	if (heap->seg_blocks)
	{
		pfree(heap->seg_blocks);
		heap->seg_blocks = NULL;
	}
	if (heap->doc_ids)
	{
		pfree(heap->doc_ids);
		heap->doc_ids = NULL;
	}
	if (heap->scores)
	{
		pfree(heap->scores);
		heap->scores = NULL;
	}
	heap->capacity = 0;
	heap->size	   = 0;
}

/*
 * Swap two heap entries.
 */
static inline void
heap_swap(TpTopKHeap *heap, int i, int j)
{
	ItemPointerData tmp_ctid	  = heap->ctids[i];
	BlockNumber		tmp_seg_block = heap->seg_blocks[i];
	uint32			tmp_doc_id	  = heap->doc_ids[i];
	float4			tmp_score	  = heap->scores[i];

	heap->ctids[i]		= heap->ctids[j];
	heap->seg_blocks[i] = heap->seg_blocks[j];
	heap->doc_ids[i]	= heap->doc_ids[j];
	heap->scores[i]		= heap->scores[j];

	heap->ctids[j]		= tmp_ctid;
	heap->seg_blocks[j] = tmp_seg_block;
	heap->doc_ids[j]	= tmp_doc_id;
	heap->scores[j]		= tmp_score;
}

/*
 * Compare two heap entries for min-heap ordering.
 * Returns true if entry at position a should be closer to the root
 * (i.e., is a weaker result that should be evicted first).
 *
 * Primary: lower score = weaker = closer to root.
 * Secondary: among equal scores, higher CTID = weaker (lower CTID wins
 * ties in the final output). For segment entries whose CTIDs are not
 * yet resolved, doc_id within the segment is a reliable proxy.
 */
static inline bool
heap_less(TpTopKHeap *heap, int a, int b)
{
	if (heap->scores[a] < heap->scores[b])
		return true;
	if (heap->scores[a] > heap->scores[b])
		return false;

	/* Equal scores: higher CTID is weaker (closer to root) */
	if (ItemPointerIsValid(&heap->ctids[a]) &&
		ItemPointerIsValid(&heap->ctids[b]))
		return ItemPointerCompare(&heap->ctids[a], &heap->ctids[b]) > 0;

	/* Segment entries: doc_id proxy (higher doc_id = weaker) */
	if (heap->doc_ids[a] != heap->doc_ids[b])
		return heap->doc_ids[a] > heap->doc_ids[b];
	return heap->seg_blocks[a] > heap->seg_blocks[b];
}

/*
 * Sift up: restore heap property after insertion at position i.
 * For min-heap: parent <= children (using heap_less for comparison).
 */
static void
heap_sift_up(TpTopKHeap *heap, int i)
{
	while (i > 0)
	{
		int parent = (i - 1) / 2;
		if (!heap_less(heap, i, parent))
			break;
		heap_swap(heap, i, parent);
		i = parent;
	}
}

/*
 * Sift down: restore heap property after replacement at position i.
 * For min-heap: parent <= children (using heap_less for comparison).
 */
static void
heap_sift_down(TpTopKHeap *heap, int i)
{
	while (true)
	{
		int left	 = 2 * i + 1;
		int right	 = 2 * i + 2;
		int smallest = i;

		if (left < heap->size && heap_less(heap, left, smallest))
			smallest = left;
		if (right < heap->size && heap_less(heap, right, smallest))
			smallest = right;

		if (smallest == i)
			break;

		heap_swap(heap, i, smallest);
		i = smallest;
	}
}

/*
 * Check if a candidate entry should replace the heap root.
 *
 * Higher score always wins. For equal scores, lower CTID wins
 * (matching the extraction sort order). When CTIDs are not both
 * available (memtable vs segment), doc_id serves as proxy.
 */
static inline bool
heap_beats_root(
		TpTopKHeap *heap, float4 score, ItemPointerData *ctid, uint32 doc_id)
{
	if (score > heap->scores[0])
		return true;
	if (score < heap->scores[0])
		return false;

	/* Equal score: lower CTID beats higher CTID */
	if (ctid && ItemPointerIsValid(ctid) &&
		ItemPointerIsValid(&heap->ctids[0]))
		return ItemPointerCompare(ctid, &heap->ctids[0]) < 0;

	/* Both segment entries: lower doc_id proxy */
	if (ctid == NULL && !ItemPointerIsValid(&heap->ctids[0]))
		return doc_id < heap->doc_ids[0];

	/* Mixed memtable/segment: can't compare, don't evict */
	return false;
}

/*
 * Add a memtable result to the top-k heap.
 * CTID is known immediately for memtable entries.
 */
void
tp_topk_add_memtable(TpTopKHeap *heap, ItemPointerData ctid, float4 score)
{
	if (heap->size < heap->capacity)
	{
		/* Heap not full - just add */
		int i				= heap->size++;
		heap->ctids[i]		= ctid;
		heap->seg_blocks[i] = InvalidBlockNumber; /* Marks as memtable entry */
		heap->doc_ids[i]	= 0;
		heap->scores[i]		= score;
		heap_sift_up(heap, i);
	}
	else if (heap_beats_root(heap, score, &ctid, 0))
	{
		/* Heap full but new entry beats root */
		heap->ctids[0]		= ctid;
		heap->seg_blocks[0] = InvalidBlockNumber;
		heap->doc_ids[0]	= 0;
		heap->scores[0]		= score;
		heap_sift_down(heap, 0);
	}
	/* else: doesn't qualify for top-k, ignore */
}

/*
 * Add a segment result to the top-k heap.
 * CTID resolution is deferred until extraction.
 */
void
tp_topk_add_segment(
		TpTopKHeap *heap, BlockNumber seg_block, uint32 doc_id, float4 score)
{
	if (heap->size < heap->capacity)
	{
		/* Heap not full - just add */
		int i = heap->size++;
		ItemPointerSetInvalid(&heap->ctids[i]); /* Will be resolved later */
		heap->seg_blocks[i] = seg_block;
		heap->doc_ids[i]	= doc_id;
		heap->scores[i]		= score;
		heap_sift_up(heap, i);
	}
	else if (heap_beats_root(heap, score, NULL, doc_id))
	{
		/* Heap full but new entry beats root */
		ItemPointerSetInvalid(&heap->ctids[0]);
		heap->seg_blocks[0] = seg_block;
		heap->doc_ids[0]	= doc_id;
		heap->scores[0]		= score;
		heap_sift_down(heap, 0);
	}
	/* else: doesn't qualify for top-k, ignore */
}

/*
 * Resolve CTIDs for segment results in the heap.
 * Batches lookups by segment - opens each unique segment once, resolves all
 * CTIDs from that segment, then closes it. This avoids O(k) segment opens.
 */
void
tp_topk_resolve_ctids(TpTopKHeap *heap, Relation index)
{
	int i, j;

	for (i = 0; i < heap->size; i++)
	{
		TpSegmentReader *reader;
		BlockNumber		 seg_block;

		/* Skip memtable entries and already-resolved entries */
		if (heap->seg_blocks[i] == InvalidBlockNumber)
			continue;

		seg_block = heap->seg_blocks[i];

		/* Open segment once for all entries from this segment */
		reader = tp_segment_open_ex(index, seg_block, false);
		if (reader == NULL)
			continue;

		/* Resolve all CTIDs from this segment */
		for (j = i; j < heap->size; j++)
		{
			if (heap->seg_blocks[j] == seg_block)
			{
				tp_segment_lookup_ctid(
						reader, heap->doc_ids[j], &heap->ctids[j]);
				/* Mark as resolved by setting seg_block to invalid */
				heap->seg_blocks[j] = InvalidBlockNumber;
			}
		}

		tp_segment_close(reader);
	}
}

/*
 * Compare function for qsort: sort by (score DESC, CTID ASC).
 * This matches the exhaustive path's tie-breaking for deterministic results.
 */
static int
compare_heap_entries(const void *a, const void *b, void *arg)
{
	int			i	 = *(const int *)a;
	int			j	 = *(const int *)b;
	TpTopKHeap *heap = (TpTopKHeap *)arg;
	int			cmp;

	/* Primary: higher score first (descending) */
	if (heap->scores[i] > heap->scores[j])
		return -1;
	if (heap->scores[i] < heap->scores[j])
		return 1;

	/* Secondary: lower CTID first (ascending) for deterministic tie-breaking
	 */
	cmp = ItemPointerCompare(&heap->ctids[i], &heap->ctids[j]);
	return cmp;
}

/*
 * Extract sorted results (descending by score, CTID tie-breaking).
 *
 * Note: Call tp_topk_resolve_ctids first if heap contains segment results.
 * This ensures CTIDs are available for tie-breaking to match exhaustive path.
 */
int
tp_topk_extract(TpTopKHeap *heap, ItemPointerData *ctids, float4 *scores)
{
	int	 count = heap->size;
	int *indices;
	int	 i;

	if (count == 0)
		return 0;

	/* Create index array for sorting */
	indices = palloc(count * sizeof(int));
	for (i = 0; i < count; i++)
		indices[i] = i;

	/* Sort indices by (score DESC, CTID ASC) */
	qsort_arg(indices, count, sizeof(int), compare_heap_entries, heap);

	/* Copy to output in sorted order */
	for (i = 0; i < count; i++)
	{
		int idx	  = indices[i];
		ctids[i]  = heap->ctids[idx];
		scores[i] = heap->scores[idx];
	}

	pfree(indices);
	heap->size = 0;

	return count;
}

/*
 * ------------------------------------------------------------
 * Block Max Score Computation
 * ------------------------------------------------------------
 */

/*
 * Score a term using the supplied frequency and BM25 length factor.
 */
static inline float4
compute_bm25_term_score(
		float4 idf, float4 tf, float4 k1_plus_one, float4 k1_len_norm)
{
	float4 tf_component = (tf * k1_plus_one) / (tf + k1_len_norm);

	return idf * tf_component;
}

float4
tp_compute_block_max_score(
		TpSkipEntry *skip, float4 idf, float4 k1, float4 b, float4 avg_doc_len)
{
	float4 tf = (float4)skip->block_max_tf;
	float4 dl = (float4)decode_fieldnorm(skip->block_max_norm);

	/* BM25 formula with max TF and min doc length in block */
	float4 len_norm = 1.0f - b + b * (dl / avg_doc_len);

	return compute_bm25_term_score(idf, tf, k1 + 1.0f, k1 * len_norm);
}

/*
 * Compute BM25 score for a single posting.
 */
static inline float4
compute_bm25_score(
		float4 idf,
		int32  tf,
		int32  doc_len,
		float4 k1,
		float4 b,
		float4 avg_doc_len)
{
	float4 len_norm = 1.0f - b + b * ((float4)doc_len / avg_doc_len);

	return compute_bm25_term_score(idf, (float4)tf, k1 + 1.0f, k1 * len_norm);
}

/*
 * ------------------------------------------------------------
 * Single-Term BMW Scoring
 * ------------------------------------------------------------
 */

/*
 * Score memtable postings for a single term.
 * Memtable has no skip index, so we score all postings exhaustively.
 *
 * Caller owns `source` (a TpMemtableChainSource over the index's
 * on-disk chain) and is responsible for closing it.  If `source`
 * is NULL we skip memtable scoring entirely.
 */
static void
score_memtable_single_term(
		TpTopKHeap	 *heap,
		TpDataSource *source,
		const char	 *term,
		float4		  idf,
		float4		  k1,
		float4		  b,
		float4		  avg_doc_len,
		TpBMWStats	 *stats,
		TpBMWFilter	 *filter)
{
	TpPostingData *postings;
	int			   i;

	if (!source)
		return;

	/* Get postings for this term */
	postings = tp_source_get_postings(source, term);
	if (!postings || postings->count == 0)
	{
		if (postings)
			tp_source_free_postings(source, postings);
		return;
	}

	/* Score each posting */
	for (i = 0; i < postings->count; i++)
	{
		ItemPointerData *ctid = &postings->ctids[i];
		int32			 tf	  = postings->frequencies[i];
		int32			 doc_len;
		float4			 score;

		if ((i & 0xFFF) == 0)
			CHECK_FOR_INTERRUPTS();

		/* Get document length */
		doc_len = tp_source_get_doc_length(source, ctid);
		if (doc_len <= 0)
			doc_len = 1; /* Fallback for missing entries */

		/* Quantize like segments so scores do not change on spill */
		doc_len = (int32)decode_fieldnorm(encode_fieldnorm((uint32)doc_len));

		score = compute_bm25_score(idf, tf, doc_len, k1, b, avg_doc_len);

		if (!tp_topk_dominated(heap, score) &&
			(filter == NULL ||
			 filter->memtable_matches(filter->state, source, ctid)))
			tp_topk_add_memtable(heap, *ctid, score);

		if (stats)
			stats->memtable_docs++;
	}

	tp_source_free_postings(source, postings);
}

/*
 * Score segment postings for a single term using BMW.
 */
static void
score_segment_single_term_bmw(
		TpTopKHeap		*heap,
		TpSegmentReader *reader,
		const char		*term,
		float4			 idf,
		float4			 k1,
		float4			 b,
		float4			 avg_doc_len,
		TpBMWStats		*stats,
		TpBMWFilter		*filter)
{
	TpSegmentPostingIterator iter;
	TpSegmentPosting		*posting;
	TpDictEntry				*dict_entry;
	uint32					 block_count;
	float4					*block_max_scores;
	uint32					*block_last_doc_ids = NULL;
	uint32					 i;
	bool drive_candidates = filter != NULL && filter->segment_next != NULL;

	/* Initialize iterator for this term */
	if (!tp_segment_posting_iterator_init(&iter, reader, term))
		return; /* Term not found in segment */
	iter.force_copy = filter != NULL;

	if (drive_candidates)
	{
		uint32 candidate;
		drive_candidates =
				filter->segment_next(filter->state, reader, 0, &candidate);
		if (drive_candidates && candidate == UINT32_MAX)
		{
			tp_segment_posting_iterator_free(&iter);
			return;
		}
	}

	/* Get dictionary entry for block count and skip index */
	dict_entry	= &iter.dict_entry;
	block_count = dict_entry->block_count;

	/* Pre-compute block max scores */
	block_max_scores = palloc(block_count * sizeof(float4));
	if (drive_candidates)
		block_last_doc_ids = palloc(block_count * sizeof(uint32));
	for (i = 0; i < block_count; i++)
	{
		TpSkipEntry skip;
		tp_segment_read_skip_entry(
				reader, dict_entry->skip_index_offset, i, &skip);
		block_max_scores[i] =
				tp_compute_block_max_score(&skip, idf, k1, b, avg_doc_len);
		if (block_last_doc_ids != NULL)
			block_last_doc_ids[i] = skip.last_doc_id;
	}

	/* Process blocks with BMW */
	for (i = 0; i < block_count; i++)
	{
		float4 threshold = tp_topk_threshold(heap);
		float4 block_max = block_max_scores[i];

		CHECK_FOR_INTERRUPTS();

		/* Skip block if it can't beat threshold */
		if (block_max < threshold)
		{
			if (stats)
				stats->blocks_skipped++;
			continue;
		}

		if (stats)
			stats->blocks_scanned++;

		/* Load and score this block */
		iter.current_block = i;
		iter.finished	   = false; /* Reset so we can process this block */
		tp_segment_posting_iterator_load_block(&iter);

		while (tp_segment_posting_iterator_next(&iter, &posting))
		{
			float4 score;

			/*
			 * Break if iterator auto-advanced to next block.
			 * This ensures we only process block i, allowing the outer
			 * for loop to apply threshold checks to subsequent blocks.
			 */
			if (iter.current_block != i)
				break;

			while (drive_candidates)
			{
				uint32 candidate;
				CHECK_FOR_INTERRUPTS();
				if (!filter->segment_next(
							filter->state,
							reader,
							posting->doc_id,
							&candidate))
				{
					drive_candidates = false;
					break;
				}
				if (candidate == UINT32_MAX)
					goto done;
				if (candidate <= posting->doc_id)
					break;
				if (candidate <= iter.skip_entry.last_doc_id)
				{
					do
					{
						if (!tp_segment_posting_iterator_next(&iter, &posting))
							goto done;
					} while (posting->doc_id < candidate);
					continue;
				}
				{
					uint32 target_block = i + 1;
					uint32 end			= block_count;

					while (target_block < end)
					{
						uint32 mid = target_block + (end - target_block) / 2;
						if (block_last_doc_ids[mid] < candidate)
							target_block = mid + 1;
						else
							end = mid;
					}
					if (stats)
						stats->blocks_skipped += target_block - i - 1;
					if (target_block == block_count)
						goto done;
					/* Apply the BMW gate before loading the target block. */
					if (block_max_scores[target_block] <
						tp_topk_threshold(heap))
					{
						i = target_block - 1;
						goto next_block;
					}
				}
				if (!tp_segment_posting_iterator_seek(
							&iter, candidate, &posting))
					goto done;
				/* Consume it unless seek's next-block fallback already did. */
				if (iter.current_in_block < iter.skip_entry.doc_count &&
					iter.block_postings[iter.current_in_block].doc_id ==
							posting->doc_id)
					iter.current_in_block++;
				i = iter.current_block;
				if (stats)
				{
					stats->blocks_scanned++;
					stats->seeks_performed++;
				}
			}

			/* Skip dead docs */
			if (!tp_segment_is_alive(reader, posting->doc_id))
			{
				if (stats)
					stats->dead_docs_skipped++;
				continue;
			}

			score = compute_bm25_score(
					idf,
					posting->frequency,
					posting->doc_length,
					k1,
					b,
					avg_doc_len);

			if (!tp_topk_dominated(heap, score) &&
				(filter == NULL ||
				 filter->segment_matches(
						 filter->state, reader, posting->doc_id)))
			{
				tp_topk_add_segment(
						heap, reader->root_block, posting->doc_id, score);
			}

			if (stats)
				stats->segment_docs_scored++;
		}
	next_block:;
	}

done:
	if (block_last_doc_ids != NULL)
		pfree(block_last_doc_ids);
	pfree(block_max_scores);
	tp_segment_posting_iterator_free(&iter);
}

int
tp_score_single_term_bmw(
		TpLocalIndexState			 *local_state,
		Relation					  index,
		const TpSegmentGraphSnapshot *snapshot,
		TpDataSource				 *memtable_src,
		const char					 *term,
		float4						  idf,
		float4						  k1,
		float4						  b,
		float4						  avg_doc_len,
		int							  max_results,
		ItemPointerData				 *result_ctids,
		float4						 *result_scores,
		TpBMWStats					 *stats,
		TpBMWFilter					 *filter)
{
	TpTopKHeap heap;
	int		   result_count;

	(void)local_state; /* reserved for future use */

	/* Initialize stats */
	if (stats)
		memset(stats, 0, sizeof(TpBMWStats));

	/* Initialize top-k heap */
	tp_topk_init(&heap, max_results, CurrentMemoryContext);

	/* Score memtable (exhaustive - no skip index) */
	score_memtable_single_term(
			&heap, memtable_src, term, idf, k1, b, avg_doc_len, stats, filter);
	if (filter != NULL)
		filter->reset(filter->state);

	/* Score each segment level with BMW */
	for (uint32 level = 0; level < TP_MAX_LEVELS; level++)
	{
		uint32			   root_count;
		const BlockNumber *roots =
				tp_segment_graph_snapshot_level(snapshot, level, &root_count);

		for (uint32 root_idx = 0; root_idx < root_count; root_idx++)
		{
			TpSegmentReader *reader = tp_segment_open(index, roots[root_idx]);

			CHECK_FOR_INTERRUPTS();

			score_segment_single_term_bmw(
					&heap,
					reader,
					term,
					idf,
					k1,
					b,
					avg_doc_len,
					stats,
					filter);
			if (filter != NULL)
				filter->reset(filter->state);

			tp_segment_close(reader);
		}
	}

	/* Resolve CTIDs for segment results before extraction */
	tp_topk_resolve_ctids(&heap, index);

	/* Extract sorted results */
	result_count = tp_topk_extract(&heap, result_ctids, result_scores);

	if (stats)
		stats->docs_in_results = result_count;

	return result_count;
}

/*
 * ------------------------------------------------------------
 * Multi-Term BMW Scoring
 * ------------------------------------------------------------
 */

/*
 * Per-term state for multi-term BMW scoring.
 */
typedef struct TpTermState
{
	const char *term;
	float4		idf;
	int32		query_freq; /* Query term frequency (for boosting) */

	/* Global maximum score across all blocks (for WAND pivot) */
	float4 max_score;

	/* Segment-specific state (reset per segment) */
	bool					 found; /* Term found in current segment */
	TpSegmentPostingIterator iter;	/* Iterator (contains dict_entry) */
	float4 *block_max_scores;		/* Pre-computed block max scores */
	uint32 *block_last_doc_ids;		/* Cached last_doc_id per block */
	uint32	cur_doc_id;				/* Cached current doc ID */
	/* MaxScore bound cursor, independent of posting iterator */
	uint32 bound_block_cursor;
} TpTermState;

/*
 * Refresh cached doc ID from iterator state.
 */
static inline void
refresh_cur_doc_id(TpTermState *ts)
{
	if (!ts->found || ts->iter.finished)
		ts->cur_doc_id = UINT32_MAX;
	else
		ts->cur_doc_id = tp_segment_posting_iterator_current_doc_id(&ts->iter);
}

/*
 * Get current doc ID for a term, or UINT32_MAX if exhausted.
 * Uses cached value maintained by advance/seek/init functions.
 */
static inline uint32
term_current_doc_id(TpTermState *ts)
{
	return ts->cur_doc_id;
}

/*
 * Score memtable postings for multiple terms.
 * Memtable has no skip index, so we score all postings exhaustively.
 */
static void
score_memtable_multi_term(
		TpTopKHeap	 *heap,
		TpDataSource *source,
		TpTermState **terms,
		int			  term_count,
		float4		  k1,
		float4		  b,
		float4		  avg_doc_len,
		TpBMWStats	 *stats,
		TpBMWFilter	 *filter)
{
	HTAB   *doc_accum;
	HASHCTL hash_ctl;
	int		term_idx;

	if (!source)
		return;

	/* Create hash table for document score accumulation */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize   = sizeof(ItemPointerData);
	hash_ctl.entrysize = sizeof(DocumentScoreEntry);
	hash_ctl.hcxt	   = CurrentMemoryContext;
	doc_accum		   = hash_create(
			 "Memtable Doc Accum", 1024, &hash_ctl, HASH_ELEM | HASH_BLOBS);

	/* Process each term */
	for (term_idx = 0; term_idx < term_count; term_idx++)
	{
		TpTermState	  *ts = terms[term_idx];
		TpPostingData *postings;
		int			   i;

		postings = tp_source_get_postings(source, ts->term);
		if (!postings || postings->count == 0)
		{
			if (postings)
				tp_source_free_postings(source, postings);
			continue;
		}

		/* Score each posting for this term */
		for (i = 0; i < postings->count; i++)
		{
			ItemPointerData	   *ctid = &postings->ctids[i];
			int32				tf	 = postings->frequencies[i];
			int32				doc_len;
			float4				term_score;
			DocumentScoreEntry *entry;
			bool				found;

			/* CHECK every 4096 postings; this loop can be very long
			 * when the memtable holds millions of postings -- gating
			 * amortizes the overhead. */
			if ((i & 0xFFF) == 0)
				CHECK_FOR_INTERRUPTS();

			/* Get document length */
			doc_len = tp_source_get_doc_length(source, ctid);
			if (doc_len <= 0)
				doc_len = 1;

			/* Quantize like segments so scores do not change on spill */
			doc_len = (int32)decode_fieldnorm(
					encode_fieldnorm((uint32)doc_len));

			/* Compute BM25 term contribution */
			term_score = compute_bm25_score(
								 ts->idf, tf, doc_len, k1, b, avg_doc_len) *
						 ts->query_freq;

			/* Accumulate in hash table */
			entry = (DocumentScoreEntry *)
					hash_search(doc_accum, ctid, HASH_ENTER, &found);

			if (!found)
			{
				entry->ctid	 = *ctid;
				entry->score = term_score;
			}
			else
			{
				entry->score += term_score;
			}
		}

		tp_source_free_postings(source, postings);
	}

	/* Add accumulated documents to heap */
	{
		HASH_SEQ_STATUS		seq;
		DocumentScoreEntry *entry;

		hash_seq_init(&seq, doc_accum);
		while ((entry = hash_seq_search(&seq)) != NULL)
		{
			float4 score = (float4)entry->score;

			if (!tp_topk_dominated(heap, score) &&
				(filter == NULL ||
				 filter->memtable_matches(
						 filter->state, source, &entry->ctid)))
				tp_topk_add_memtable(heap, entry->ctid, score);

			if (stats)
				stats->memtable_docs++;
		}
	}

	hash_destroy(doc_accum);
}

/*
 * Advance a term iterator to the next document.
 * Returns true if iterator is still active, false if exhausted.
 */
static bool
advance_term_iterator(TpTermState *ts)
{
	ts->iter.current_in_block++;

	if (ts->iter.current_in_block >= ts->iter.skip_entry.doc_count)
	{
		ts->iter.current_block++;
		if (ts->iter.current_block >= ts->iter.dict_entry.block_count)
		{
			ts->iter.finished = true;
			ts->cur_doc_id	  = UINT32_MAX;
			return false;
		}
		ts->iter.current_in_block = 0;
		if (!tp_segment_posting_iterator_load_block(&ts->iter))
		{
			ts->iter.finished = true;
			ts->cur_doc_id	  = UINT32_MAX;
			return false;
		}
	}
	ts->cur_doc_id = ts->iter.block_postings[ts->iter.current_in_block].doc_id;
	return true;
}

/*
 * Seek a term iterator to target doc ID using binary search on cached skip
 * data. Returns true if iterator is still active (positioned at doc >=
 * target), false if exhausted.
 *
 * Uses pre-loaded block_last_doc_ids for O(log blocks) in-memory binary
 * search, avoiding I/O during the search. Only loads the target block from
 * disk.
 *
 * Post-condition on `return true`: ts->cur_doc_id >= target_doc_id.
 *
 * Both the in-block linear scan and the binary-search-selected block can
 * exhaust without finding doc >= target if the cached skip data
 * (skip_entry.last_doc_id, block_last_doc_ids[]) is inconsistent with
 * on-disk block_postings -- a condition observed on MS MARCO under
 * concurrent-insert segment topology. In that case we keep loading
 * subsequent blocks until we find one whose linear scan succeeds, or we
 * exhaust the iterator. Without this guarantee, callers like seek_to_pivot
 * spin forever (gdb stack on the production hang showed
 * seek_to_pivot:1329 with cur_doc_id < pivot_doc_id after a `return true`
 * from this function).
 */
static bool
seek_term_to_doc(TpTermState *ts, uint32 target_doc_id)
{
	uint32 block_count;
	int	   left, right, mid;
	uint32 target_block;

	if (!ts->found || ts->iter.finished)
		return false;

	/*
	 * Fast path: target is in (or before) the current block, per the
	 * cached last_doc_id. Linear-scan from current_in_block.
	 *
	 * If the cached last_doc_id is accurate, the loop below will find a
	 * doc >= target before exhausting the block. If it is *not* accurate
	 * (i.e., last_doc_id >= target but no posting in the block is >=
	 * target), the loop falls through and the block-advancing loop below
	 * picks up where we left off.
	 */
	if (target_doc_id <= ts->iter.skip_entry.last_doc_id)
	{
		while (ts->iter.current_in_block < ts->iter.skip_entry.doc_count)
		{
			uint32 doc_id =
					ts->iter.block_postings[ts->iter.current_in_block].doc_id;
			if (doc_id >= target_doc_id)
			{
				ts->cur_doc_id = doc_id;
				return true;
			}
			ts->iter.current_in_block++;
		}
		/* Fall through: advance past current block. */
	}
	else
	{
		/*
		 * Target is past current block's cached last_doc_id. Binary
		 * search the per-term skip cache to find the first block whose
		 * last_doc_id >= target.
		 */
		block_count = ts->iter.dict_entry.block_count;
		left		= ts->iter.current_block + 1;
		right		= (int)block_count - 1;

		while (left < right)
		{
			mid = left + (right - left) / 2;
			if (ts->block_last_doc_ids[mid] < target_doc_id)
				left = mid + 1;
			else
				right = mid;
		}
		target_block = left;

		if (target_block >= block_count)
		{
			ts->iter.finished = true;
			ts->cur_doc_id	  = UINT32_MAX;
			return false;
		}

		/* Position at the start of the binary-search-selected block. */
		ts->iter.current_block	  = target_block;
		ts->iter.current_in_block = 0;
		if (!tp_segment_posting_iterator_load_block(&ts->iter))
		{
			ts->iter.finished = true;
			ts->cur_doc_id	  = UINT32_MAX;
			return false;
		}
	}

	/*
	 * Block-advancing scan loop. Keep loading subsequent blocks until
	 * we find a posting >= target_doc_id or exhaust the iterator.
	 *
	 * This loop is the durable post-condition guarantor: it terminates
	 * only by finding doc >= target (and returning true) or by
	 * exhausting all blocks (and returning false). It does not rely on
	 * cached skip data being consistent with block_postings -- if a
	 * block's actual postings are all < target despite a cached
	 * last_doc_id >= target, we simply move on to the next block.
	 *
	 * Termination: each iteration of the outer loop strictly increments
	 * current_block, bounded by dict_entry.block_count. The inner
	 * while-loop scans a single block, bounded by skip_entry.doc_count.
	 */
	for (;;)
	{
		/*
		 * Cancelability: each iteration of the outer loop calls
		 * tp_segment_posting_iterator_load_block, which performs disk
		 * I/O and possibly block decompression. Under the cache-
		 * inconsistency scenario this function defends against, the
		 * loop can iterate across many blocks. Without this CHECK,
		 * statement_timeout / pg_cancel_backend cannot abort the
		 * scan, and seek_to_pivot's CHECK_FOR_INTERRUPTS one level up
		 * never fires because we never return from this function.
		 */
		CHECK_FOR_INTERRUPTS();

		while (ts->iter.current_in_block < ts->iter.skip_entry.doc_count)
		{
			uint32 doc_id =
					ts->iter.block_postings[ts->iter.current_in_block].doc_id;
			if (doc_id >= target_doc_id)
			{
				ts->cur_doc_id = doc_id;
				return true;
			}
			ts->iter.current_in_block++;
		}

		ts->iter.current_block++;
		if (ts->iter.current_block >= ts->iter.dict_entry.block_count)
		{
			ts->iter.finished = true;
			ts->cur_doc_id	  = UINT32_MAX;
			return false;
		}
		ts->iter.current_in_block = 0;
		if (!tp_segment_posting_iterator_load_block(&ts->iter))
		{
			ts->iter.finished = true;
			ts->cur_doc_id	  = UINT32_MAX;
			return false;
		}
	}
}

/*
 * Initialize term states for a segment.
 * Returns count of active iterators (terms found in segment).
 * Terms array is sorted by doc ID after initialization.
 */
static int
init_segment_term_states(
		TpTermState	   **terms,
		int				 term_count,
		TpSegmentReader *reader,
		float4			 k1,
		float4			 b,
		float4			 avg_doc_len)
{
	int active_count = 0;
	int term_idx;

	for (term_idx = 0; term_idx < term_count; term_idx++)
	{
		TpTermState *ts = terms[term_idx];

		ts->found			   = false;
		ts->max_score		   = 0.0f;
		ts->cur_doc_id		   = UINT32_MAX;
		ts->block_max_scores   = NULL;
		ts->block_last_doc_ids = NULL;
		ts->bound_block_cursor = 0;

		if (!tp_segment_posting_iterator_init(&ts->iter, reader, ts->term))
			continue;

		ts->found = true;

		/* Pre-load skip entries for BMW threshold checks and fast seeking */
		if (ts->iter.dict_entry.block_count > 0)
		{
			uint32		 block_idx;
			uint32		 block_count = ts->iter.dict_entry.block_count;
			TpSkipEntry *skip_cache;

			ts->block_max_scores   = palloc(block_count * sizeof(float4));
			ts->block_last_doc_ids = palloc(block_count * sizeof(uint32));

			/* Allocate and populate skip entry cache for the iterator */
			skip_cache = palloc(block_count * sizeof(TpSkipEntry));

			for (block_idx = 0; block_idx < block_count; block_idx++)
			{
				tp_segment_read_skip_entry(
						reader,
						ts->iter.dict_entry.skip_index_offset,
						block_idx,
						&skip_cache[block_idx]);
				ts->block_max_scores[block_idx] = tp_compute_block_max_score(
						&skip_cache[block_idx], ts->idf, k1, b, avg_doc_len);
				ts->block_last_doc_ids[block_idx] =
						skip_cache[block_idx].last_doc_id;

				if (ts->block_max_scores[block_idx] > ts->max_score)
					ts->max_score = ts->block_max_scores[block_idx];
			}

			ts->max_score *= ts->query_freq;

			/* Set caches on iterator for load_block to use */
			ts->iter.cached_skip_entries  = skip_cache;
			ts->iter.compressed_buf_cache = palloc(
					TP_MAX_COMPRESSED_BLOCK_SIZE);
		}

		if (tp_segment_posting_iterator_load_block(&ts->iter))
		{
			refresh_cur_doc_id(ts);
			active_count++;
		}
	}

	return active_count;
}

/*
 * Free term state resources for a segment.
 */
static void
cleanup_segment_term_states(TpTermState **terms, int term_count)
{
	int term_idx;

	for (term_idx = 0; term_idx < term_count; term_idx++)
	{
		TpTermState *ts = terms[term_idx];

		/*
		 * Free BMW-owned caches before iterator_free (which NULLs
		 * the borrowed pointers but doesn't free them).
		 */
		if (ts->iter.cached_skip_entries)
			pfree(ts->iter.cached_skip_entries);
		if (ts->iter.compressed_buf_cache)
			pfree(ts->iter.compressed_buf_cache);

		if (ts->found)
			tp_segment_posting_iterator_free(&ts->iter);
		if (ts->block_max_scores)
			pfree(ts->block_max_scores);
		if (ts->block_last_doc_ids)
			pfree(ts->block_last_doc_ids);
	}
}

/*
 * Compare terms by current doc_id for initial sort.
 * Exhausted terms (UINT32_MAX) sort to the end.
 */
static int
compare_term_doc_id(const void *a, const void *b)
{
	TpTermState *const *pa = (TpTermState *const *)a;
	TpTermState *const *pb = (TpTermState *const *)b;
	uint32				da = term_current_doc_id(*pa);
	uint32				db = term_current_doc_id(*pb);

	if (da < db)
		return -1;
	if (da > db)
		return 1;
	return 0;
}

/*
 * Sort term states by current doc_id.
 * Used once after init_segment_term_states.
 */
static void
sort_terms_by_doc_id(TpTermState **terms, int term_count)
{
	qsort(terms, term_count, sizeof(TpTermState *), compare_term_doc_id);
}

/*
 * Restore sorted order after term at position 'ord' advanced.
 * The term's doc_id increased, so it may need to move right.
 * Uses linear insertion -- O(1) typical, O(T) worst case.
 */
static void
restore_ordering(TpTermState **terms, int term_count, int ord)
{
	TpTermState *tmp;
	uint32		 doc_id = term_current_doc_id(terms[ord]);
	int			 i;

	for (i = ord + 1; i < term_count; i++)
	{
		if (term_current_doc_id(terms[i]) >= doc_id)
			break;
	}

	/* Term needs to move from ord to i-1 */
	if (i > ord + 1)
	{
		tmp = terms[ord];
		memmove(&terms[ord],
				&terms[ord + 1],
				(i - ord - 1) * sizeof(TpTermState *));
		terms[i - 1] = tmp;
	}
}

/*
 * WAND pivot selection.
 *
 * Given terms sorted by current doc_id, walk from lowest doc_id
 * accumulating each term's max_score. When the sum exceeds
 * threshold, we've found the pivot.
 *
 * Returns true if a pivot was found, false if no term combination
 * can beat threshold. Sets *pivot_len_out to the number of terms
 * participating (terms[0..pivot_len-1]) and *pivot_doc_id_out to
 * the pivot document.
 */
static bool
find_wand_pivot(
		TpTermState **terms,
		int			  term_count,
		float4		  threshold,
		int			 *pivot_len_out,
		uint32		 *pivot_doc_id_out)
{
	double accumulated = 0.0;
	int	   i;

	for (i = 0; i < term_count; i++)
	{
		uint32 doc_id = term_current_doc_id(terms[i]);
		if (doc_id == UINT32_MAX)
			break; /* No more active terms */

		accumulated += terms[i]->max_score;
		if (accumulated > (double)threshold)
		{
			/*
			 * Found pivot: terms[i] is the pivot term.
			 * Include any subsequent terms at the same doc_id.
			 */
			uint32 pivot_doc = doc_id;
			int	   pivot_len = i + 1;

			while (pivot_len < term_count &&
				   term_current_doc_id(terms[pivot_len]) == pivot_doc)
				pivot_len++;

			*pivot_len_out	  = pivot_len;
			*pivot_doc_id_out = pivot_doc;
			return true;
		}
	}

	return false; /* Can't beat threshold */
}

/*
 * Compute block-max score upper bound at pivot.
 *
 * After WAND pivot selection using global max_scores, refine
 * with actual block-level upper bounds. Only considers terms
 * [0..pivot_len-1] since they're the only ones at or before
 * the pivot.
 */
static double
compute_block_max_at_pivot(TpTermState **terms, int pivot_len)
{
	double upper_bound = 0.0;
	int	   i;

	for (i = 0; i < pivot_len; i++)
	{
		TpTermState *ts = terms[i];
		uint32		 block;

		if (!ts->found || ts->iter.finished)
			continue;
		if (ts->block_max_scores == NULL)
			continue;

		block = ts->iter.current_block;
		if (block < ts->iter.dict_entry.block_count)
			upper_bound += ts->block_max_scores[block] * ts->query_freq;
	}

	return upper_bound;
}

/*
 * When block-max upper bound < threshold, advance one scorer.
 *
 * We pick the pivot term whose current block ends *soonest*
 * (min_block_end), because seeking that term to min_block_end+1 is
 * guaranteed to advance it past its current block boundary, which
 * guarantees forward progress of the outer WAND loop.
 *
 * A previous version picked the pivot term with the highest max_score
 * and seeked it to min_block_end+1. That term's cur_doc_id could be
 * greater than min_block_end+1, in which case seek_term_to_doc was a
 * no-op and the outer WAND loop would spin forever (issue #355).
 * High-max-score selection is a performance heuristic; forward progress
 * is the correctness requirement.
 */
static void
block_max_skip_advance(
		TpTermState **terms,
		int			  term_count,
		int			  pivot_len,
		int			 *active_count,
		TpBMWStats	 *stats)
{
	int	   seek_term_idx = -1;
	uint32 min_block_end = UINT32_MAX;
	uint32 seek_target;
	int	   i;

	/* Find pivot term whose current block ends soonest. */
	for (i = 0; i < pivot_len; i++)
	{
		TpTermState *ts = terms[i];
		uint32		 doc_id;
		uint32		 block;
		uint32		 block_last;

		doc_id = term_current_doc_id(ts);
		if (doc_id == UINT32_MAX)
			continue;

		block = ts->iter.current_block;
		if (ts->block_last_doc_ids != NULL &&
			block < ts->iter.dict_entry.block_count)
		{
			block_last = ts->block_last_doc_ids[block];
			if (block_last < min_block_end)
			{
				min_block_end = block_last;
				seek_term_idx = i;
			}
		}
		else if (seek_term_idx < 0)
		{
			/*
			 * Fallback: term has no cached block_last_doc_ids. Pick
			 * the first such term so we still make progress.
			 */
			seek_term_idx = i;
		}
	}

	if (seek_term_idx < 0)
		return;

	/* Seek target: past the chosen term's current block. */
	if (min_block_end == UINT32_MAX)
		seek_target = term_current_doc_id(terms[seek_term_idx]) + 1;
	else
		seek_target = min_block_end + 1;

	/* Don't skip past the first non-pivot term */
	if (pivot_len < term_count)
	{
		uint32 next_doc = term_current_doc_id(terms[pivot_len]);
		if (next_doc < seek_target)
			seek_target = next_doc;
	}

	/*
	 * Defense-in-depth: seek_term_to_doc(target <= cur_doc_id) is a
	 * no-op and would leave us in an infinite loop. The selection
	 * above guarantees forward progress in the normal case (the
	 * min-block-end term is by construction at cur_doc_id <=
	 * min_block_end < seek_target), but the non-pivot cap above can
	 * pull seek_target back to next_doc, which can equal the chosen
	 * term's cur_doc_id when pivot_doc_id and next_doc coincide on a
	 * tie boundary. In that edge case force a single posting
	 * advance to guarantee progress.
	 */
	if (seek_target <= term_current_doc_id(terms[seek_term_idx]))
	{
		if (!advance_term_iterator(terms[seek_term_idx]))
			(*active_count)--;
		if (stats)
			stats->blocks_skipped++;
	}
	else
	{
		if (!seek_term_to_doc(terms[seek_term_idx], seek_target))
			(*active_count)--;
		if (stats)
		{
			stats->blocks_skipped++;
			stats->seeks_performed++;
		}
	}

	restore_ordering(terms, term_count, seek_term_idx);
}

/*
 * Seek pre-pivot terms to pivot_doc_id.
 * Returns true if all terms are still active, false if a term
 * was exhausted (caller should re-pivot).
 */
static bool
seek_to_pivot(
		TpTermState **terms,
		int			  term_count,
		int			  pivot_len,
		uint32		  pivot_doc_id,
		int			 *active_count)
{
	int i;

	for (i = 0; i < pivot_len; i++)
	{
		uint32 doc_id;

		/*
		 * Cancelability: the for loop's `i--; continue;` re-entry
		 * pattern, combined with restore_ordering rotating terms
		 * through slot `i`, was the gdb-observed spin site for the
		 * MS MARCO bucket-8 hang. The underlying cause (seek_term_to_doc
		 * returning true without advancing past target) is now fixed at
		 * the source, but keep this CHECK so any future regression in
		 * BMW invariants is interruptible from SQL rather than
		 * requiring SIGKILL. (The outer WAND loop's CFI is too coarse:
		 * if seek_to_pivot fails to terminate it never returns there.)
		 */
		CHECK_FOR_INTERRUPTS();

		doc_id = term_current_doc_id(terms[i]);

		if (doc_id == UINT32_MAX)
		{
			(*active_count)--;
			return false;
		}

		if (doc_id < pivot_doc_id)
		{
			if (!seek_term_to_doc(terms[i], pivot_doc_id))
			{
				(*active_count)--;
				restore_ordering(terms, term_count, i);
				return false;
			}
			restore_ordering(terms, term_count, i);
			i--; /* Re-check: a new term slid into position i */
			continue;
		}
	}

	return true;
}

/*
 * Verify all pivot terms are aligned at pivot_doc_id.
 * Seeks may have overshot; if any term is past pivot, return false
 * so the caller can re-pivot.
 */
static bool
verify_pivot_alignment(TpTermState **terms, int pivot_len, uint32 pivot_doc_id)
{
	int i;

	for (i = 0; i < pivot_len; i++)
	{
		if (term_current_doc_id(terms[i]) != pivot_doc_id)
			return false;
	}
	return true;
}

/*
 * Score pivot document by accumulating BM25 contributions from
 * all confirmed pivot terms.
 *
 * Only called after verify_pivot_alignment confirms all pivot
 * terms are positioned at pivot_doc_id, so each term's current
 * posting is guaranteed to be the pivot document.
 */
static float4
score_pivot_document(
		TpTermState **terms,
		int			  pivot_len,
		float4		  k1,
		float4		  b,
		float4		  avg_doc_len)
{
	/* Match the MaxScore and memtable rounding at the heap boundary. */
	double doc_score = 0.0;
	int	   i;

	for (i = 0; i < pivot_len; i++)
	{
		TpTermState	   *ts = terms[i];
		TpBlockPosting *bp;
		float4			term_score;

		if (!ts->found || ts->iter.finished)
			continue;

		bp		   = &ts->iter.block_postings[ts->iter.current_in_block];
		term_score = compute_bm25_score(
							 ts->idf,
							 bp->frequency,
							 (int32)decode_fieldnorm(bp->fieldnorm),
							 k1,
							 b,
							 avg_doc_len) *
					 ts->query_freq;
		doc_score += term_score;
	}

	return (float4)doc_score;
}

/*
 * Windowed exact MaxScore: metadata bounds and posting cursors are
 * independent.
 */
#define TP_MAXSCORE_BOUND_WINDOW 8192
#define TP_MAXSCORE_BATCH		 4096
#define TP_MAXSCORE_MIN_POSTINGS 256
#define TP_MAXSCORE_DENSITY_DENOMINATOR \
	256 /* At least one posting per 256 docs */

typedef struct TpMaxScoreParams
{
	bool   initialized;
	float4 k1_plus_one;
	float4 k1_len_norm[256];
} TpMaxScoreParams;

static void
init_maxscore_params(
		TpMaxScoreParams *params, float4 k1, float4 b, float4 avg_doc_len)
{
	params->k1_plus_one = k1 + 1.0f;
	for (int i = 0; i < 256; i++)
	{
		float4 doc_len		   = (float4)decode_fieldnorm((uint8)i);
		float4 len_norm		   = 1.0f - b + b * (doc_len / avg_doc_len);
		params->k1_len_norm[i] = k1 * len_norm;
	}
	params->initialized = true;
}

typedef struct TpMaxScoreTerm
{
	TpTermState *ts;
	double		 bound;
	double		 remaining;
	double		 priority;
} TpMaxScoreTerm;

typedef struct TpMaxScoreMatch
{
	uint32 doc;
	double score;
} TpMaxScoreMatch;

static int
compare_maxscore_priority(const void *a, const void *b)
{
	const TpMaxScoreTerm *ta = a;
	const TpMaxScoreTerm *tb = b;
	return float8_cmp_internal(ta->priority, tb->priority);
}

/*
 * Bounds are per-term float4 upper bounds. The caller retains its existing
 * conservative inflation, covering double summation and final float4 rounding.
 * Keep partial sums double here; narrowing before pruning restores the
 * order-sensitive multi-term rounding this path is intended to avoid.
 * Strict rejection keeps candidates tied with the threshold.
 */
static inline bool
maxscore_rejected(
		double partial, double remaining, double rounding, float4 threshold)
{
	if (unlikely(
				!(rounding >= 1.0 && rounding < HUGE_VAL && partial >= 0.0 &&
				  remaining >= 0.0 && isfinite(threshold))))
		return false;
	return (partial + remaining) * rounding < (double)threshold;
}

/*
 * Keep bitmap extraction's small live set separate from the much larger
 * traversal loop. Word-relative pointers avoid rebuilding a global offset
 * and indexing the output array for every surviving candidate.
 */
static pg_noinline int
maxscore_extract_matches(
		TpSegmentReader *reader,
		double			*scores,
		uint64			*touched,
		uint32			 word_count,
		uint32			 base,
		TpMaxScoreMatch *matches,
		double			 weak_bound,
		double			 rounding,
		float4			 threshold)
{
	uint64			*bitmap		 = touched;
	uint64			*bitmap_end	 = touched + word_count;
	double			*word_scores = scores;
	uint32			 doc_base	 = base;
	TpMaxScoreMatch *write		 = matches;

	while (bitmap < bitmap_end)
	{
		uint64 bits = *bitmap;
		*bitmap++	= 0;
		while (bits)
		{
			int	   bit	 = pg_rightmost_one_pos64(bits);
			double score = word_scores[bit];
			bits &= bits - 1;
			word_scores[bit] = 0.0;
			if (!maxscore_rejected(score, weak_bound, rounding, threshold))
			{
				uint32 doc = doc_base + bit;
				if (tp_segment_is_alive(reader, doc))
				{
					write->doc	 = doc;
					write->score = score;
					write++;
				}
			}
		}
		word_scores += 64;
		doc_base += 64;
	}
	return (int)(write - matches);
}

/*
 * Scalar block enumeration caches immutable score parameters across posting
 * iteration. Preserve every original float4 operation and its order.
 */
static inline float4
maxscore_posting_contribution(
		float4				  idf,
		float4				  k1_plus_one,
		const float4		 *norms,
		float4				  query_freq,
		const TpBlockPosting *bp)
{
	float4 score = compute_bm25_term_score(
			idf, (float4)bp->frequency, k1_plus_one, norms[bp->fieldnorm]);

	return score * query_freq;
}

static inline float4
maxscore_term_contribution(TpTermState *ts, const TpMaxScoreParams *params)
{
	return maxscore_posting_contribution(
			ts->idf,
			params->k1_plus_one,
			params->k1_len_norm,
			ts->query_freq,
			&ts->iter.block_postings[ts->iter.current_in_block]);
}

static pg_noinline void
maxscore_out_of_order_error(TpSegmentReader *reader)
{
	const char *index_name = reader && reader->index
									 ? RelationGetRelationName(reader->index)
									 : "<unknown>";

	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("out-of-order posting in index \"%s\"", index_name),
			 errhint("Rebuild the index with REINDEX.")));
}

/*
 * The chunk consumed postings [old_index,index). At a batch boundary retain
 * index's unconsumed posting. At block end let the original advance helper
 * load the next block/exhaust once, starting from the last consumed posting.
 */
static inline void
maxscore_finish_chunk(TpTermState *ts, uint32 index, uint32 block_count)
{
	Assert(block_count > 0 && index <= block_count);
	if (index == block_count)
	{
		ts->iter.current_in_block = block_count - 1;
		advance_term_iterator(ts);
	}
	else
	{
		ts->iter.current_in_block = index;
		ts->cur_doc_id			  = ts->iter.block_postings[index].doc_id;
	}
}

/*
 * Single essential list: scan ascending postings directly into matches[].
 */
static pg_noinline int
maxscore_one_essential_matches(
		TpSegmentReader		   *reader,
		TpTermState			   *ts,
		const TpMaxScoreParams *params,
		uint32					base,
		uint32					batch_end,
		TpMaxScoreMatch		   *matches,
		double					weak_bound,
		double					rounding,
		float4					threshold,
		TpBMWStats			   *stats)
{
	const float4  idf			= ts->idf;
	const float4  k1_plus_one	= params->k1_plus_one;
	const float4  query_freq	= (float4)ts->query_freq;
	const float4 *norms			= params->k1_len_norm;
	int			  matches_count = 0;
	uint32		  min_doc		= base;

	Assert(batch_end >= base && batch_end - base <= TP_MAXSCORE_BATCH);

	while (term_current_doc_id(ts) < batch_end)
	{
		const TpBlockPosting *postings	  = ts->iter.block_postings;
		uint32				  index		  = ts->iter.current_in_block;
		uint32				  block_count = ts->iter.skip_entry.doc_count;
		uint32				  start_index = index;

		CHECK_FOR_INTERRUPTS();
		if (stats && index == 0)
			stats->blocks_scanned++;
		while (index < block_count && postings[index].doc_id < batch_end)
		{
			const TpBlockPosting *bp	= &postings[index];
			uint32				  doc	= bp->doc_id;
			double				  score = 0.0;

			if (unlikely(doc < min_doc))
				maxscore_out_of_order_error(reader);
			min_doc = doc + 1;

			score += maxscore_posting_contribution(
					idf, k1_plus_one, norms, query_freq, bp);
			if (!maxscore_rejected(score, weak_bound, rounding, threshold) &&
				tp_segment_is_alive(reader, doc))
			{
				matches[matches_count].doc	   = doc;
				matches[matches_count++].score = score;
			}
			index++;
		}
		if (unlikely(index == start_index))
			maxscore_out_of_order_error(reader);
		maxscore_finish_chunk(ts, index, block_count);
	}
	return matches_count;
}

/*
 * Exactly two essential lists can emit their ascending union directly.
 * Local block positions survive loading the other iterator because decoded
 * outputs are owned copies and raw direct inputs have independent pins.
 */
static pg_noinline int
maxscore_two_essential_matches(
		TpSegmentReader		   *reader,
		TpTermState			   *a,
		TpTermState			   *b,
		const TpMaxScoreParams *params,
		uint32					base,
		uint32					batch_end,
		TpMaxScoreMatch		   *matches,
		double					weak_bound,
		double					rounding,
		float4					threshold,
		TpBMWStats			   *stats)
{
	const TpBlockPosting *ap	  = a->iter.block_postings;
	const TpBlockPosting *bp	  = b->iter.block_postings;
	uint32				  ai	  = a->iter.current_in_block;
	uint32				  bi	  = b->iter.current_in_block;
	uint32				  an	  = a->iter.skip_entry.doc_count;
	uint32				  bn	  = b->iter.skip_entry.doc_count;
	const float4		  aidf	  = a->idf;
	const float4		  ak1	  = params->k1_plus_one;
	const float4		  aqf	  = (float4)a->query_freq;
	const float4		 *anorms  = params->k1_len_norm;
	const float4		  bidf	  = b->idf;
	const float4		  bk1	  = params->k1_plus_one;
	const float4		  bqf	  = (float4)b->query_freq;
	const float4		 *bnorms  = params->k1_len_norm;
	TpMaxScoreMatch		 *write	  = matches;
	uint32				  min_doc = base;

	Assert(batch_end >= base && batch_end - base <= TP_MAXSCORE_BATCH);

	if (a->iter.finished)
		an = 0;
	if (b->iter.finished)
		bn = 0;
	for (;;)
	{
		bool   active_a	 = ai < an && ap[ai].doc_id < batch_end;
		bool   active_b	 = bi < bn && bp[bi].doc_id < batch_end;
		uint32 chunk_end = batch_end;
		uint32 prev_ai	 = ai;
		uint32 prev_bi	 = bi;

		if (!active_a && !active_b)
			break;
		CHECK_FOR_INTERRUPTS();
		if (active_a)
		{
			uint32 last_a = ap[an - 1].doc_id;
			if (unlikely(last_a < ap[ai].doc_id))
				maxscore_out_of_order_error(reader);
			if (last_a < chunk_end)
				chunk_end = last_a + 1;
		}
		if (active_b)
		{
			uint32 last_b = bp[bn - 1].doc_id;
			if (unlikely(last_b < bp[bi].doc_id))
				maxscore_out_of_order_error(reader);
			if (last_b < chunk_end)
				chunk_end = last_b + 1;
		}

		for (;;)
		{
			uint32 adoc	 = active_a && ai < an ? ap[ai].doc_id : UINT32_MAX;
			uint32 bdoc	 = active_b && bi < bn ? bp[bi].doc_id : UINT32_MAX;
			uint32 doc	 = Min(adoc, bdoc);
			double score = 0.0;

			if (doc >= chunk_end)
				break;
			if (unlikely(doc < min_doc))
				maxscore_out_of_order_error(reader);
			min_doc = doc + 1;

			/* Keep the generic dense path's state order and float4 terms. */
			if (adoc == doc)
			{
				if (stats && ai == 0)
					stats->blocks_scanned++;
				score += maxscore_posting_contribution(
						aidf, ak1, anorms, aqf, &ap[ai++]);
			}
			if (bdoc == doc)
			{
				if (stats && bi == 0)
					stats->blocks_scanned++;
				score += maxscore_posting_contribution(
						bidf, bk1, bnorms, bqf, &bp[bi++]);
			}
			if (!maxscore_rejected(score, weak_bound, rounding, threshold) &&
				tp_segment_is_alive(reader, doc))
			{
				write->doc	 = doc;
				write->score = score;
				write++;
			}
		}

		if (unlikely(ai == prev_ai && bi == prev_bi))
			maxscore_out_of_order_error(reader);

		if (active_a && ai == an)
		{
			maxscore_finish_chunk(a, ai, an);
			ai = a->iter.current_in_block;
			an = a->iter.finished ? 0 : a->iter.skip_entry.doc_count;
			ap = a->iter.block_postings;
		}
		if (active_b && bi == bn)
		{
			maxscore_finish_chunk(b, bi, bn);
			bi = b->iter.current_in_block;
			bn = b->iter.finished ? 0 : b->iter.skip_entry.doc_count;
			bp = b->iter.block_postings;
		}
	}
	/* Remaining local positions stop at the batch boundary, not block end. */
	if (!a->iter.finished)
		maxscore_finish_chunk(a, ai, an);
	if (!b->iter.finished)
		maxscore_finish_chunk(b, bi, bn);
	return (int)(write - matches);
}

/*
 * Three or more essential lists: accumulate into dense batch bitmap/scores.
 */
static pg_noinline int
maxscore_bitmap_essential_matches(
		TpSegmentReader		   *reader,
		TpMaxScoreTerm		   *states,
		int						weak_count,
		int						count,
		const TpMaxScoreParams *params,
		uint32					base,
		uint32					batch_end,
		double				   *scores,
		uint64				   *touched,
		TpMaxScoreMatch		   *matches,
		double					weak_bound,
		double					rounding,
		float4					threshold,
		TpBMWStats			   *stats)
{
	uint32 word_count;
	int	   i;

	Assert(batch_end >= base && batch_end - base <= TP_MAXSCORE_BATCH);

	for (i = weak_count; i < count; i++)
	{
		TpTermState	 *ts		  = states[i].ts;
		const float4  idf		  = ts->idf;
		const float4  k1_plus_one = params->k1_plus_one;
		const float4  query_freq  = (float4)ts->query_freq;
		const float4 *norms		  = params->k1_len_norm;
		uint32		  min_doc	  = base;

		while (term_current_doc_id(ts) < batch_end)
		{
			const TpBlockPosting *postings	  = ts->iter.block_postings;
			uint32				  index		  = ts->iter.current_in_block;
			uint32				  block_count = ts->iter.skip_entry.doc_count;
			uint32				  start_index = index;

			CHECK_FOR_INTERRUPTS();
			if (stats && index == 0)
				stats->blocks_scanned++;
			while (index < block_count && postings[index].doc_id < batch_end)
			{
				const TpBlockPosting *bp  = &postings[index];
				uint32				  doc = bp->doc_id;
				uint32				  offset;

				if (unlikely(doc < min_doc))
					maxscore_out_of_order_error(reader);
				min_doc = doc + 1;
				offset	= doc - base;
				touched[offset / 64] |= UINT64_C(1) << (offset % 64);
				scores[offset] += maxscore_posting_contribution(
						idf, k1_plus_one, norms, query_freq, bp);
				index++;
			}
			if (unlikely(index == start_index))
				maxscore_out_of_order_error(reader);
			maxscore_finish_chunk(ts, index, block_count);
		}
	}
	word_count = (batch_end - base + 63) / 64;
	return maxscore_extract_matches(
			reader,
			scores,
			touched,
			word_count,
			base,
			matches,
			weak_bound,
			rounding,
			threshold);
}

/*
 * A NaN in a Max() reduction can discard an earlier finite maximum.
 * A zero or subnormal block score can also reflect underflow. Treat these
 * inputs as unbounded. Callers represent empty posting ranges separately.
 */
static inline float4
maxscore_max_bound(float4 largest, float4 value)
{
	if (unlikely(!(value >= FLT_MIN)))
		return get_float4_infinity();
	return Max(largest, value);
}

/*
 * For fixed k1+1, 1-b and decoded length / avgdl, each positive normal
 * term score has at most seven rounded operations: two to form len_norm,
 * one to multiply by k1, then numerator, denominator, division and IDF.
 * Fusing a multiply-add removes a rounding. The worst ratio between a
 * posting and its computed upper bound is ((1+u)/(1-u))^7 < 1+15u for
 * u=2^-24. Sixteen upward float4 steps cover this error. A subnormal
 * b*(length/avgdl) has negligible absolute error beside 1-b in the
 * supported length range. Multiplying both scores by the same positive
 * query frequency preserves their order.
 */
static inline float4
conservative_score_bound(float4 bound)
{
	uint32 bits;

	if (!(bound >= FLT_MIN) || !isfinite(bound))
		return get_float4_infinity();
	memcpy(&bits, &bound, sizeof(bits));
	bits = Min(bits + 16, UINT32_C(0x7f800000));
	memcpy(&bound, &bits, sizeof(bound));
	return bound;
}

/*
 * Return a maximum over blocks covering [start,target), together with the
 * end of the already-covered range. All terms use the same nominal target;
 * the caller extends its window only through their common coverage.
 */
static double
maxscore_window_bound(
		TpTermState *ts,
		uint32		 start,
		uint32		 target,
		uint32		 segment_end,
		uint32		*covered_end,
		double		*priority)
{
	uint32 left		   = Max(ts->iter.current_block, ts->bound_block_cursor);
	uint32 block_count = ts->iter.dict_entry.block_count;
	float4 bound	   = 0.0f;
	uint32 first_block;

	*priority = 0.0;

	/* Advancing the physical cursor also consumes all preceding blocks. */
	ts->bound_block_cursor = left;
	/*
	 * A zero bound is valid only before this term's next posting. It does
	 * not scan/consume metadata, so retain the prior safe cursor.
	 */
	if (term_current_doc_id(ts) >= target)
	{
		*covered_end = Min(term_current_doc_id(ts), segment_end);
		return 0.0;
	}
	/*
	 * Prior scans stopped at the block covering old target. Every block
	 * before it ended before old target <= this window's start.
	 */
	while (left < block_count && ts->block_last_doc_ids[left] < start)
		left++;
	first_block	 = left;
	*covered_end = segment_end;
	for (; left < block_count; left++)
	{
		bound = maxscore_max_bound(bound, ts->block_max_scores[left]);
		if (ts->block_last_doc_ids[left] >= target - 1)
		{
			*covered_end = (uint32)
					Min((uint64)segment_end,
						(uint64)ts->block_last_doc_ids[left] + 1);
			break;
		}
	}
	/*
	 * Retain, rather than consume, the last covering block: a sparse
	 * block can span the next window as well. block_count is a sentinel.
	 */
	ts->bound_block_cursor = left;
	if (first_block == block_count)
		return 0.0;

	bound = conservative_score_bound(bound) * ts->query_freq;
	if (first_block < block_count)
	{
		uint32 last_block	  = Min(left, block_count - 1);
		uint32 covered_blocks = last_block - first_block + 1;
		uint64 span_start =
				first_block == 0
						? 0
						: (uint64)ts->block_last_doc_ids[first_block - 1] + 1;
		uint64 span_end = (uint64)ts->block_last_doc_ids[last_block] + 1;

		Assert(covered_blocks > 0 && span_end > span_start);

		/*
		 * Estimate the local posting density over complete covered blocks,
		 * not over the nominal window: sparse blocks can span far beyond it.
		 * 128/block avoids extra strided reads of the full skip-entry cache.
		 */
		*priority = (double)bound * (double)(span_end - span_start) /
					((double)covered_blocks * TP_BLOCK_SIZE);
	}
	return (double)bound;
}

/*
 * Called only with >=2 initialized active terms. Each strong posting is
 * visited sequentially; weak terms seek only surviving, ascending candidates.
 * All actual matching term contributions survive into each offered score.
 */
static void
score_segment_multi_term_maxscore(
		TpTopKHeap			   *heap,
		TpSegmentReader		   *reader,
		TpTermState			  **terms,
		int						term_count,
		TpBMWStats			   *stats,
		TpBMWFilter			   *filter,
		const TpMaxScoreParams *params)
{
	TpMaxScoreTerm	*states	 = palloc(sizeof(TpMaxScoreTerm) * term_count);
	TpMaxScoreMatch *matches = palloc(
			sizeof(TpMaxScoreMatch) * TP_MAXSCORE_BATCH);
	double *scores	= palloc0(sizeof(double) * TP_MAXSCORE_BATCH);
	uint64 *touched = palloc0(sizeof(uint64) * (TP_MAXSCORE_BATCH / 64));
	uint32	start	= UINT32_MAX;
	int		count	= 0;
	int		i;
	/*
	 * u=2^-24. Using 2u, plus two extra operations, leaves room for
	 * double bound accumulation/division rounding as well as <=term_count
	 * positive float4 additions. For n*2u>=1, infinity disables pruning.
	 */
	double error	= ((double)term_count + 2.0) * 0x1p-23;
	double rounding = error < 1.0 ? 1.0 / (1.0 - error) : HUGE_VAL;

	for (i = 0; i < term_count; i++)
	{
		if (term_current_doc_id(terms[i]) == UINT32_MAX)
			continue;
		states[count++].ts = terms[i];
		start			   = Min(start, term_current_doc_id(terms[i]));
	}
	while (start < reader->header->num_docs)
	{
		uint32 end = reader->header->num_docs;
		uint32 target;
		uint32 first_posting = UINT32_MAX;
		float4 threshold	 = tp_topk_threshold(heap);
		double bound		 = 0.0;
		double weak_bound;
		int	   weak_count = 0;
		int	   live_count = 0;

		CHECK_FOR_INTERRUPTS();
		/* If every cursor is ahead, the gap contains no remaining matches. */
		for (i = 0; i < count; i++)
			first_posting =
					Min(first_posting, term_current_doc_id(states[i].ts));
		start = Max(start, first_posting);
		if (start >= end)
			break;
		target = (uint32)
				Min((uint64)end, (uint64)start + TP_MAXSCORE_BOUND_WINDOW);
		for (i = 0; i < count; i++)
		{
			TpTermState *ts = states[i].ts;
			uint32		 covered_end;
			if (term_current_doc_id(ts) == UINT32_MAX)
				continue;
			states[live_count].ts	 = ts;
			states[live_count].bound = maxscore_window_bound(
					ts,
					start,
					target,
					reader->header->num_docs,
					&covered_end,
					&states[live_count].priority);
			end = Min(end, covered_end);
			live_count++;
		}
		count = live_count;
		if (count == 0)
			break;
		qsort(states,
			  count,
			  sizeof(TpMaxScoreTerm),
			  compare_maxscore_priority);
		for (i = 0; i < count; i++)
		{
			states[i].remaining = bound;
			bound += states[i].bound;
			if (maxscore_rejected(0.0f, bound, rounding, threshold))
				weak_count = i + 1;
		}
		if (weak_count == count)
		{
			if (stats)
				stats->maxscore_windows_pruned++;
			start = end;
			continue;
		}
		weak_bound = states[weak_count].remaining;
		for (i = weak_count; i < count; i++)
		{
			TpTermState *ts = states[i].ts;
			if (term_current_doc_id(ts) < start)
			{
				seek_term_to_doc(ts, start);
				if (stats)
					stats->seeks_performed++;
			}
		}
		for (;;)
		{
			uint32 base = UINT32_MAX;
			uint32 batch_end;
			int	   matches_count = 0;

			CHECK_FOR_INTERRUPTS();
			threshold = tp_topk_threshold(heap);
			/*
			 * A rising threshold can defer more terms without changing this
			 * window's bounds/order. This also avoids scanning the first whole
			 * 8192-doc cell exhaustively while the heap initially fills.
			 */
			while (weak_count < count &&
				   maxscore_rejected(
						   0.0f,
						   states[weak_count].remaining +
								   states[weak_count].bound,
						   rounding,
						   threshold))
				weak_count++;
			if (weak_count == count)
				break;
			weak_bound = states[weak_count].remaining;
			for (i = weak_count; i < count; i++)
				base = Min(base, term_current_doc_id(states[i].ts));
			if (base >= end)
				break;
			batch_end = (uint32)
					Min((uint64)end,
						(uint64)base + (heap->size < heap->capacity
												? 128
												: TP_MAXSCORE_BATCH));
			if (count - weak_count == 1)
			{
				matches_count = maxscore_one_essential_matches(
						reader,
						states[weak_count].ts,
						params,
						base,
						batch_end,
						matches,
						weak_bound,
						rounding,
						threshold,
						stats);
			}
			else if (count - weak_count == 2)
			{
				matches_count = maxscore_two_essential_matches(
						reader,
						states[weak_count].ts,
						states[weak_count + 1].ts,
						params,
						base,
						batch_end,
						matches,
						weak_bound,
						rounding,
						threshold,
						stats);
			}
			else
			{
				matches_count = maxscore_bitmap_essential_matches(
						reader,
						states,
						weak_count,
						count,
						params,
						base,
						batch_end,
						scores,
						touched,
						matches,
						weak_bound,
						rounding,
						threshold,
						stats);
			}
			for (i = weak_count - 1; i >= 0 && matches_count > 0; i--)
			{
				TpMaxScoreTerm *state = &states[i];
				TpTermState	   *ts	  = state->ts;
				int				kept  = 0;
				int				j;
				CHECK_FOR_INTERRUPTS();
				for (j = 0; j < matches_count; j++)
				{
					uint32 doc	 = matches[j].doc;
					double score = matches[j].score;
					if (maxscore_rejected(
								score,
								state->bound + state->remaining,
								rounding,
								threshold))
						continue;
					if (term_current_doc_id(ts) < doc)
					{
						seek_term_to_doc(ts, doc);
						if (stats)
							stats->seeks_performed++;
					}
					if (term_current_doc_id(ts) == doc)
						score += maxscore_term_contribution(ts, params);
					if (!maxscore_rejected(
								score, state->remaining, rounding, threshold))
					{
						matches[kept].doc	  = doc;
						matches[kept++].score = score;
					}
				}
				matches_count = kept;
			}
			for (i = 0; i < matches_count; i++)
			{
				/* Round once after all float4 term contributions are added. */
				float4 score = (float4)matches[i].score;
				if (score > 0.0f && !tp_topk_dominated(heap, score) &&
					(filter == NULL ||
					 filter->segment_matches(
							 filter->state, reader, matches[i].doc)))
					tp_topk_add_segment(
							heap, reader->root_block, matches[i].doc, score);
				if (stats)
					stats->segment_docs_scored++;
			}
		}
		start = end;
	}
	pfree(touched);
	pfree(scores);
	pfree(matches);
	pfree(states);
}

/*
 * Score multi-term segment postings using MaxScore or Block-Max WAND.
 */
static void
score_segment_multi_term_bmw(
		TpTopKHeap		 *heap,
		TpSegmentReader	 *reader,
		TpTermState		**terms,
		int				  term_count,
		float4			  k1,
		float4			  b,
		float4			  avg_doc_len,
		TpBMWStats		 *stats,
		TpBMWFilter		 *filter,
		TpMaxScoreParams *maxscore_params)
{
	int	 active_count;
	bool drive_candidates = filter != NULL && filter->segment_next != NULL;

	if (drive_candidates)
	{
		uint32 candidate;
		drive_candidates =
				filter->segment_next(filter->state, reader, 0, &candidate);
		if (drive_candidates && candidate == UINT32_MAX)
			return;
	}

	active_count = init_segment_term_states(
			terms, term_count, reader, k1, b, avg_doc_len);

	if (active_count == 0)
	{
		cleanup_segment_term_states(terms, term_count);
		return;
	}

	/*
	 * Keep selective Boolean candidate streams on the candidate-driven
	 * WAND path. MaxScore amortizes its setup on denser ranking postings.
	 */
	if (!drive_candidates && active_count >= 2)
	{
		uint64 postings = 0;

		for (int i = 0; i < term_count; i++)
			if (term_current_doc_id(terms[i]) != UINT32_MAX)
				postings += terms[i]->iter.dict_entry.doc_freq;

		if (postings >= TP_MAXSCORE_MIN_POSTINGS &&
			postings * TP_MAXSCORE_DENSITY_DENOMINATOR >=
					reader->header->num_docs)
		{
			if (!maxscore_params->initialized)
				init_maxscore_params(maxscore_params, k1, b, avg_doc_len);
			score_segment_multi_term_maxscore(
					heap,
					reader,
					terms,
					term_count,
					stats,
					filter,
					maxscore_params);
			cleanup_segment_term_states(terms, term_count);
			return;
		}
	}

	/*
	 * WAND walks terms in doc ID order and accumulates score bounds to
	 * find a pivot. Block-max refinement checks the blocks at that pivot
	 * and advances a posting cursor if their bounds fall short.
	 */
	sort_terms_by_doc_id(terms, term_count);

	while (active_count > 0)
	{
		int	   pivot_len;
		uint32 pivot_doc_id;
		float4 threshold;
		double block_upper;
		float4 doc_score;
		int	   i;

		CHECK_FOR_INTERRUPTS();

		if (drive_candidates)
		{
			uint32 minimum = term_current_doc_id(terms[0]);
			uint32 candidate;
			if (!filter->segment_next(
						filter->state, reader, minimum, &candidate))
				drive_candidates = false;
			else if (candidate == UINT32_MAX)
				break;
			else if (candidate > minimum)
			{
				for (i = 0; i < term_count; i++)
				{
					if (term_current_doc_id(terms[i]) < candidate)
					{
						if (!seek_term_to_doc(terms[i], candidate))
							active_count--;
						if (stats)
							stats->seeks_performed++;
					}
				}
				sort_terms_by_doc_id(terms, term_count);
				continue;
			}
		}

		threshold = tp_topk_threshold(heap);

		/* Step 1: Find WAND pivot */
		if (!find_wand_pivot(
					terms, term_count, threshold, &pivot_len, &pivot_doc_id))
			break; /* No term combination can beat threshold */

		/* Step 2: Seek pre-pivot terms to pivot_doc_id */
		if (!seek_to_pivot(
					terms, term_count, pivot_len, pivot_doc_id, &active_count))
			continue; /* Re-pivot with updated positions */

		/*
		 * Step 3: Block-max refinement.
		 * Check if block-level upper bound still beats threshold.
		 *
		 * Correctness note (#365): block_max_skip_advance() advances
		 * a pivot term past its current block via seek_term_to_doc.
		 * Docs in the skipped block that exist in *non-pivot* terms'
		 * posting lists too can still be top-K candidates -- their
		 * total true score includes non-pivot term contributions
		 * that the pivot's block_upper alone doesn't capture.
		 *
		 * The safe-skip condition is therefore:
		 *   block_upper(pivot) + max_score_sum(non-pivot) <= threshold
		 * (an upper bound on a hypothetical doc's score using
		 * pivot terms' actual block-max plus non-pivot terms' global
		 * max). Only then can we skip without losing a top-K
		 * candidate -- or under-scoring one we later see via another
		 * pivot, after a relevant term already jumped over it.
		 *
		 * Previous version used block_upper alone, which let
		 * block_max_skip_advance jump a term past a doc that other
		 * pivot iterations would later pick up with one term missing
		 * from its score sum. Manifests at large K (low heap
		 * threshold) where the looser check fires for blocks whose
		 * docs are still top-K when non-pivot contributions count.
		 */
		block_upper = compute_block_max_at_pivot(terms, pivot_len);

		{
			double non_pivot_max = 0.0;
			int	   np;

			for (np = pivot_len; np < term_count; np++)
			{
				TpTermState *ts = terms[np];
				if (!ts->found || ts->iter.finished)
					continue;
				non_pivot_max += ts->max_score;
			}

			if ((block_upper + non_pivot_max) <= (double)threshold)
			{
				block_max_skip_advance(
						terms, term_count, pivot_len, &active_count, stats);
				continue;
			}
		}

		if (stats)
			stats->blocks_scanned++;

		/* Step 4: Verify all pivot terms are at pivot_doc_id */
		if (!verify_pivot_alignment(terms, pivot_len, pivot_doc_id))
			continue; /* Re-pivot with new positions */

		/* Skip dead docs */
		if (!tp_segment_is_alive(reader, pivot_doc_id))
		{
			if (stats)
				stats->dead_docs_skipped++;
		}
		else
		{
			/* Step 5: Score the pivot document */
			doc_score =
					score_pivot_document(terms, pivot_len, k1, b, avg_doc_len);

			if (doc_score > 0.0f && !tp_topk_dominated(heap, doc_score) &&
				(filter == NULL ||
				 filter->segment_matches(filter->state, reader, pivot_doc_id)))
				tp_topk_add_segment(
						heap, reader->root_block, pivot_doc_id, doc_score);

			if (stats)
				stats->segment_docs_scored++;
		}

		/*
		 * Step 6: Advance all pivot terms past pivot_doc_id.
		 * Iterate backward so restore_ordering shifts don't
		 * skip any terms.
		 */
		for (i = pivot_len - 1; i >= 0; i--)
		{
			if (term_current_doc_id(terms[i]) == pivot_doc_id)
			{
				if (!advance_term_iterator(terms[i]))
					active_count--;
				restore_ordering(terms, term_count, i);
			}
		}
	}

	cleanup_segment_term_states(terms, term_count);
}

/*
 * Score multi-term queries across the memtable and segments.
 */
int
tp_score_multi_term_bmw(
		TpLocalIndexState			 *local_state,
		Relation					  index,
		const TpSegmentGraphSnapshot *snapshot,
		TpDataSource				 *memtable_src,
		char						**query_terms,
		int							  term_count,
		int32						 *query_freqs,
		float4						 *idfs,
		float4						  k1,
		float4						  b,
		float4						  avg_doc_len,
		int							  max_results,
		ItemPointerData				 *result_ctids,
		float4						 *result_scores,
		TpBMWStats					 *stats,
		TpBMWFilter					 *filter)
{
	TpTopKHeap		 heap;
	TpMaxScoreParams maxscore_params = {0};
	TpTermState	   **terms;
	int				 result_count;
	int				 i;

	(void)local_state; /* reserved for future use */

	/* Initialize stats */
	if (stats)
		memset(stats, 0, sizeof(TpBMWStats));

	/* Initialize top-k heap */
	tp_topk_init(&heap, max_results, CurrentMemoryContext);

	/* Initialize term states */
	terms = palloc(term_count * sizeof(TpTermState *));
	for (i = 0; i < term_count; i++)
	{
		terms[i]			 = palloc(sizeof(TpTermState));
		terms[i]->term		 = query_terms[i];
		terms[i]->idf		 = idfs[i];
		terms[i]->query_freq = query_freqs[i];
	}

	/* Score memtable (exhaustive - no skip index) */
	score_memtable_multi_term(
			&heap,
			memtable_src,
			terms,
			term_count,
			k1,
			b,
			avg_doc_len,
			stats,
			filter);
	if (filter != NULL)
		filter->reset(filter->state);

	/* Select a scoring strategy for each segment. */
	for (uint32 level = 0; level < TP_MAX_LEVELS; level++)
	{
		uint32			   root_count;
		const BlockNumber *roots =
				tp_segment_graph_snapshot_level(snapshot, level, &root_count);

		for (uint32 root_idx = 0; root_idx < root_count; root_idx++)
		{
			TpSegmentReader *reader = tp_segment_open(index, roots[root_idx]);

			CHECK_FOR_INTERRUPTS();

			score_segment_multi_term_bmw(
					&heap,
					reader,
					terms,
					term_count,
					k1,
					b,
					avg_doc_len,
					stats,
					filter,
					&maxscore_params);
			if (filter != NULL)
				filter->reset(filter->state);

			tp_segment_close(reader);
		}
	}

	for (i = 0; i < term_count; i++)
		pfree(terms[i]);
	pfree(terms);

	/* Resolve CTIDs for segment results before extraction */
	tp_topk_resolve_ctids(&heap, index);

	/* Extract sorted results */
	result_count = tp_topk_extract(&heap, result_ctids, result_scores);

	if (stats)
		stats->docs_in_results = result_count;

	return result_count;
}
