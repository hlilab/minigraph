#include <assert.h>
#include "gfa-priv.h"
#include "ksort.h"

typedef struct {
	uint32_t side;
	uint32_t ins:31, end:1;
} gfa_split_t;

#define split_key(p) ((p).side)
KRADIX_SORT_INIT(split, gfa_split_t, split_key, 4)

void gfa_augment(gfa_t *g, int32_t n_ins, const gfa_ins_t *ins, int32_t n_ctg, const char **name, const char **seq)
{
	int32_t i, j, k, *scnt, *soff, n_ctg_seg, n_old_seg, n_seg, *ins2seg;
	gfa_split_t *sp;
	gfa_seg_t *seg;
	char buf[16];
	uint32_t *old2new;
	uint64_t t, n_old_arc, *ins_side;

	if (n_ins <= 0 || n_ctg <= 0) return;

	// set soff[]
	GFA_CALLOC(scnt, g->n_seg);
	for (i = 0; i < n_ins; ++i)
		++scnt[ins[i].v[0]>>1], ++scnt[ins[i].v[1]>>1];
	GFA_MALLOC(soff, g->n_seg + 1);
	for (j = 1, soff[0] = 0; j <= g->n_seg; ++j)
		soff[j] = soff[j-1] + scnt[j-1];

	// populate sp[]
	GFA_MALLOC(sp, soff[g->n_seg]);
	GFA_BZERO(scnt, g->n_seg);
	for (i = 0, n_ctg_seg = 0; i < n_ins; ++i) {
		const gfa_ins_t *p = &ins[i];
		for (k = 0; k < 2; ++k) {
			uint32_t vlen = g->seg[p->v[k]>>1].len;
			gfa_split_t *q = &sp[soff[p->v[k]>>1] + scnt[p->v[k]>>1]];
			q->ins = i, q->end = k;
			q->side = (p->v[k]&1? vlen - p->voff[k] : p->voff[k]) << 1 | ((p->v[k]&1) ^ k);
			assert(q->side != (0<<1|0) && q->side != (vlen<<1|1)); // not possible to link such sides
			++scnt[p->v[k]>>1];
		}
		if (p->coff[1] > p->coff[0])
			++n_ctg_seg;
	}
	free(scnt);

	// sort sp[]
	for (j = 0, n_old_seg = 0; j < g->n_seg; ++j)
		if (soff[j+1] - soff[j] > 1)
			radix_sort_split(&sp[soff[j]], &sp[soff[j+1]]);

	// precompute the number of segments after split
	for (j = 0, n_old_seg = 0; j < g->n_seg; ++j) {
		int32_t i0;
		for (i0 = soff[j], i = i0 + 1, k = 0; i <= soff[j+1]; ++i)
			if (i == soff[j+1] || sp[i0].side>>1 != sp[i].side>>1) {
				if (sp[i0].side>>1 != 0 && sp[i0].side>>1 != g->seg[j].len) // otherwise no new segment will be created
					++k;
				i0 = i;
			}
		n_old_seg += k + 1;
	}

	// create newly inserted segments
	n_seg = n_old_seg + n_ctg_seg;
	GFA_CALLOC(seg, n_seg);
	GFA_MALLOC(ins2seg, n_ins);
	for (i = 0, k = n_old_seg; i < n_ins; ++i) {
		const gfa_ins_t *p = &ins[i];
		gfa_seg_t *t;
		ins2seg[i] = -1;
		if (p->coff[1] <= p->coff[0]) continue; // no new segment created
		ins2seg[i] = k;
		t = &seg[k++];
		snprintf(buf, 15, "v%d", k);
		t->name = strdup(buf);
		GFA_MALLOC(t->seq, p->coff[1] - p->coff[0] + 1);
		for (j = 0; j < p->coff[1] - p->coff[0]; ++j)
			t->seq[j] = seq[i][p->coff[0] + j];
		t->seq[j] = 0;
		t->len = j;
		t->pnid = gfa_add_pname(g, name[i]);
		t->ppos = p->coff[0];
		t->rank = g->max_rank + 1; // TODO: to deal with SN/SS/SR tags somewhere
	}

	// compute ins_side[] and split old segments
	g->is_srt = g->is_symm = 0;
	n_old_arc = g->n_arc;
	GFA_CALLOC(ins_side, n_ins);
	GFA_MALLOC(old2new, g->n_seg * 2);
	for (j = 0, k = 0; j < g->n_seg; ++j) {
		int32_t i0, l, off = 0, k0 = k;
		gfa_seg_t *s = &g->seg[j];
		gfa_seg_t *t = &seg[k]; // this is so far a placeholder
		// create the first half of a new segment
		snprintf(buf, 15, "v%d", k);
		t->name = strdup(buf);
		t->pnid = s->pnid, t->ppos = s->ppos, t->rank = s->rank;
		// iterate over splits
		for (i0 = soff[j], i = i0 + 1; i <= soff[j+1]; ++i) {
			if (i == soff[j+1] || sp[i].side>>1 != sp[i0].side>>1) {
				gfa_split_t *q0 = &sp[i0];
				for (l = i0; l < i; ++l) {
					gfa_split_t *q = &sp[l];
					int shift = q->end? 32 : 0;
					if (q->side&1) ins_side[q->ins] |= (uint64_t)(k << 1 | 1) << shift;
					else ins_side[q->ins] |= (uint64_t)((k - 1) << 1) << shift;
				}
				if (q0->side>>1 != 0 && q0->side>>1 != g->seg[j].len) { // create a new segment
					t->len = (q0->side>>1) - off;
					GFA_MALLOC(t->seq, t->len + 1);
					memcpy(t->seq, &s->seq[off], t->len);
					t->seq[t->len] = 0;
					off += t->len;
					t = &seg[k++]; // create a new segment
					snprintf(buf, 15, "v%d", k);
					t->name = strdup(buf);
					t->pnid = s->pnid, t->ppos = s->ppos + off, t->rank = s->rank;
				}
				i0 = i;
			}
		}
		// finish the last segment
		t->len = s->len - off;
		GFA_MALLOC(t->seq, t->len + 1);
		memcpy(t->seq, &s->seq[off], t->len);
		t->seq[t->len] = 0;
		// translate table for old and new vertices
		old2new[j<<1|0] = (uint32_t)(k - 1) << 1 | 0;
		old2new[j<<1|1] = (uint32_t)k0 << 1 | 1;
		// add new arcs between newly created segments
		for (i = 0; i < k - k0 - 1; ++i) {
			gfa_arc_t *a;
			if (g->n_arc == g->m_arc) GFA_EXPAND(g->arc, g->m_arc);
			a = &g->arc[g->n_arc++];
			a->ov = a->ow = 0;
			a->link_id = g->n_arc - 1;
			a->del = a->comp = 0;
			a->v_lv = (uint64_t)(k0 + i) << 33 | seg[k0 + i].len;
			a->w = (uint32_t)(k0 + i + 1) << 1;
			a->lw = seg[k0 + i + 1].len;
		}
	}
	assert(k == n_old_seg);

	// update existing g->arc[]
	for (t = 0; t < n_old_arc; ++t) {
		gfa_arc_t *a = &g->arc[t];
		uint32_t v = old2new[a->v_lv>>32];
		a->v_lv = (uint64_t)v << 32 | seg[v>>1].len;
		a->w = old2new[a->w];
		a->lw = seg[a->w>>1].len;
	}
	free(old2new);

	free(sp); free(soff);
	gfa_arc_sort(g);
	gfa_arc_index(g);
}
