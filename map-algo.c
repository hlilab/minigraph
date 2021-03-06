#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include "kalloc.h"
#include "mgpriv.h"
#include "khash.h"

struct mg_tbuf_s {
	void *km;
	int frag_gap;
};

mg_tbuf_t *mg_tbuf_init(void)
{
	mg_tbuf_t *b;
	b = (mg_tbuf_t*)calloc(1, sizeof(mg_tbuf_t));
	if (!(mg_dbg_flag & MG_DBG_NO_KALLOC)) b->km = km_init();
	return b;
}

void mg_tbuf_destroy(mg_tbuf_t *b)
{
	if (b == 0) return;
	if (b->km) km_destroy(b->km);
	free(b);
}

void *mg_tbuf_get_km(mg_tbuf_t *b)
{
	return b->km;
}

static void collect_minimizers(void *km, const mg_mapopt_t *opt, const mg_idx_t *gi, int n_segs, const int *qlens, const char **seqs, mg128_v *mv)
{
	int i, n, sum = 0;
	mv->n = 0;
	for (i = n = 0; i < n_segs; ++i) {
		size_t j;
		mg_sketch(km, seqs[i], qlens[i], gi->w, gi->k, i, mv);
		for (j = n; j < mv->n; ++j)
			mv->a[j].y += sum << 1;
		sum += qlens[i], n = mv->n;
	}
}

#include "ksort.h"
#define heap_lt(a, b) ((a).x > (b).x)
KSORT_INIT(heap, mg128_t, heap_lt)

typedef struct {
	uint32_t n;
	uint32_t q_pos, q_span;
	uint32_t seg_id:16, weight:15, is_tandem:1;
	const uint64_t *cr;
} mg_match_t;

static mg_match_t *collect_matches(void *km, int *_n_m, int max_occ, const mg_idx_t *gi, const mg128_v *mv, int64_t *n_a, int *rep_len, int *n_mini_pos, int32_t **mini_pos)
{
	int rep_st = 0, rep_en = 0, n_m;
	size_t i;
	mg_match_t *m;
	*n_mini_pos = 0;
	KMALLOC(km, *mini_pos, mv->n);
	m = (mg_match_t*)kmalloc(km, mv->n * sizeof(mg_match_t));
	for (i = 0, n_m = 0, *rep_len = 0, *n_a = 0; i < mv->n; ++i) {
		const uint64_t *cr;
		mg128_t *p = &mv->a[i];
		uint32_t q_pos = (uint32_t)p->y, q_span = p->x & 0xff;
		int t;
		cr = mg_idx_get(gi, p->x>>8, &t);
		if (t >= max_occ) {
			int en = (q_pos >> 1) + 1, st = en - q_span;
			if (st > rep_en) {
				*rep_len += rep_en - rep_st;
				rep_st = st, rep_en = en;
			} else rep_en = en;
		} else {
			mg_match_t *q = &m[n_m++];
			q->q_pos = q_pos, q->q_span = q_span, q->cr = cr, q->n = t, q->seg_id = p->y >> 32;
			q->is_tandem = 0, q->weight = 255;
			if (i > 0 && p->x>>8 == mv->a[i - 1].x>>8) q->is_tandem = 1;
			if (i < mv->n - 1 && p->x>>8 == mv->a[i + 1].x>>8) q->is_tandem = 1;
			*n_a += q->n;
			(*mini_pos)[(*n_mini_pos)++] = q_pos>>1;
		}
	}
	*rep_len += rep_en - rep_st;
	*_n_m = n_m;
	return m;
}

static void cal_weight(int base, int n_m, mg_match_t *m)
{
	const float b = 10.0, log2_b = 3.321928f; // log2_b is slightly smaller than log_2{b}
	int i;
	for (i = 0; i < n_m; ++i) {
		mg_match_t *p = &m[i];
		p->weight = 255;
		if (p->n > base) {
			float x = (float)(b * p->n / base);
			float y = log2_b / mg_log2(x); // y < 1 if there were no rounding errors
			p->weight = y >= 1.0f? 255 : (int)(255.0 * (y > 0.7f? y : 0.7f));
		}
	}
}

