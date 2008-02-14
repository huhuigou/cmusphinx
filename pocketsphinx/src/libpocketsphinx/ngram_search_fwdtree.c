/* -*- c-basic-offset: 4; indent-tabs-mode: nil -*- */
/* ====================================================================
 * Copyright (c) 2008 Carnegie Mellon University.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * This work was supported in part by funding from the Defense Advanced 
 * Research Projects Agency and the National Science Foundation of the 
 * United States of America, and the CMU Sphinx Speech Consortium.
 *
 * THIS SOFTWARE IS PROVIDED BY CARNEGIE MELLON UNIVERSITY ``AS IS'' AND 
 * ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, 
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY
 * NOR ITS EMPLOYEES BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ====================================================================
 *
 */

/**
 * @file ngram_search_fwdtree.c Lexicon tree search.
 */

/* System headers. */
#include <string.h>
#include <assert.h>

/* SphinxBase headers. */
#include <ckd_alloc.h>
#include <linklist.h>

/* Local headers. */
#include "ngram_search_fwdtree.h"

/*
 * NOTE: this module assumes that the dictionary is organized as follows:
 *     Main, real dictionary words
 *     </s>
 *     <s>... (possibly more than one of these)
 *     <sil>
 *     noise-words...
 * In particular, note that </s> comes before <s> since </s> occurs in the LM, but
 * not <s> (well, there's no transition to <s> in the LM).
 *
 * This should probably be fixed at some point.
 */

/**
 * Enter a word in the backpointer table.
 */
static void save_bwd_ptr(ngram_search_t *ngs, int frame_idx, int32 w,
                         int32 score, int32 path, int32 rc);

/* Turn this on to dump channels for debugging */
#define __CHAN_DUMP__		0
#if __CHAN_DUMP__
#define chan_v_eval(chan) hmm_dump_vit_eval(&(chan)->hmm, stderr)
#else
#define chan_v_eval(chan) hmm_vit_eval(&(chan)->hmm)
#endif

/*
 * Allocate that part of the search channel tree structure that is independent of the
 * LM in use.
 */
static void
init_search_tree(ngram_search_t *ngs)
{
    int32 w, mpx, max_ph0, i, n_words, n_main_words;
    dict_entry_t *de;

    n_words = ngs->dict->dict_entry_count;
    n_main_words = dict_get_num_main_words(ngs->dict);
    ngs->homophone_set = ckd_calloc(n_main_words, sizeof(*ngs->homophone_set));

    /* Find #single phone words, and #unique first diphones (#root channels) in dict. */
    max_ph0 = -1;
    ngs->n_1ph_words = 0;
    mpx = ngs->dict->dict_list[0]->mpx;
    for (w = 0; w < n_main_words; w++) {
        de = ngs->dict->dict_list[w];

        /* Paranoia Central; this check can probably be removed (RKM) */
        if (de->mpx != mpx)
            E_FATAL("HMM tree words not all mpx or all non-mpx\n");

        if (de->len == 1)
            ngs->n_1ph_words++;
        else {
            if (max_ph0 < de->phone_ids[0])
                max_ph0 = de->phone_ids[0];
        }
    }

    /* Add remaining dict words (</s>, <s>, <sil>, noise words) to single-phone words */
    ngs->n_1ph_words += (n_words - n_main_words);
    ngs->n_root_chan_alloc = max_ph0 + 1;

    /* Allocate and initialize root channels */
    ngs->root_chan =
        ckd_calloc(ngs->n_root_chan_alloc, sizeof(*ngs->root_chan));
    for (i = 0; i < ngs->n_root_chan_alloc; i++) {
        hmm_init(ngs->hmmctx, &ngs->root_chan[i].hmm, mpx, -1, -1);
        ngs->root_chan[i].penult_phn_wid = -1;
        ngs->root_chan[i].next = NULL;
    }

    /* Allocate space for left-diphone -> root-chan map */
    ngs->first_phone_rchan_map =
        ckd_calloc(ngs->n_root_chan_alloc,
                   sizeof(*ngs->first_phone_rchan_map));

    /* Permanently allocate channels for single-phone words (1/word) */
    ngs->all_rhmm = ckd_calloc(ngs->n_1ph_words, sizeof(*ngs->all_rhmm));
    i = 0;
    for (w = 0; w < n_words; w++) {
        de = ngs->dict->dict_list[w];
        if (de->len != 1)
            continue;

        ngs->all_rhmm[i].diphone = de->phone_ids[0];
        ngs->all_rhmm[i].ciphone = de->ci_phone_ids[0];
        hmm_init(ngs->hmmctx, &ngs->all_rhmm[i].hmm, de->mpx,
                 de->phone_ids[0], de->ci_phone_ids[0]);
        ngs->all_rhmm[i].next = NULL;

        ngs->word_chan[w] = (chan_t *) &(ngs->all_rhmm[i]);
        i++;
    }

    ngs->single_phone_wid = ckd_calloc(ngs->n_1ph_words,
                                       sizeof(*ngs->single_phone_wid));
    E_INFO("%d root, %d non-root channels, %d single-phone words\n",
           ngs->n_root_chan, ngs->n_nonroot_chan, ngs->n_1ph_words);
}

/*
 * One-time initialization of internal channels in HMM tree.
 */
static void
init_nonroot_chan(ngram_search_t *ngs, chan_t * hmm, int32 ph, int32 ci)
{
    hmm->next = NULL;
    hmm->alt = NULL;
    hmm->info.penult_phn_wid = -1;
    hmm->ciphone = ci;
    hmm_init(ngs->hmmctx, &hmm->hmm, FALSE, ph, ci);
}

/*
 * Allocate and initialize search channel-tree structure.
 * At this point, all the root-channels have been allocated and partly initialized
 * (as per init_search_tree()), and channels for all the single-phone words have been
 * allocated and initialized.  None of the interior channels of search-trees have
 * been allocated.
 * This routine may be called on every utterance, after reinit_search_tree() clears
 * the search tree created for the previous utterance.  Meant for reconfiguring the
 * search tree to suit the currently active LM.
 */