static mg128_t *collect_seed_hits_heap(void *km, const mg_mapopt_t *opt, int max_occ, const mg_idx_t *gi, const char *qname, const mg128_v *mv, int qlen, int64_t *n_a, int *rep_len,
								  int *n_mini_pos, int32_t **mini_pos)
{
	int i, n_m, heap_size = 0;
	int64_t n_for = 0, n_rev = 0;
	mg_match_t *m;
	mg128_t *a, *heap;

	m = collect_matches(km, &n_m, max_occ, gi, mv, n_a, rep_len, n_mini_pos, mini_pos);
	cal_weight(opt->occ_weight, n_m, m);

	heap = (mg128_t*)kmalloc(km, n_m * sizeof(mg128_t));
	a = (mg128_t*)kmalloc(km, *n_a * sizeof(mg128_t));

	for (i = 0, heap_size = 0; i < n_m; ++i) {
		if (m[i].n > 0) {
			heap[heap_size].x = m[i].cr[0];
			heap[heap_size].y = (uint64_t)i<<32;
			++heap_size;
		}
	}
	ks_heapmake_heap(heap_size, heap);
	while (heap_size > 0) {
		mg_match_t *q = &m[heap->y>>32];
		mg128_t *p;
		uint64_t r = heap->x;
		int32_t rpos = (uint32_t)r >> 1;
		// TODO: skip anchor if MG_F_NO_DIAL
		if ((r&1) == (q->q_pos&1)) { // forward strand
			p = &a[n_for++];
			p->x = r>>32<<33 | rpos;
		} else { // reverse strand; TODO: more testing needed for this block
			p = &a[(*n_a) - (++n_rev)];
			p->x = r>>32<<33 | 1ULL<<32 | (gi->g->seg[r>>32].len - (rpos + 1 - q->q_span) - 1);
		}
		p->y = (uint64_t)q->q_span << 32 | q->q_pos >> 1;
		p->y |= (uint64_t)q->seg_id << MG_SEED_SEG_SHIFT;
		if (q->is_tandem) p->y |= MG_SEED_TANDEM;
		p->y |= (uint64_t)q->weight << MG_SEED_WT_SHIFT;
		// update the heap
		if ((uint32_t)heap->y < q->n - 1) {
			++heap[0].y;
			heap[0].x = m[heap[0].y>>32].cr[(uint32_t)heap[0].y];
		} else {
			heap[0] = heap[heap_size - 1];
			--heap_size;
		}
		ks_heapdown_heap(0, heap_size, heap);
	}
	kfree(km, m);
	kfree(km, heap);

	// reverse anchors on the reverse strand, as they are in the descending order
	if (*n_a > n_for + n_rev) {
		memmove(a + n_for, a + (*n_a) - n_rev, n_rev * sizeof(mg128_t));
		*n_a = n_for + n_rev;
	}
	return a;
}

static mg128_t *collect_seed_hits(void *km, const mg_mapopt_t *opt, int max_occ, const mg_idx_t *gi, const char *qname, const mg128_v *mv, int qlen, int64_t *n_a, int *rep_len,
								  int *n_mini_pos, int32_t **mini_pos)
{
	int i, n_m;
	mg_match_t *m;
	mg128_t *a;
	m = collect_matches(km, &n_m, max_occ, gi, mv, n_a, rep_len, n_mini_pos, mini_pos);
	cal_weight(opt->occ_weight, n_m, m);
	a = (mg128_t*)kmalloc(km, *n_a * sizeof(mg128_t));
	for (i = 0, *n_a = 0; i < n_m; ++i) {
		mg_match_t *q = &m[i];
		const uint64_t *r = q->cr;
		uint32_t k;
		for (k = 0; k < q->n; ++k) {
			int32_t rpos = (uint32_t)r[k] >> 1;
			mg128_t *p;
			if (qname && (opt->flag & MG_M_NO_DIAG)) {
				const gfa_seg_t *s = &gi->g->seg[r[k]>>32];
				const char *gname = s->snid >= 0 && gi->g->sseq? gi->g->sseq[s->snid].name : s->name;
				int32_t g_pos;
				if (s->snid >= 0 && gi->g->sseq)
					gname = gi->g->sseq[s->snid].name, g_pos = s->soff + (uint32_t)r[k];
				else
					gname = s->name, g_pos = (uint32_t)r[k];
				if (g_pos == q->q_pos && strcmp(qname, gname) == 0)
					continue;
			}
			p = &a[(*n_a)++];
			if ((r[k]&1) == (q->q_pos&1)) // forward strand
				p->x = r[k]>>32<<33 | rpos;
			else // reverse strand
				p->x = r[k]>>32<<33 | 1ULL<<32 | (gi->g->seg[r[k]>>32].len - (rpos + 1 - q->q_span) - 1);
			p->y = (uint64_t)q->q_span << 32 | q->q_pos >> 1;
			p->y |= (uint64_t)q->seg_id << MG_SEED_SEG_SHIFT;
			if (q->is_tandem) p->y |= MG_SEED_TANDEM;
			p->y |= (uint64_t)q->weight << MG_SEED_WT_SHIFT;
		}
	}
	kfree(km, m);
	radix_sort_128x(a, a + (*n_a));
	return a;
}