static void
create_search_tree(ngram_search_t *ngs)
{
    dict_entry_t *de;
    chan_t *hmm;
    root_chan_t *rhmm;
    int32 w, i, j, p, ph;
    int32 n_words, n_main_words;

    n_words = ngs->dict->dict_entry_count;
    n_main_words = dict_get_num_main_words(ngs->dict);

    E_INFO("Creating search tree\n");

    for (w = 0; w < n_main_words; w++)
        ngs->homophone_set[w] = -1;

    for (i = 0; i < ngs->n_root_chan_alloc; i++)
        ngs->first_phone_rchan_map[i] = -1;

    E_INFO("%d root, %d non-root channels, %d single-phone words\n",
           ngs->n_root_chan, ngs->n_nonroot_chan, ngs->n_1ph_words);

    ngs->n_1ph_LMwords = 0;
    ngs->n_root_chan = 0;
    ngs->n_nonroot_chan = 0;

    for (w = 0; w < n_main_words; w++) {
        de = ngs->dict->dict_list[w];

        /* Ignore dictionary words not in LM */
        if (!ngram_model_set_known_wid(ngs->lmset, de->wid))
            continue;

        /* Handle single-phone words individually; not in channel tree */
        if (de->len == 1) {
            ngs->single_phone_wid[ngs->n_1ph_LMwords++] = w;
            continue;
        }

        /* Insert w into channel tree; first find or allocate root channel */
        if (ngs->first_phone_rchan_map[de->phone_ids[0]] < 0) {
            ngs->first_phone_rchan_map[de->phone_ids[0]] = ngs->n_root_chan;
            rhmm = &(ngs->root_chan[ngs->n_root_chan]);
            if (hmm_is_mpx(&rhmm->hmm))
                rhmm->hmm.s.mpx_ssid[0] = de->phone_ids[0];
            else
                rhmm->hmm.s.ssid = de->phone_ids[0];
            rhmm->hmm.tmatid = de->ci_phone_ids[0];
            rhmm->diphone = de->phone_ids[0];
            rhmm->ciphone = de->ci_phone_ids[0];

            ngs->n_root_chan++;
        }
        else
            rhmm = &(ngs->root_chan[ngs->first_phone_rchan_map[de->phone_ids[0]]]);

        /* Now, rhmm = root channel for w.  Go on to remaining phones */
        if (de->len == 2) {
            /* Next phone is the last; not kept in tree; add w to penult_phn_wid set */
            if ((j = rhmm->penult_phn_wid) < 0)
                rhmm->penult_phn_wid = w;
            else {
                for (; ngs->homophone_set[j] >= 0; j = ngs->homophone_set[j]);
                ngs->homophone_set[j] = w;
            }
        }
        else {
            /* Add remaining phones, except the last, to tree */
            ph = de->phone_ids[1];
            hmm = rhmm->next;
            if (hmm == NULL) {
                rhmm->next = hmm = (chan_t *) listelem_alloc(sizeof(*hmm));
                init_nonroot_chan(ngs, hmm, ph, de->ci_phone_ids[1]);
                ngs->n_nonroot_chan++;
            }
            else {
                chan_t *prev_hmm = NULL;

                for (; hmm && (hmm->hmm.s.ssid != ph); hmm = hmm->alt)
                    prev_hmm = hmm;
                if (!hmm) {     /* thanks, rkm! */
                    prev_hmm->alt = hmm = listelem_alloc(sizeof(*hmm));
                    init_nonroot_chan(ngs, hmm, ph, de->ci_phone_ids[1]);
                    ngs->n_nonroot_chan++;
                }
            }
            /* de->phone_ids[1] now in tree; pointed to by hmm */

            for (p = 2; p < de->len - 1; p++) {
                ph = de->phone_ids[p];
                if (!hmm->next) {
                    hmm->next = listelem_alloc(sizeof(*hmm->next));
                    hmm = hmm->next;
                    init_nonroot_chan(ngs, hmm, ph, de->ci_phone_ids[p]);
                    ngs->n_nonroot_chan++;
                }
                else {
                    chan_t *prev_hmm = NULL;

                    for (hmm = hmm->next; hmm && (hmm->hmm.s.ssid != ph);
                         hmm = hmm->alt)
                        prev_hmm = hmm;
                    if (!hmm) { /* thanks, rkm! */
                        prev_hmm->alt = hmm = listelem_alloc(sizeof(*hmm));
                        init_nonroot_chan(ngs, hmm, ph, de->ci_phone_ids[p]);
                        ngs->n_nonroot_chan++;
                    }
                }
            }

            /* All but last phone of w in tree; add w to hmm->info.penult_phn_wid set */
            if ((j = hmm->info.penult_phn_wid) < 0)
                hmm->info.penult_phn_wid = w;
            else {
                for (; ngs->homophone_set[j] >= 0; j = ngs->homophone_set[j]);
                ngs->homophone_set[j] = w;
            }
        }
    }

    ngs->n_1ph_words = ngs->n_1ph_LMwords;
    ngs->n_1ph_LMwords++;            /* including </s> */

    /* FIXME: I'm not really sure why n_1ph_words got reset above. */
    for (w = dict_to_id(ngs->dict, "</s>"); w < n_words; ++w) {
        de = ngs->dict->dict_list[w];
        /* Skip any non-fillers that aren't in the LM. */
        /* FIXME: Not the best way to tell if it's a filler. */
        if ((!w >= ngs->silence_wid)
            && (!ngram_model_set_known_wid(ngs->lmset, de->wid)))
            continue;
        ngs->single_phone_wid[ngs->n_1ph_words++] = w;
    }

    if (ngs->n_nonroot_chan >= ngs->max_nonroot_chan) {
        /* Give some room for channels for new words added dynamically at run time */
        ngs->max_nonroot_chan = ngs->n_nonroot_chan + 128;
        E_INFO("max nonroot chan increased to %d\n", ngs->max_nonroot_chan);

        /* Free old active channel list array if any and allocate new one */
        if (ngs->active_chan_list)
            ckd_free_2d(ngs->active_chan_list);
        ngs->active_chan_list = ckd_calloc_2d(2, ngs->max_nonroot_chan,
                                              sizeof(**ngs->active_chan_list));
    }

    E_INFO("%d root, %d non-root channels, %d single-phone words\n",
           ngs->n_root_chan, ngs->n_nonroot_chan, ngs->n_1ph_words);
}

static void
reinit_search_subtree(chan_t * hmm)
{
    chan_t *child, *sibling;

    /* First free all children under hmm */
    for (child = hmm->next; child; child = sibling) {
        sibling = child->alt;
        reinit_search_subtree(child);
    }

    /* Now free hmm */
    hmm_deinit(&hmm->hmm);
    listelem_free(hmm, sizeof(*hmm));
}

/*
 * Delete search tree by freeing all interior channels within search tree and
 * restoring root channel state to the init state (i.e., just after init_search_tree()).
 */
static void
reinit_search_tree(ngram_search_t *ngs)
{
    int32 i;
    chan_t *hmm, *sibling;

    for (i = 0; i < ngs->n_root_chan; i++) {
        hmm = ngs->root_chan[i].next;

        while (hmm) {
            sibling = hmm->alt;
            reinit_search_subtree(hmm);
            hmm = sibling;
        }

        ngs->root_chan[i].penult_phn_wid = -1;
        ngs->root_chan[i].next = NULL;
    }
    ngs->n_nonroot_chan = 0;
}

void
ngram_fwdtree_init(ngram_search_t *ngs)
{
    init_search_tree(ngs);
    create_search_tree(ngs);
}

void
ngram_fwdtree_deinit(ngram_search_t *ngs)
{
    int i, w, n_words;

    n_words = ngs->dict->dict_entry_count;
    /* Reset non-root channels. */
    reinit_search_tree(ngs);

    /* Now deallocate all the root channels too. */
    for (i = 0; i < ngs->n_root_chan_alloc; i++) {
        hmm_deinit(&ngs->root_chan[i].hmm);
    }
    if (ngs->all_rhmm) {
        for (i = w = 0; w < n_words; ++w) {
            if (ngs->dict->dict_list[w]->len != 1)
                continue;
            hmm_deinit(&ngs->all_rhmm[i].hmm);
            ++i;
        }
        ckd_free(ngs->all_rhmm);
    }
    ngs->n_nonroot_chan = 0;
    ckd_free(ngs->first_phone_rchan_map);
    ckd_free(ngs->root_chan);
    ckd_free(ngs->homophone_set);
    ckd_free(ngs->single_phone_wid);
    ngs->max_nonroot_chan = 0;
    ckd_free_2d(ngs->active_chan_list);
}

void
ngram_fwdtree_start(ngram_search_t *ngs)
{
    int32 i, w, n_words;
    root_chan_t *rhmm;

    n_words = ngs->dict->dict_entry_count;

    /* Reset utterance statistics. */
    memset(&ngs->st, 0, sizeof(ngs->st));

    /* Reset backpointer table. */
    ngs->bpidx = 0;
    ngs->bss_head = 0;

    /* Reset word lattice. */
    for (i = 0; i < n_words; ++i)
        ngs->word_lat_idx[i] = NO_BP;

    /* Reset active HMM and word lists. */
    ngs->n_active_chan[0] = ngs->n_active_chan[1] = 0;
    ngs->n_active_word[0] = ngs->n_active_word[1] = 0;

    /* Reset scores. */
    ngs->best_score = 0;
    ngs->renormalized = 0;

    /* Reset other stuff. */
    for (i = 0; i < n_words; i++)
        ngs->last_ltrans[i].sf = -1;

    /* Clear the hypothesis string. */
    ckd_free(ngs->hyp_str);
    ngs->hyp_str = NULL;

    /* Reset the permanently allocated single-phone words, since they
     * may have junk left over in them from FWDFLAT. */
    for (i = 0; i < ngs->n_1ph_words; i++) {
        w = ngs->single_phone_wid[i];
        rhmm = (root_chan_t *) ngs->word_chan[w];
        hmm_clear(&rhmm->hmm);
    }

    /* Start search with <s>; word_chan[<s>] is permanently allocated */
    rhmm = (root_chan_t *) ngs->word_chan[dict_to_id(ngs->dict, "<s>")];
    hmm_clear(&rhmm->hmm);
    hmm_enter(&rhmm->hmm, 0, NO_BP, 0);
}

/*
 * Mark the active senones for all senones belonging to channels that are active in the
 * current frame.
 */
static void
compute_sen_active(ngram_search_t *ngs, int frame_idx)
{
    root_chan_t *rhmm;
    chan_t *hmm, **acl;
    int32 i, w, *awl;

    acmod_clear_active(ngs->acmod);

    /* Flag active senones for root channels */
    for (i = ngs->n_root_chan, rhmm = ngs->root_chan; i > 0; --i, rhmm++) {
        if (hmm_frame(&rhmm->hmm) == frame_idx)
            acmod_activate_hmm(ngs->acmod, &rhmm->hmm);
    }

    /* Flag active senones for nonroot channels in HMM tree */
    i = ngs->n_active_chan[frame_idx & 0x1];
    acl = ngs->active_chan_list[frame_idx & 0x1];
    for (hmm = *(acl++); i > 0; --i, hmm = *(acl++)) {
        acmod_activate_hmm(ngs->acmod, &hmm->hmm);
    }

    /* Flag active senones for individual word channels */
    i = ngs->n_active_word[frame_idx & 0x1];
    awl = ngs->active_word_list[frame_idx & 0x1];
    for (w = *(awl++); i > 0; --i, w = *(awl++)) {
        for (hmm = ngs->word_chan[w]; hmm; hmm = hmm->next) {
            acmod_activate_hmm(ngs->acmod, &hmm->hmm);
        }
    }
    for (i = 0; i < ngs->n_1ph_words; i++) {
        w = ngs->single_phone_wid[i];
        rhmm = (root_chan_t *) ngs->word_chan[w];

        if (hmm_frame(&rhmm->hmm) == frame_idx)
            acmod_activate_hmm(ngs->acmod, &rhmm->hmm);
    }
}

static void
renormalize_scores(ngram_search_t *ngs, int frame_idx, ascr_t norm)
{
    root_chan_t *rhmm;
    chan_t *hmm, **acl;
    int32 i, w, *awl;

    /* Renormalize root channels */
    for (i = ngs->n_root_chan, rhmm = ngs->root_chan; i > 0; --i, rhmm++) {
        if (hmm_frame(&rhmm->hmm) == frame_idx) {
            hmm_normalize(&rhmm->hmm, norm);
        }
    }

    /* Renormalize nonroot channels in HMM tree */
    i = ngs->n_active_chan[frame_idx & 0x1];
    acl = ngs->active_chan_list[frame_idx & 0x1];
    for (hmm = *(acl++); i > 0; --i, hmm = *(acl++)) {
        hmm_normalize(&hmm->hmm, norm);
    }

    /* Renormalize individual word channels */
    i = ngs->n_active_word[frame_idx & 0x1];
    awl = ngs->active_word_list[frame_idx & 0x1];
    for (w = *(awl++); i > 0; --i, w = *(awl++)) {
        for (hmm = ngs->word_chan[w]; hmm; hmm = hmm->next) {
            hmm_normalize(&hmm->hmm, norm);
        }
    }
    for (i = 0; i < ngs->n_1ph_words; i++) {
        w = ngs->single_phone_wid[i];
        rhmm = (root_chan_t *) ngs->word_chan[w];
        if (hmm_frame(&rhmm->hmm) == frame_idx) {
            hmm_normalize(&rhmm->hmm, norm);
        }
    }

    ngs->renormalized = TRUE;
}

static int32
eval_root_chan(ngram_search_t *ngs, int frame_idx)
{
    root_chan_t *rhmm;
    int32 i, bestscore;

    bestscore = WORST_SCORE;
    for (i = ngs->n_root_chan, rhmm = ngs->root_chan; i > 0; --i, rhmm++) {
        if (hmm_frame(&rhmm->hmm) == frame_idx) {
            int32 score = chan_v_eval(rhmm);
            if (bestscore < score)
                bestscore = score;
            ++ngs->st.n_root_chan_eval;
        }
    }
    return (bestscore);
}

static int32
eval_nonroot_chan(ngram_search_t *ngs, int frame_idx)
{
    chan_t *hmm, **acl;
    int32 i, bestscore;

    i = ngs->n_active_chan[frame_idx & 0x1];
    acl = ngs->active_chan_list[frame_idx & 0x1];
    bestscore = WORST_SCORE;
    ngs->st.n_nonroot_chan_eval += i;

    for (hmm = *(acl++); i > 0; --i, hmm = *(acl++)) {
        int32 score = chan_v_eval(hmm);
        assert(hmm_frame(&hmm->hmm) == frame_idx);
        if (bestscore < score)
            bestscore = score;
    }

    return bestscore;
}

static int32
eval_word_chan(ngram_search_t *ngs, int frame_idx)
{
    root_chan_t *rhmm;
    chan_t *hmm;
    int32 i, w, bestscore, *awl, j, k;

    k = 0;
    bestscore = WORST_SCORE;
    awl = ngs->active_word_list[frame_idx & 0x1];

    i = ngs->n_active_word[frame_idx & 0x1];
    for (w = *(awl++); i > 0; --i, w = *(awl++)) {
        assert(ngs->word_active[w] != 0);
        ngs->word_active[w] = 0;
        assert(ngs->word_chan[w] != NULL);

        for (hmm = ngs->word_chan[w]; hmm; hmm = hmm->next) {
            int32 score;

            assert(hmm_frame(&hmm->hmm) == frame_idx);
            score = chan_v_eval(hmm);
            /*printf("eval word chan %d score %d\n", w, score); */

            if (bestscore < score)
                bestscore = score;

            k++;
        }
    }

    /* Similarly for statically allocated single-phone words */
    j = 0;
    for (i = 0; i < ngs->n_1ph_words; i++) {
        int32 score;

        w = ngs->single_phone_wid[i];
        rhmm = (root_chan_t *) ngs->word_chan[w];
        if (hmm_frame(&rhmm->hmm) < frame_idx)
            continue;

        score = chan_v_eval(rhmm);
        /* printf("eval 1ph word chan %d score %d\n", w, score); */
        if (bestscore < score && w != ngs->finish_wid)
            bestscore = score;

        j++;
    }

    ngs->st.n_last_chan_eval += k + j;
    ngs->st.n_nonroot_chan_eval += k + j;
    ngs->st.n_word_lastchan_eval +=
        ngs->n_active_word[frame_idx & 0x1] + j;

    return bestscore;
}

static ascr_t
evaluate_channels(ngram_search_t *ngs, ascr_t const *senone_scores, int frame_idx)
{
    int32 bs;

    hmm_context_set_senscore(ngs->hmmctx, senone_scores);
    ngs->best_score = eval_root_chan(ngs, frame_idx);
    if ((bs = eval_nonroot_chan(ngs, frame_idx)) > ngs->best_score)
        ngs->best_score = bs;
    if ((bs = eval_word_chan(ngs, frame_idx)) > ngs->best_score)
        ngs->best_score = bs;
    ngs->last_phone_best_score = bs;

    return ngs->best_score;
}