static int64_t flt_anchors(int64_t n_a, mg128_t *a, int32_t r)
{
	int64_t i, j;
	for (i = 0; i < n_a; ++i) {
		for (j = i - 1; j >= 0; --j) {
			int32_t dq;
			int64_t dr = a[i].x - a[j].x;
			if (dr > r) break;
			dq = (int32_t)a[i].y - (int32_t)a[j].y;
			if (dq > r || dq < 0) continue;
			a[j].y |= MG_SEED_KEPT;
			a[i].y |= MG_SEED_KEPT;
			break;
		}
	}
	for (i = n_a - 1; i >= 0; --i) {
		if (a[i].y & MG_SEED_KEPT) continue;
		for (j = i + 1; j < n_a; ++j) {
			int32_t dq;
			int64_t dr = a[j].x - a[i].x;
			if (dr > r) break;
			dq = (int32_t)a[j].y - (int32_t)a[i].y;
			if (dq > r || dq < 0) continue;
			a[j].y |= MG_SEED_KEPT;
			a[i].y |= MG_SEED_KEPT;
			break;
		}
	}
	for (i = j = 0; i < n_a; ++i)
		if (a[i].y & MG_SEED_KEPT)
			a[j++] = a[i];
	return j;
}

void mg_map_frag(const mg_idx_t *gi, int n_segs, const int *qlens, const char **seqs, mg_gchains_t **gcs, mg_tbuf_t *b, const mg_mapopt_t *opt, const char *qname)
{
	int i, l, rep_len, qlen_sum, n_lc, n_gc, n_mini_pos;
	int max_chain_gap_qry, max_chain_gap_ref, is_splice = !!(opt->flag & MG_M_SPLICE), is_sr = !!(opt->flag & MG_M_SR);
	uint32_t hash;
	int64_t n_a;
	uint64_t *u;
	int32_t *mini_pos;
	mg128_t *a;
	mg128_v mv = {0,0,0};
	mg_lchain_t *lc;
	char *seq_cat;
	km_stat_t kmst;
	float tmp, chn_pen_gap, chn_pen_skip;

	for (i = 0, qlen_sum = 0; i < n_segs; ++i)
		qlen_sum += qlens[i], gcs[i] = 0;

	if (qlen_sum == 0 || n_segs <= 0 || n_segs > MG_MAX_SEG) return;
	if (opt->max_qlen > 0 && qlen_sum > opt->max_qlen) return;

	hash  = qname? __ac_X31_hash_string(qname) : 0;
	hash ^= __ac_Wang_hash(qlen_sum) + __ac_Wang_hash(opt->seed);
	hash  = __ac_Wang_hash(hash);

	collect_minimizers(b->km, opt, gi, n_segs, qlens, seqs, &mv);
	if (opt->flag & MG_M_HEAP_SORT) a = collect_seed_hits_heap(b->km, opt, opt->occ_max1, gi, qname, &mv, qlen_sum, &n_a, &rep_len, &n_mini_pos, &mini_pos);
	else a = collect_seed_hits(b->km, opt, opt->occ_max1, gi, qname, &mv, qlen_sum, &n_a, &rep_len, &n_mini_pos, &mini_pos);

	if (mg_dbg_flag & MG_DBG_SEED) {
		fprintf(stderr, "RS\t%d\n", rep_len);
		for (i = 0; i < n_a; ++i)
			fprintf(stderr, "SD\t%s\t%d\t%c\t%d\t%d\t%d\n", gi->g->seg[a[i].x>>33].name, (int32_t)a[i].x, "+-"[a[i].x>>32&1], (int32_t)a[i].y, (int32_t)(a[i].y>>32&0xff),
					i == 0? 0 : ((int32_t)a[i].y - (int32_t)a[i-1].y) - ((int32_t)a[i].x - (int32_t)a[i-1].x));
	}

	// set max chaining gap on the query and the reference sequence
	if (is_sr)
		max_chain_gap_qry = qlen_sum > opt->max_gap? qlen_sum : opt->max_gap;
	else max_chain_gap_qry = opt->max_gap;
	if (opt->max_gap_ref > 0) {
		max_chain_gap_ref = opt->max_gap_ref; // always honor mg_mapopt_t::max_gap_ref if set
	} else if (opt->max_frag_len > 0) {
		max_chain_gap_ref = opt->max_frag_len - qlen_sum;
		if (max_chain_gap_ref < opt->max_gap) max_chain_gap_ref = opt->max_gap;
	} else max_chain_gap_ref = opt->max_gap;

	tmp = expf(-opt->div * gi->k);
	chn_pen_gap = opt->chn_pen_gap * tmp;
	chn_pen_skip = opt->chn_pen_skip * tmp;

	if (!(opt->flag & MG_M_RMQ) && !is_splice && !is_sr && opt->max_gap_pre > 0 && opt->max_gap_pre * 2 < opt->max_gap)
		n_a = flt_anchors(n_a, a, opt->max_gap_pre);
	if (n_a == 0) {
		if (a) kfree(b->km, a);
		a = 0, n_lc = 0, u = 0;
	} else {
		if (opt->flag & MG_M_RMQ)
			a = mg_lchain_rmq(opt->bw, opt->max_gap_pre, opt->max_lc_skip, opt->max_rmq_size, opt->min_lc_cnt, opt->min_lc_score, chn_pen_gap, chn_pen_skip, n_a, a, &n_lc, &u, b->km);
		else
			a = mg_lchain_dp(max_chain_gap_ref, max_chain_gap_qry, opt->bw, opt->max_lc_skip, opt->max_lc_iter, opt->min_lc_cnt, opt->min_lc_score,
							 chn_pen_gap, chn_pen_skip, is_splice, n_segs, n_a, a, &n_lc, &u, b->km);
	}

	b->frag_gap = max_chain_gap_ref;
	kfree(b->km, mv.a);

	if (n_lc) {
		lc = mg_lchain_gen(b->km, hash, qlen_sum, n_lc, u, a);
		for (i = 0; i < n_lc; ++i)
			mg_update_anchors(lc[i].cnt, &a[lc[i].off], n_mini_pos, mini_pos);
	} else lc = 0;
	kfree(b->km, mini_pos);
	kfree(b->km, u);

	if (mg_dbg_flag & MG_DBG_LCHAIN)
		mg_print_lchain(stdout, gi, n_lc, lc, a, qname);

	KMALLOC(b->km, seq_cat, qlen_sum);
	for (i = l = 0; i < n_segs; ++i) {
		strncpy(&seq_cat[l], seqs[i], qlens[i]);
		l += qlens[i];
	}
	n_gc = mg_gchain1_dp(b->km, gi->g, &n_lc, lc, qlen_sum, max_chain_gap_ref, max_chain_gap_qry, opt->bw, opt->max_gc_skip, opt->ref_bonus,
						 chn_pen_gap, chn_pen_skip, opt->mask_level, opt->max_gc_seq_ext, seq_cat, a, &u);
	gcs[0] = mg_gchain_gen(0, b->km, gi->g, n_gc, u, lc, a, hash, opt->min_gc_cnt, opt->min_gc_score);
	gcs[0]->rep_len = rep_len;
	kfree(b->km, seq_cat);
	kfree(b->km, a);
	kfree(b->km, lc);
	kfree(b->km, u);

	mg_gchain_set_parent(b->km, opt->mask_level, gcs[0]->n_gc, gcs[0]->gc, opt->sub_diff, 0);
	mg_gchain_flt_sub(opt->pri_ratio, gi->k * 2, opt->best_n, gcs[0]->n_gc, gcs[0]->gc);
	mg_gchain_drop_flt(b->km, gcs[0]);
	mg_gchain_set_mapq(b->km, gcs[0], qlen_sum, mv.n, opt->min_gc_score);

	if (b->km) {
		km_stat(b->km, &kmst);
		if (mg_dbg_flag & MG_DBG_QNAME)
			fprintf(stderr, "QM\t%s\t%d\tcap=%ld,nCore=%ld,largest=%ld\n", qname, qlen_sum, kmst.capacity, kmst.n_cores, kmst.largest);
		if (kmst.n_blocks != kmst.n_cores) {
			fprintf(stderr, "[E::%s] memory leak at %s\n", __func__, qname);
			abort();
		}
		if (kmst.largest > 1U<<28) {
			km_destroy(b->km);
			b->km = km_init();
		}
	}
}

mg_gchains_t *mg_map(const mg_idx_t *gi, int qlen, const char *seq, mg_tbuf_t *b, const mg_mapopt_t *opt, const char *qname)
{
	mg_gchains_t *gcs;
	mg_map_frag(gi, 1, &qlen, &seq, &gcs, b, opt, qname);
	return gcs;
}