/*
 * Prune currently active root channels for next frame.  Also, perform exit
 * transitions out of them and activate successors.
 * score[] of pruned root chan set to WORST_SCORE elsewhere.
 */
static void
prune_root_chan(ngram_search_t *ngs, int frame_idx)
{
    root_chan_t *rhmm;
    chan_t *hmm;
    int32 i, nf, w;
    int32 thresh, newphone_thresh, lastphn_thresh, newphone_score;
    chan_t **nacl;              /* next active list */
    lastphn_cand_t *candp;
    dict_entry_t *de;

    nf = frame_idx + 1;
    thresh = ngs->best_score + ngs->dynamic_beam;
    newphone_thresh =
        ngs->best_score + (ngs->dynamic_beam >
                           ngs->pbeam ? ngs->dynamic_beam :
                           ngs->pbeam);
    lastphn_thresh =
        ngs->best_score + (ngs->dynamic_beam >
                           ngs->lpbeam ? ngs->dynamic_beam :
                           ngs->lpbeam);
    nacl = ngs->active_chan_list[nf & 0x1];

    for (i = 0, rhmm = ngs->root_chan; i < ngs->n_root_chan; i++, rhmm++) {
        /* First check if this channel was active in current frame */
        if (hmm_frame(&rhmm->hmm) < frame_idx)
            continue;

        if (hmm_bestscore(&rhmm->hmm) > thresh) {
            hmm_frame(&rhmm->hmm) = nf;  /* rhmm will be active in next frame */

            /* transitions out of this root channel */
            newphone_score = hmm_out_score(&rhmm->hmm) + ngs->pip;
            if (newphone_score > newphone_thresh) {
                /* transition to all next-level channels in the HMM tree */
                for (hmm = rhmm->next; hmm; hmm = hmm->alt) {
                    if ((hmm_frame(&hmm->hmm) < frame_idx)
                        || (hmm_in_score(&hmm->hmm)
                            < newphone_score)) {
                        hmm_enter(&hmm->hmm, newphone_score,
                                  hmm_out_history(&rhmm->hmm), nf);
                        *(nacl++) = hmm;
                    }
                }

                /*
                 * Transition to last phone of all words for which this is the
                 * penultimate phone (the last phones may need multiple right contexts).
                 * Remember to remove the temporary newword_penalty.
                 */
                if (newphone_score > lastphn_thresh) {
                    for (w = rhmm->penult_phn_wid; w >= 0;
                         w = ngs->homophone_set[w]) {
                        de = ngs->dict->dict_list[w];
                        candp = ngs->lastphn_cand + ngs->n_lastphn_cand;
                        ngs->n_lastphn_cand++;
                        candp->wid = w;
                        candp->score =
                            newphone_score - ngs->nwpen;
                        candp->bp = hmm_out_history(&rhmm->hmm);
                    }
                }
            }
        }
    }
    ngs->n_active_chan[nf & 0x1] = nacl - ngs->active_chan_list[nf & 0x1];
}

/*
 * Prune currently active nonroot channels in HMM tree for next frame.  Also, perform
 * exit transitions out of such channels and activate successors.
 */
static void
prune_nonroot_chan(ngram_search_t *ngs, int frame_idx)
{
    chan_t *hmm, *nexthmm;
    int32 nf, w, i;
    int32 thresh, newphone_thresh, lastphn_thresh, newphone_score;
    chan_t **acl, **nacl;       /* active list, next active list */
    lastphn_cand_t *candp;
    dict_entry_t *de;

    nf = frame_idx + 1;

    thresh = ngs->best_score + ngs->dynamic_beam;
    newphone_thresh =
        ngs->best_score + (ngs->dynamic_beam >
                           ngs->pbeam ? ngs->dynamic_beam :
                           ngs->pbeam);
    lastphn_thresh =
        ngs->best_score + (ngs->dynamic_beam >
                           ngs->lpbeam ? ngs->dynamic_beam :
                           ngs->lpbeam);

    acl = ngs->active_chan_list[frame_idx & 0x1];   /* currently active HMMs in tree */
    nacl = ngs->active_chan_list[nf & 0x1] + ngs->n_active_chan[nf & 0x1];

    for (i = ngs->n_active_chan[frame_idx & 0x1], hmm = *(acl++); i > 0;
         --i, hmm = *(acl++)) {
        assert(hmm_frame(&hmm->hmm) >= frame_idx);

        if (hmm_bestscore(&hmm->hmm) > thresh) {
            /* retain this channel in next frame */
            if (hmm_frame(&hmm->hmm) != nf) {
                hmm_frame(&hmm->hmm) = nf;
                *(nacl++) = hmm;
            }

            /* transitions out of this channel */
            newphone_score = hmm_out_score(&hmm->hmm) + ngs->pip;
            if (newphone_score > newphone_thresh) {
                /* transition to all next-level channel in the HMM tree */
                for (nexthmm = hmm->next; nexthmm; nexthmm = nexthmm->alt) {
                    if ((hmm_frame(&nexthmm->hmm) < frame_idx)
                        || (hmm_in_score(&nexthmm->hmm) < newphone_score)) {
                        if (hmm_frame(&nexthmm->hmm) != nf) {
                            /* Keep this HMM on the active list */
                            *(nacl++) = nexthmm;
                        }
                        hmm_enter(&nexthmm->hmm, newphone_score,
                                  hmm_out_history(&hmm->hmm), nf);
                    }
                }

                /*
                 * Transition to last phone of all words for which this is the
                 * penultimate phone (the last phones may need multiple right contexts).
                 * Remember to remove the temporary newword_penalty.
                 */
                if (newphone_score > lastphn_thresh) {
                    for (w = hmm->info.penult_phn_wid; w >= 0;
                         w = ngs->homophone_set[w]) {
                        de = ngs->dict->dict_list[w];
                        candp = ngs->lastphn_cand + ngs->n_lastphn_cand;
                        ngs->n_lastphn_cand++;
                        candp->wid = w;
                        candp->score =
                            newphone_score - ngs->nwpen;
                        candp->bp = hmm_out_history(&hmm->hmm);
                    }
                }
            }
        }
        else if (hmm_frame(&hmm->hmm) != nf) {
            hmm_clear_scores(&hmm->hmm);
        }
    }
    ngs->n_active_chan[nf & 0x1] = nacl - ngs->active_chan_list[nf & 0x1];
}

/*
 * Allocate last phone channels for all possible right contexts for word w.  (Some
 * may already exist.)
 * (NOTE: Assume that w uses context!!)
 */
static void
alloc_all_rc(ngram_search_t *ngs, int32 w)
{
    dict_entry_t *de;
    chan_t *hmm, *thmm;
    int32 *sseq_rc;             /* list of sseqid for all possible right context for w */
    int32 i;

    de = ngs->dict->dict_list[w];

    assert(de->mpx);

    sseq_rc = ngs->dict->rcFwdTable[de->phone_ids[de->len - 1]];

    hmm = ngs->word_chan[w];
    if ((hmm == NULL) || (hmm->hmm.s.ssid != *sseq_rc)) {
        hmm = (chan_t *) listelem_alloc(sizeof(chan_t));
        hmm->next = ngs->word_chan[w];
        ngs->word_chan[w] = hmm;

        hmm->info.rc_id = 0;
        hmm->ciphone = de->ci_phone_ids[de->len - 1];
        hmm_init(ngs->hmmctx, &hmm->hmm, FALSE, *sseq_rc, hmm->ciphone);
    }
    for (i = 1, sseq_rc++; *sseq_rc >= 0; sseq_rc++, i++) {
        if ((hmm->next == NULL) || (hmm->next->hmm.s.ssid != *sseq_rc)) {
            thmm = (chan_t *) listelem_alloc(sizeof(chan_t));
            thmm->next = hmm->next;
            hmm->next = thmm;
            hmm = thmm;

            hmm->info.rc_id = i;
            hmm->ciphone = de->ci_phone_ids[de->len - 1];
            hmm_init(ngs->hmmctx, &hmm->hmm, FALSE, *sseq_rc, hmm->ciphone);
        }
        else
            hmm = hmm->next;
    }
}

static void
free_all_rc(ngram_search_t *ngs, int32 w)
{
    chan_t *hmm, *thmm;

    for (hmm = ngs->word_chan[w]; hmm; hmm = thmm) {
        thmm = hmm->next;
        hmm_deinit(&hmm->hmm);
        listelem_free(hmm, sizeof(chan_t));
    }
    ngs->word_chan[w] = NULL;
}

/*
 * Execute the transition into the last phone for all candidates words emerging from
 * the HMM tree.  Attach LM scores to such transitions.
 * (Executed after pruning root and non-root, but before pruning word-chan.)
 */
static void
last_phone_transition(ngram_search_t *ngs, int frame_idx)
{
    int32 i, j, k, nf, bp, bplast, w;
    lastphn_cand_t *candp;
    int32 *nawl;
    int32 thresh;
    int32 *rcpermtab, ciph0;
    int32 bestscore, dscr;
    dict_entry_t *de;
    chan_t *hmm;
    bptbl_t *bpe;
    int32 n_cand_sf = 0;

    nf = frame_idx + 1;
    nawl = ngs->active_word_list[nf & 0x1];
    ngs->st.n_lastphn_cand_utt += ngs->n_lastphn_cand;

    /* For each candidate word (entering its last phone) */
    /* If best LM score and bp for candidate known use it, else sort cands by startfrm */
    for (i = 0, candp = ngs->lastphn_cand; i < ngs->n_lastphn_cand; i++, candp++) {
        /* Backpointer entry for it. */
        bpe = &(ngs->bp_table[candp->bp]);
        /* Right context phone table. */
        rcpermtab =
            (bpe->r_diph >=
             0) ? ngs->dict->rcFwdPermTable[bpe->r_diph] : ngs->zeroPermTab;

        /* Subtract starting score for candidate, leave it with only word score */
        de = ngs->dict->dict_list[candp->wid];
        ciph0 = de->ci_phone_ids[0];
        candp->score -= ngs->bscore_stack[bpe->s_idx + rcpermtab[ciph0]];

        /*
         * If this candidate not occurred in an earlier frame, prepare for finding
         * best transition score into last phone; sort by start frame.
         */
        /* i.e. if we don't have an entry in last_ltrans for this
         * <word,sf>, then create one */
        if (ngs->last_ltrans[candp->wid].sf != bpe->frame + 1) {
            /* Look for an entry in cand_sf matching the backpointer
             * for this candidate. */
            for (j = 0; j < n_cand_sf; j++) {
                if (ngs->cand_sf[j].bp_ef == bpe->frame)
                    break;
            }
            /* Oh, we found one, so chain onto it. */
            if (j < n_cand_sf)
                candp->next = ngs->cand_sf[j].cand;
            else {
                /* Nope, let's make a new one, allocating cand_sf if necessary. */
                if (n_cand_sf >= ngs->cand_sf_alloc) {
                    if (ngs->cand_sf_alloc == 0) {
                        ngs->cand_sf =
                            ckd_calloc(CAND_SF_ALLOCSIZE,
                                       sizeof(*ngs->cand_sf));
                        ngs->cand_sf_alloc = CAND_SF_ALLOCSIZE;
                    }
                    else {
                        ngs->cand_sf_alloc += CAND_SF_ALLOCSIZE;
                        ngs->cand_sf = ckd_realloc(ngs->cand_sf,
                                                   ngs->cand_sf_alloc
                                                   * sizeof(*ngs->cand_sf));
                        E_INFO("cand_sf[] increased to %d entries\n",
                               ngs->cand_sf_alloc);
                    }
                }

                /* Use the newly created cand_sf. */
                j = n_cand_sf++;
                candp->next = -1; /* End of the chain. */
                ngs->cand_sf[j].bp_ef = bpe->frame;
            }
            /* Update it to point to this candidate. */
            ngs->cand_sf[j].cand = i;

            ngs->last_ltrans[candp->wid].dscr = WORST_SCORE;
            ngs->last_ltrans[candp->wid].sf = bpe->frame + 1;
        }
    }

    /* Compute best LM score and bp for new cands entered in the sorted lists above */
    for (i = 0; i < n_cand_sf; i++) {
        /* For the i-th unique end frame... */
        bp = ngs->bp_table_idx[ngs->cand_sf[i].bp_ef];
        bplast = ngs->bp_table_idx[ngs->cand_sf[i].bp_ef + 1] - 1;

        for (bpe = &(ngs->bp_table[bp]); bp <= bplast; bp++, bpe++) {
            if (!bpe->valid)
                continue;

            /* For each bp entry in the i-th end frame... */
            rcpermtab = (bpe->r_diph >= 0) ?
                ngs->dict->rcFwdPermTable[bpe->r_diph] : ngs->zeroPermTab;

            /* For each candidate at the start frame find bp->cand transition-score */
            for (j = ngs->cand_sf[i].cand; j >= 0; j = candp->next) {
                int32 n_used;
                candp = &(ngs->lastphn_cand[j]);
                de = ngs->dict->dict_list[candp->wid];
                ciph0 = de->ci_phone_ids[0];

                dscr = ngs->bscore_stack[bpe->s_idx + rcpermtab[ciph0]];
                dscr += ngram_tg_score(ngs->lmset, de->wid, bpe->real_wid,
                                       bpe->prev_real_wid, &n_used);

                if (ngs->last_ltrans[candp->wid].dscr < dscr) {
                    ngs->last_ltrans[candp->wid].dscr = dscr;
                    ngs->last_ltrans[candp->wid].bp = bp;
                }
            }
        }
    }

    /* Update best transitions for all candidates; also update best lastphone score */
    bestscore = ngs->last_phone_best_score;
    for (i = 0, candp = ngs->lastphn_cand; i < ngs->n_lastphn_cand; i++, candp++) {
        candp->score += ngs->last_ltrans[candp->wid].dscr;
        candp->bp = ngs->last_ltrans[candp->wid].bp;

        if (bestscore < candp->score)
            bestscore = candp->score;
    }
    ngs->last_phone_best_score = bestscore;

    /* At this pt, we know the best entry score (with LM component) for all candidates */
    thresh = bestscore + ngs->lponlybeam;
    for (i = ngs->n_lastphn_cand, candp = ngs->lastphn_cand; i > 0; --i, candp++) {
        if (candp->score > thresh) {
            w = candp->wid;

            alloc_all_rc(ngs, w);

            k = 0;
            for (hmm = ngs->word_chan[w]; hmm; hmm = hmm->next) {
                if ((hmm_frame(&hmm->hmm) < frame_idx)
                    || (hmm_in_score(&hmm->hmm) < candp->score)) {
                    assert(hmm_frame(&hmm->hmm) != nf);
                    hmm_enter(&hmm->hmm,
                              candp->score, candp->bp, nf);
                    k++;
                }
            }
            if (k > 0) {
                assert(!ngs->word_active[w]);
                assert(ngs->dict->dict_list[w]->len > 1);
                *(nawl++) = w;
                ngs->word_active[w] = 1;
            }
        }
    }
    ngs->n_active_word[nf & 0x1] = nawl - ngs->active_word_list[nf & 0x1];
}

/*
 * Prune currently active word channels for next frame.  Also, perform exit
 * transitions out of such channels and active successors.
 */
static void
prune_word_chan(ngram_search_t *ngs, int frame_idx)
{
    root_chan_t *rhmm;
    chan_t *hmm, *thmm;
    chan_t **phmmp;             /* previous HMM-pointer */
    int32 nf, w, i, k;
    int32 newword_thresh, lastphn_thresh;
    int32 *awl, *nawl;

    nf = frame_idx + 1;
    newword_thresh =
        ngs->last_phone_best_score + (ngs->dynamic_beam >
                                      ngs->wbeam ? ngs->dynamic_beam :
                                      ngs->wbeam);
    lastphn_thresh =
        ngs->last_phone_best_score + (ngs->dynamic_beam >
                                      ngs->lponlybeam ?
                                      ngs->dynamic_beam :
                                      ngs->lponlybeam);

    awl = ngs->active_word_list[frame_idx & 0x1];
    nawl = ngs->active_word_list[nf & 0x1] + ngs->n_active_word[nf & 0x1];

    /* Dynamically allocated last channels of multi-phone words */
    for (i = ngs->n_active_word[frame_idx & 0x1], w = *(awl++); i > 0;
         --i, w = *(awl++)) {
        k = 0;
        phmmp = &(ngs->word_chan[w]);
        for (hmm = ngs->word_chan[w]; hmm; hmm = thmm) {
            assert(hmm_frame(&hmm->hmm) >= frame_idx);

            thmm = hmm->next;
            if (hmm_bestscore(&hmm->hmm) > lastphn_thresh) {
                /* retain this channel in next frame */
                hmm_frame(&hmm->hmm) = nf;
                k++;
                phmmp = &(hmm->next);

                /* Could if ((! skip_alt_frm) || (frame_idx & 0x1)) the following */
                if (hmm_out_score(&hmm->hmm) > newword_thresh) {
                    /* can exit channel and recognize word */
                    save_bwd_ptr(ngs, frame_idx, w,
                                 hmm_out_score(&hmm->hmm),
                                 hmm_out_history(&hmm->hmm),
                                 hmm->info.rc_id);
                }
            }
            else if (hmm_frame(&hmm->hmm) == nf) {
                phmmp = &(hmm->next);
            }
            else {
                hmm_deinit(&hmm->hmm);
                listelem_free(hmm, sizeof(chan_t));
                *phmmp = thmm;
            }
        }
        if ((k > 0) && (!ngs->word_active[w])) {
            assert(ngs->dict->dict_list[w]->len > 1);
            *(nawl++) = w;
            ngs->word_active[w] = 1;
        }
    }
    ngs->n_active_word[nf & 0x1] = nawl - ngs->active_word_list[nf & 0x1];

    /*
     * Prune permanently allocated single-phone channels.
     * NOTES: score[] of pruned channels set to WORST_SCORE elsewhere.
     */
    for (i = 0; i < ngs->n_1ph_words; i++) {
        w = ngs->single_phone_wid[i];
        rhmm = (root_chan_t *) ngs->word_chan[w];
        if (hmm_frame(&rhmm->hmm) < frame_idx)
            continue;
        if (hmm_bestscore(&rhmm->hmm) > lastphn_thresh) {
            hmm_frame(&rhmm->hmm) = nf;

            /* Could if ((! skip_alt_frm) || (frame_idx & 0x1)) the following */
            if (hmm_out_score(&rhmm->hmm) > newword_thresh) {
                save_bwd_ptr(ngs, frame_idx, w,
                             hmm_out_score(&rhmm->hmm),
                             hmm_out_history(&rhmm->hmm), 0);
            }
        }
    }
}

static void
prune_channels(ngram_search_t *ngs, int frame_idx)
{
    /* Clear last phone candidate list. */
    ngs->n_lastphn_cand = 0;
    /* Set the dynamic beam based on maxhmmpf here. */
    ngs->dynamic_beam = ngs->beam;
    if (ngs->maxhmmpf != -1
        && ngs->st.n_root_chan_eval + ngs->st.n_nonroot_chan_eval > ngs->maxhmmpf) {
        /* Build a histogram to approximately prune them. */
        int32 bins[256], bw, nhmms, i;
        root_chan_t *rhmm;
        chan_t **acl, *hmm;

        /* Bins go from zero (best score) to edge of beam. */
        bw = -ngs->beam / 256;
        memset(bins, 0, sizeof(bins));
        /* For each active root channel. */
        for (i = 0, rhmm = ngs->root_chan; i < ngs->n_root_chan; i++, rhmm++) {
            int32 b;

            /* Put it in a bin according to its bestscore. */
            b = (ngs->best_score - hmm_bestscore(&rhmm->hmm)) / bw;
            if (b >= 256)
                b = 255;
            ++bins[b];
        }
        /* For each active non-root channel. */
        acl = ngs->active_chan_list[frame_idx & 0x1];       /* currently active HMMs in tree */
        for (i = ngs->n_active_chan[frame_idx & 0x1], hmm = *(acl++);
             i > 0; --i, hmm = *(acl++)) {
            int32 b;

            /* Put it in a bin according to its bestscore. */
            b = (ngs->best_score - hmm_bestscore(&hmm->hmm)) / bw;
            if (b >= 256)
                b = 255;
            ++bins[b];
        }
        /* Walk down the bins to find the new beam. */
        for (i = nhmms = 0; i < 256; ++i) {
            nhmms += bins[i];
            if (nhmms > ngs->maxhmmpf)
                break;
        }
        ngs->dynamic_beam = -(i * bw);
    }

    prune_root_chan(ngs, frame_idx);
    prune_nonroot_chan(ngs, frame_idx);
    last_phone_transition(ngs, frame_idx);
    prune_word_chan(ngs, frame_idx);
}

/**
 * Find trigram predecessors for a backpointer table entry.
 */
static void
cache_bptable_paths(ngram_search_t *ngs, int32 bp)
{
    int32 w, prev_bp;
    bptbl_t *bpe;

    bpe = &(ngs->bp_table[bp]);
    prev_bp = bp;
    w = bpe->wid;
    /* FIXME: This isn't the ideal way to tell if it's a filler. */
    while (w >= ngs->silence_wid) {
        prev_bp = ngs->bp_table[prev_bp].bp;
        w = ngs->bp_table[prev_bp].wid;
    }
    bpe->real_wid = ngs->dict->dict_list[w]->wid;

    prev_bp = ngs->bp_table[prev_bp].bp;
    bpe->prev_real_wid =
        (prev_bp != NO_BP) ? ngs->bp_table[prev_bp].real_wid : -1;
}

static void
save_bwd_ptr(ngram_search_t *ngs, int frame_idx,
             int32 w, int32 score, int32 path, int32 rc)
{
    int32 _bp_;

    _bp_ = ngs->word_lat_idx[w];
    if (_bp_ != NO_BP) {
        if (ngs->bp_table[_bp_].score < score) {
            if (ngs->bp_table[_bp_].bp != path) {
                ngs->bp_table[_bp_].bp = path;
                cache_bptable_paths(ngs, _bp_);
            }
            ngs->bp_table[_bp_].score = score;
        }
        ngs->bscore_stack[ngs->bp_table[_bp_].s_idx + rc] = score;
    }
    else {
        int32 i, rcsize, *bss;
        dict_entry_t *de;
        bptbl_t *bpe;

        /* Expand the backpointer tables if necessary. */
        if (ngs->bpidx >= ngs->bp_table_size) {
            ngs->bp_table_size *= 2;
            ngs->bp_table = ckd_realloc(ngs->bp_table,
                                        ngs->bp_table_size
                                        * sizeof(*ngs->bp_table));
            E_INFO("Resized backpointer table to %d entries\n", ngs->bp_table_size);
        }
        if (ngs->bss_head >= ngs->bscore_stack_size
            - bin_mdef_n_ciphone(ngs->acmod->mdef)) {
            ngs->bscore_stack_size *= 2;
            ngs->bscore_stack = ckd_realloc(ngs->bscore_stack,
                                            ngs->bscore_stack_size
                                            * sizeof(*ngs->bscore_stack));
            E_INFO("Resized score stack to %d entries\n", ngs->bscore_stack_size);
        }

        de = ngs->dict->dict_list[w];
        ngs->word_lat_idx[w] = ngs->bpidx;
        bpe = &(ngs->bp_table[ngs->bpidx]);
        bpe->wid = w;
        bpe->frame = frame_idx;
        bpe->bp = path;
        bpe->score = score;
        bpe->s_idx = ngs->bss_head;
        bpe->valid = TRUE;

        if ((de->len != 1) && (de->mpx)) {
            bpe->r_diph = de->phone_ids[de->len - 1];
            rcsize = ngs->dict->rcFwdSizeTable[bpe->r_diph];
        }
        else {
            bpe->r_diph = -1;
            rcsize = 1;
        }
        for (i = rcsize, bss = ngs->bscore_stack + ngs->bss_head; i > 0; --i, bss++)
            *bss = WORST_SCORE;
        ngs->bscore_stack[ngs->bss_head + rc] = score;
        cache_bptable_paths(ngs, ngs->bpidx);

        ngs->bpidx++;
        ngs->bss_head += rcsize;
    }
}


/*
 * Limit the number of word exits in each frame to maxwpf.  And also limit the number of filler
 * words to 1.
 */
static void
bptable_maxwpf(ngram_search_t *ngs, int frame_idx)
{
    int32 bp, n;
    int32 bestscr, worstscr;
    bptbl_t *bpe, *bestbpe, *worstbpe;

    /* Don't prune if no pruing. */
    if (ngs->maxwpf == -1 || ngs->maxwpf == ngs->dict->dict_entry_count)
        return;

    /* Allow only one filler word exit (the best) per frame */
    bestscr = (int32) 0x80000000;
    bestbpe = NULL;
    n = 0;
    for (bp = ngs->bp_table_idx[frame_idx]; bp < ngs->bpidx; bp++) {
        bpe = &(ngs->bp_table[bp]);
        /* FIXME: Not the ideal way to tell if this is a filler word. */
        if (bpe->wid >= ngs->silence_wid) {
            if (bpe->score > bestscr) {
                bestscr = bpe->score;
                bestbpe = bpe;
            }
            bpe->valid = FALSE; /* Flag to indicate invalidation */
            n++;                /* No. of filler words */
        }
    }
    /* Restore bestbpe to valid state */
    if (bestbpe != NULL) {
        bestbpe->valid = TRUE;
        --n;
    }

    /* Allow up to maxwpf best entries to survive; mark the remaining with valid = 0 */
    n = (ngs->bpidx
         - ngs->bp_table_idx[frame_idx]) - n;  /* No. of entries after limiting fillers */
    for (; n > ngs->maxwpf; --n) {
        /* Find worst BPTable entry */
        worstscr = (int32) 0x7fffffff;
        worstbpe = NULL;
        for (bp = ngs->bp_table_idx[frame_idx]; (bp < ngs->bpidx); bp++) {
            bpe = &(ngs->bp_table[bp]);
            if (bpe->valid && (bpe->score < worstscr)) {
                worstscr = bpe->score;
                worstbpe = bpe;
            }
        }
        /* FIXME: Don't panic! */
        if (worstbpe == NULL)
            E_FATAL("PANIC: No worst BPtable entry remaining\n");
        worstbpe->valid = 0;
    }
}

static void
word_transition(ngram_search_t *ngs, int frame_idx)
{
    int32 i, k, bp, w, nf;
    int32 rc;
    int32 *rcss;                /* right context score stack */
    int32 *rcpermtab;
    int32 thresh, newscore;
    bptbl_t *bpe;
    dict_entry_t *pde, *de;     /* previous dict entry, dict entry */
    root_chan_t *rhmm;
    struct bestbp_rc_s *bestbp_rc_ptr;
    int32 last_ciph;
    int32 ssid;

    /*
     * Transition to start of new word instances (HMM tree roots); but only if words
     * other than </s> finished here.
     * But, first, find the best starting score for each possible right context phone.
     */
    for (i = bin_mdef_n_ciphone(ngs->acmod->mdef) - 1; i >= 0; --i)
        ngs->bestbp_rc[i].score = WORST_SCORE;
    k = 0;
    for (bp = ngs->bp_table_idx[frame_idx]; bp < ngs->bpidx; bp++) {
        bpe = &(ngs->bp_table[bp]);
        ngs->word_lat_idx[bpe->wid] = NO_BP;

        if (bpe->wid == ngs->finish_wid)
            continue;
        k++;

        de = ngs->dict->dict_list[bpe->wid];
        rcpermtab =
            (bpe->r_diph >=
             0) ? ngs->dict->rcFwdPermTable[bpe->r_diph] : ngs->zeroPermTab;
        last_ciph = de->ci_phone_ids[de->len - 1];

        rcss = &(ngs->bscore_stack[bpe->s_idx]);
        for (rc = bin_mdef_n_ciphone(ngs->acmod->mdef) - 1; rc >= 0; --rc) {
            if (rcss[rcpermtab[rc]] > ngs->bestbp_rc[rc].score) {
                ngs->bestbp_rc[rc].score = rcss[rcpermtab[rc]];
                ngs->bestbp_rc[rc].path = bp;
                ngs->bestbp_rc[rc].lc = last_ciph;
            }
        }
    }
    if (k == 0)
        return;

    nf = frame_idx + 1;
    thresh = ngs->best_score + ngs->dynamic_beam;
    /*
     * Hypothesize successors to words finished in this frame.
     * Main dictionary, multi-phone words transition to HMM-trees roots.
     */
    for (i = ngs->n_root_chan, rhmm = ngs->root_chan; i > 0; --i, rhmm++) {
        bestbp_rc_ptr = &(ngs->bestbp_rc[rhmm->ciphone]);

        newscore = bestbp_rc_ptr->score + ngs->nwpen + ngs->pip;
        if (newscore > thresh) {
            if ((hmm_frame(&rhmm->hmm) < frame_idx)
                || (hmm_in_score(&rhmm->hmm) < newscore)) {
                ssid =
                    ngs->dict->lcFwdTable[rhmm->diphone][bestbp_rc_ptr->lc];
                hmm_enter(&rhmm->hmm, newscore,
                          bestbp_rc_ptr->path, nf);
                if (hmm_is_mpx(&rhmm->hmm)) {
                    rhmm->hmm.s.mpx_ssid[0] = ssid;
                }
            }
        }
    }

    /*
     * Single phone words; no right context for these.  Cannot use bestbp_rc as
     * LM scores have to be included.  First find best transition to these words.
     */
    for (i = 0; i < ngs->n_1ph_LMwords; i++) {
        w = ngs->single_phone_wid[i];
        ngs->last_ltrans[w].dscr = (int32) 0x80000000;
    }
    for (bp = ngs->bp_table_idx[frame_idx]; bp < ngs->bpidx; bp++) {
        bpe = &(ngs->bp_table[bp]);
        if (!bpe->valid)
            continue;

        rcpermtab =
            (bpe->r_diph >=
             0) ? ngs->dict->rcFwdPermTable[bpe->r_diph] : ngs->zeroPermTab;
        rcss = ngs->bscore_stack + bpe->s_idx;

        for (i = 0; i < ngs->n_1ph_LMwords; i++) {
            int32 n_used;
            w = ngs->single_phone_wid[i];
            de = ngs->dict->dict_list[w];

            newscore = rcss[rcpermtab[de->ci_phone_ids[0]]];
            newscore += ngram_tg_score(ngs->lmset, de->wid, bpe->real_wid,
                                       bpe->prev_real_wid, &n_used);

            if (ngs->last_ltrans[w].dscr < newscore) {
                ngs->last_ltrans[w].dscr = newscore;
                ngs->last_ltrans[w].bp = bp;
            }
        }
    }

    /* Now transition to in-LM single phone words */
    for (i = 0; i < ngs->n_1ph_LMwords; i++) {
        w = ngs->single_phone_wid[i];
        rhmm = (root_chan_t *) ngs->word_chan[w];
        if ((newscore = ngs->last_ltrans[w].dscr + ngs->pip) > thresh) {
            bpe = ngs->bp_table + ngs->last_ltrans[w].bp;
            pde = ngs->dict->dict_list[bpe->wid];

            if ((hmm_frame(&rhmm->hmm) < frame_idx)
                || (hmm_in_score(&rhmm->hmm) < newscore)) {
                hmm_enter(&rhmm->hmm,
                          newscore, ngs->last_ltrans[w].bp, nf);
                if (hmm_is_mpx(&rhmm->hmm)) {
                    rhmm->hmm.s.mpx_ssid[0] =
                        ngs->dict->lcFwdTable[rhmm->diphone][pde->ci_phone_ids[pde->len - 1]];
                }
            }
        }
    }

    /* Remaining words: <sil>, noise words.  No mpx for these! */
    bestbp_rc_ptr = &(ngs->bestbp_rc[ngs->acmod->mdef->sil]);
    newscore = bestbp_rc_ptr->score + ngs->silpen + ngs->pip;
    if (newscore > thresh) {
        w = ngs->silence_wid;
        rhmm = (root_chan_t *) ngs->word_chan[w];
        if ((hmm_frame(&rhmm->hmm) < frame_idx)
            || (hmm_in_score(&rhmm->hmm) < newscore)) {
            hmm_enter(&rhmm->hmm,
                      newscore, bestbp_rc_ptr->path, nf);
        }
    }
    newscore = bestbp_rc_ptr->score + ngs->fillpen + ngs->pip;
    if (newscore > thresh) {
        /* FIXME FIXME: This depends on having the noise words
         * immediately following silence in the dictionary... */
        for (w = ngs->silence_wid + 1; w < ngs->dict->dict_entry_count; w++) {
            rhmm = (root_chan_t *) ngs->word_chan[w];
            if ((hmm_frame(&rhmm->hmm) < frame_idx)
                || (hmm_in_score(&rhmm->hmm) < newscore)) {
                hmm_enter(&rhmm->hmm,
                          newscore, bestbp_rc_ptr->path, nf);
            }
        }
    }
}

static void
deactivate_channels(ngram_search_t *ngs, int frame_idx)
{
    root_chan_t *rhmm;
    int i;

    /* Clear score[] of pruned root channels */
    for (i = ngs->n_root_chan, rhmm = ngs->root_chan; i > 0; --i, rhmm++) {
        if (hmm_frame(&rhmm->hmm) == frame_idx) {
            hmm_clear_scores(&rhmm->hmm);
        }
    }
    /* Clear score[] of pruned single-phone channels */
    for (i = 0; i < ngs->n_1ph_words; i++) {
        int32 w = ngs->single_phone_wid[i];
        rhmm = (root_chan_t *) ngs->word_chan[w];
        if (hmm_frame(&rhmm->hmm) == frame_idx) {
            hmm_clear_scores(&rhmm->hmm);
        }
    }
}

int
ngram_fwdtree_search(ngram_search_t *ngs)
{
    ascr_t const *senscr;
    int frame_idx, best_senid;
    ascr_t best_senscr;

    /* Determine if we actually have a frame to process. */
    if (ngs->acmod->n_feat_frame == 0)
        return 0;

    /* Activate our HMMs for the current frame if need be. */
    if (!ngs->acmod->compallsen)
        compute_sen_active(ngs, acmod_frame_idx(ngs->acmod));

    /* Compute GMM scores for the current frame. */
    senscr = acmod_score(ngs->acmod, &frame_idx,
                         &best_senscr, &best_senid);
    ngs->st.n_senone_active_utt += ngs->acmod->n_senone_active;

    /* Mark backpointer table for current frame. */
    ngram_search_mark_bptable(ngs, frame_idx);

    /* Renormalize if necessary (FIXME: Make sure to test this) */
    if (ngs->best_score + (2 * ngs->beam) < WORST_SCORE) {
        E_INFO("Renormalizing Scores at frame %d, best score %d\n",
               frame_idx, ngs->best_score);
        renormalize_scores(ngs, frame_idx, ngs->best_score);
    }

    /* Evaluate HMMs */
    evaluate_channels(ngs, senscr, frame_idx);

    /* Prune HMMs and do phone transitions. */
    prune_channels(ngs, frame_idx);

    /* Do absolute pruning on word exits. */
    bptable_maxwpf(ngs, frame_idx);

    /* Do word transitions. */
    word_transition(ngs, frame_idx);

    /* Deactivate pruned HMMs. */
    deactivate_channels(ngs, frame_idx);

    /* Return the number of frames processed. */
    return 1;
}

void
ngram_fwdtree_finish(ngram_search_t *ngs)
{
    int32 i, w, cf, *awl;
    root_chan_t *rhmm;
    chan_t *hmm, **acl;

    /* This is the number of frames processed. */
    cf = acmod_frame_idx(ngs->acmod);
    /* Add a mark in the backpointer table for one past the final frame. */
    ngram_search_mark_bptable(ngs, cf);

    /* Deactivate channels lined up for the next frame */
    /* First, root channels of HMM tree */
    for (i = ngs->n_root_chan, rhmm = ngs->root_chan; i > 0; --i, rhmm++) {
        hmm_clear(&rhmm->hmm);
    }

    /* nonroot channels of HMM tree */
    i = ngs->n_active_chan[cf & 0x1];
    acl = ngs->active_chan_list[cf & 0x1];
    for (hmm = *(acl++); i > 0; --i, hmm = *(acl++)) {
        hmm_clear(&hmm->hmm);
    }

    /* word channels */
    i = ngs->n_active_word[cf & 0x1];
    awl = ngs->active_word_list[cf & 0x1];
    for (w = *(awl++); i > 0; --i, w = *(awl++)) {
        /* Don't accidentally free single-phone words! */
        if (ngs->dict->dict_list[w]->len == 1)
            continue;
        ngs->word_active[w] = 0;
        if (ngs->word_chan[w] == NULL)
            continue;
        free_all_rc(ngs, w);
    }

    /*
     * The previous search code did a postprocessing of the
     * backpointer table here, but we will postpone this until it is
     * absolutely necessary, i.e. when generating a word graph.
     * Likewise we don't actually have to decide what the exit word is
     * until somebody requests a backtrace.
     */

    /* Print out some statistics. */
    if (cf > 0) {
        E_INFO("%8d words recognized (%d/fr)\n",
               ngs->bpidx, (ngs->bpidx + (cf >> 1)) / (cf + 1));
        E_INFO("%8d senones evaluated (%d/fr)\n", ngs->st.n_senone_active_utt,
               (ngs->st.n_senone_active_utt + (cf >> 1)) / (cf + 1));
        E_INFO("%8d channels searched (%d/fr), %d 1st, %d last\n",
               ngs->st.n_root_chan_eval + ngs->st.n_nonroot_chan_eval,
               (ngs->st.n_root_chan_eval + ngs->st.n_nonroot_chan_eval) / (cf + 1),
               ngs->st.n_root_chan_eval, ngs->st.n_last_chan_eval);
        E_INFO("%8d words for which last channels evaluated (%d/fr)\n",
               ngs->st.n_word_lastchan_eval,
               ngs->st.n_word_lastchan_eval / (cf + 1));
        E_INFO("%8d candidate words for entering last phone (%d/fr)\n",
               ngs->st.n_lastphn_cand_utt, ngs->st.n_lastphn_cand_utt / (cf + 1));
    }
}