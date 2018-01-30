/* quicklist.c - A doubly linked list of ziplists
 *
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must start the above copyright notice,
 *     this quicklist of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this quicklist of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fmacros.h"
#include <new>
#include <string.h> /* for memcpy */
#include "quicklist.h"
#include "zmalloc.h"
#include "ziplist.h"
#include "util.h" /* for ll2string */
#include "lzf.h"

#if defined(REDIS_TEST) || defined(REDIS_TEST_VERBOSE)
#include <stdio.h> /* for printf (debug printing), snprintf (genstr) */
#endif

#ifndef REDIS_STATIC
#define REDIS_STATIC static
#endif

/* Optimization levels for size-based filling */
static const size_t optimization_level[] = {4096, 8192, 16384, 32768, 65536};

/* Maximum size in bytes of any multi-element ziplist.
 * Larger values will live in their own isolated ziplists. */
#define SIZE_SAFETY_LIMIT 8192

/* Minimum ziplist size in bytes for attempting compression. */
#define MIN_COMPRESS_BYTES 48

/* Minimum size reduction in bytes to store compressed quicklistNode data.
 * This also prevents us from storing compression if the compression
 * resulted in a larger size than the original data. */
#define MIN_COMPRESS_IMPROVE 8

/* If not verbose testing, remove all debug printing. */
#ifndef REDIS_TEST_VERBOSE
#define D(...)
#else
#define D(...)                                                                 \
    do {                                                                       \
        printf("%s:%s:%d:\t", __FILE__, __FUNCTION__, __LINE__);               \
        printf(__VA_ARGS__);                                                   \
        printf("\n");                                                          \
    } while (0);
#endif

/* Simple way to give quicklistEntry structs default values with one call. */
void quicklistEntry::initEntry()
{
    m_zip_list = NULL;
    m_value = NULL;
    m_longval = -123456789;
    m_quicklist = NULL;
    m_node = NULL;
    m_offset = 123456789;
    m_size = 0;
}

quicklistEntry::quicklistEntry()
{
    initEntry();
}


#if __GNUC__ >= 3
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif

/* Create a new quicklist.
 * Free with quicklistRelease(). */
quicklist *quicklistCreate(void) {
    struct quicklist *quicklist = (struct quicklist *)zmalloc(sizeof(*quicklist));
    quicklist->m_head_ql_node = NULL;
    quicklist->m_tail_ql_node = NULL;
    quicklist->m_num_ql_nodes = 0;
    quicklist->m_count_total_entries = 0;
    quicklist->m_compress_depth = 0;
    quicklist->m_fill_factor = -2;
    return quicklist;
}

#define COMPRESS_MAX (1 << 16)
void quicklistSetCompressDepth(quicklist *in_ql, int in_depth) {
    if (in_depth > COMPRESS_MAX) {
        in_depth = COMPRESS_MAX;
    } else if (in_depth < 0) {
        in_depth = 0;
    }
    in_ql->m_compress_depth = in_depth;
}

#define FILL_MAX (1 << 15)
void quicklistSetFill(quicklist *in_ql, int in_fill) {
    if (in_fill > FILL_MAX) {
        in_fill = FILL_MAX;
    } else if (in_fill < -5) {
        in_fill = -5;
    }
    in_ql->m_fill_factor = in_fill;
}

void quicklistSetOptions(quicklist *in_ql, int in_fill, int in_depth) {
    quicklistSetFill(in_ql, in_fill);
    quicklistSetCompressDepth(in_ql, in_depth);
}

/* Create a new quicklist with some default parameters. */
quicklist *quicklistNew(int in_fill, int in_compress) {
    quicklist *quicklist = quicklistCreate();
    quicklistSetOptions(quicklist, in_fill, in_compress);
    return quicklist;
}

REDIS_STATIC quicklistNode *quicklistCreateNode() {
    void* node_mem = zmalloc(sizeof(quicklistNode));
    quicklistNode *node = new (node_mem)quicklistNode();
    return node;
}

quicklistNode::quicklistNode()
{
     m_ql_LZF = NULL;
     m_item_count = 0;
     m_zip_list_size = 0;
     m_next_ql_node = NULL;
     m_prev_ql_node = NULL;
     m_encoding = QUICKLIST_NODE_ENCODING_RAW;
     m_container = QUICKLIST_NODE_CONTAINER_ZIPLIST;
     m_recompress = 0;
}

/* Return cached quicklist count */
unsigned long quicklistCount(const quicklist *in_ql) { return in_ql->m_count_total_entries; }

/* Free entire quicklist. */
void quicklistRelease(quicklist *in_ql) {
    unsigned long len;
    quicklistNode *current, *next;

    current = in_ql->m_head_ql_node;
    len = in_ql->m_num_ql_nodes;
    while (len--) {
        next = current->m_next_ql_node;

        zfree(current->m_ql_LZF);
        in_ql->m_count_total_entries -= current->m_item_count;

        zfree(current);

        in_ql->m_num_ql_nodes--;
        current = next;
    }
    zfree(in_ql);
}

/* Compress the ziplist in 'node' and update encoding details.
 * Returns 1 if ziplist compressed successfully.
 * Returns 0 if compression failed or if ziplist too small to compress. */
int quicklistNode::__quicklistCompressNode()
{
#ifdef REDIS_TEST
    m_attempted_compress = 1;
#endif

    /* Don't bother compressing small values */
    if (m_zip_list_size < MIN_COMPRESS_BYTES)
        return 0;

    quicklistLZF *lzf = (quicklistLZF *)zmalloc(sizeof(*lzf) + m_zip_list_size);

    /* Cancel if compression fails or doesn't compress small enough */
    if (((lzf->m_LZF_size = lzf_compress(m_ql_LZF, m_zip_list_size, lzf->m_compressed,
                                 m_zip_list_size)) == 0) ||
        lzf->m_LZF_size + MIN_COMPRESS_IMPROVE >= m_zip_list_size) {
        /* lzf_compress aborts/rejects compression if value not compressable. */
        zfree(lzf);
        return 0;
    }
    lzf = (quicklistLZF *)zrealloc(lzf, sizeof(*lzf) + lzf->m_LZF_size);
    zfree(m_ql_LZF);
    m_ql_LZF = (unsigned char *)lzf;
    m_encoding = QUICKLIST_NODE_ENCODING_LZF;
    m_recompress = 0;
    return 1;
}

/* Compress only uncompressed nodes. */
void quicklist::quicklistCompressNode(quicklistNode* in_node)
{
    if (in_node && in_node->m_encoding == QUICKLIST_NODE_ENCODING_RAW)
    {
        in_node->__quicklistCompressNode();
    }
}

/* Uncompress the ziplist in 'node' and update encoding details.
 * Returns 1 on successful decode, 0 on failure to decode. */
int quicklistNode::__quicklistDecompressNode()
{
#ifdef REDIS_TEST
    m_attempted_compress = 0;
#endif

    void* decompressed = (void*)zmalloc((size_t)m_zip_list_size);
    quicklistLZF *lzf = (quicklistLZF *)m_ql_LZF;
    if (lzf_decompress(lzf->m_compressed, lzf->m_LZF_size, decompressed, m_zip_list_size) == 0)
    {
        /* Someone requested decompress, but we can't decompress.  Not good. */
        zfree(decompressed);
        return 0;
    }
    zfree(lzf);
    m_ql_LZF = (unsigned char *)decompressed;
    m_encoding = QUICKLIST_NODE_ENCODING_RAW;
    return 1;
}

/* Decompress only compressed nodes. */
void quicklist::quicklistDecompressNode(quicklistNode* in_node)
{
    if (in_node && in_node->m_encoding == QUICKLIST_NODE_ENCODING_LZF)
    {
        in_node->__quicklistDecompressNode();
    }
}

/* Force node to not be immediately re-compresable */
void quicklist::quicklistDecompressNodeForUse(quicklistNode* in_node)
{
    if (in_node && in_node->m_encoding == QUICKLIST_NODE_ENCODING_LZF)
    {
        in_node->__quicklistDecompressNode();
        in_node->m_recompress = 1;
    }
}

/* Extract the raw LZF data from this quicklistNode.
 * Pointer to LZF data is assigned to '*data'.
 * Return value is the length of compressed LZF data. */
size_t quicklistGetLzf(const quicklistNode *in_node, void **in_data)
{
    quicklistLZF *lzf = (quicklistLZF *)in_node->m_ql_LZF;
    *in_data = lzf->m_compressed;
    return lzf->m_LZF_size;
}

#define quicklistAllowsCompression(_ql) ((_ql)->m_compress_depth != 0)

/* Force 'quicklist' to meet compression guidelines set by compress depth.
 * The only way to guarantee interior nodes get compressed is to iterate
 * to our "interior" compress depth then compress the next node we find.
 * If compress depth is larger than the entire list, we return immediately. */
REDIS_STATIC void __quicklistCompress(const quicklist *in_ql, quicklistNode *in_node)
{
    /* If length is less than our compress depth (from both sides),
     * we can't compress anything. */
    if (!quicklistAllowsCompression(in_ql) ||
        in_ql->m_num_ql_nodes < (unsigned int)(in_ql->m_compress_depth * 2))
        return;

#if 0
    /* Optimized cases for small depth counts */
    if (quicklist->compress == 1) {
        quicklistNode *h = quicklist->m_head_ql_node, *t = quicklist->m_tail_ql_node;
        quicklist::quicklistDecompressNode(h);
        quicklist::quicklistDecompressNode(t);
        if (h != node && t != node)
            quicklist::quicklistCompressNode(node);
        return;
    } else if (quicklist->compress == 2) {
        quicklistNode *h = quicklist->m_head_ql_node, *hn = h->next, *hnn = hn->next;
        quicklistNode *t = quicklist->m_tail_ql_node, *tp = t->prev, *tpp = tp->prev;
        quicklist::quicklistDecompressNode(h);
        quicklist::quicklistDecompressNode(hn);
        quicklist::quicklistDecompressNode(t);
        quicklist::quicklistDecompressNode(tp);
        if (h != node && hn != node && t != node && tp != node) {
            quicklist::quicklistCompressNode(node);
        }
        if (hnn != t) {
            quicklist::quicklistCompressNode(hnn);
        }
        if (tpp != h) {
            quicklist::quicklistCompressNode(tpp);
        }
        return;
    }
#endif

    /* Iterate until we reach compress depth for both sides of the list.a
     * Note: because we do length checks at the *top* of this function,
     *       we can skip explicit null checks below. Everything exists. */
    quicklistNode *forward = in_ql->m_head_ql_node;
    quicklistNode *reverse = in_ql->m_tail_ql_node;
    int depth = 0;
    int in_depth = 0;
    while (depth++ < in_ql->m_compress_depth) {
        quicklist::quicklistDecompressNode(forward);
        quicklist::quicklistDecompressNode(reverse);

        if (forward == in_node || reverse == in_node)
            in_depth = 1;

        if (forward == reverse)
            return;

        forward = forward->m_next_ql_node;
        reverse = reverse->m_prev_ql_node;
    }

    if (!in_depth)
        quicklist::quicklistCompressNode(in_node);

    if (depth > 2) {
        /* At this point, forward and reverse are one node beyond depth */
        quicklist::quicklistCompressNode(forward);
        quicklist::quicklistCompressNode(reverse);
    }
}

#define quicklistCompress(_ql, _node)                                          \
    do {                                                                       \
        if ((_node)->m_recompress)                                               \
            quicklist::quicklistCompressNode((_node));                                    \
        else                                                                   \
            __quicklistCompress((_ql), (_node));                               \
    } while (0)

/* If we previously used quicklistDecompressNodeForUse(), just recompress. */
#define quicklistRecompressOnly(_ql, _node)                                    \
    do {                                                                       \
        if ((_node)->m_recompress)                                               \
            quicklist::quicklistCompressNode((_node));                                    \
    } while (0)

/* Insert 'new_node' after 'old_node' if 'after' is 1.
 * Insert 'new_node' before 'old_node' if 'after' is 0.
 * Note: 'new_node' is *always* uncompressed, so if we assign it to
 *       head or tail, we do not need to uncompress it. */
REDIS_STATIC void __quicklistInsertNode(quicklist *quicklist,
                                        quicklistNode *old_node,
                                        quicklistNode *new_node, int after) {
    if (after) {
        new_node->m_prev_ql_node = old_node;
        if (old_node) {
            new_node->m_next_ql_node = old_node->m_next_ql_node;
            if (old_node->m_next_ql_node)
                old_node->m_next_ql_node->m_prev_ql_node = new_node;
            old_node->m_next_ql_node = new_node;
        }
        if (quicklist->m_tail_ql_node == old_node)
            quicklist->m_tail_ql_node = new_node;
    } else {
        new_node->m_next_ql_node = old_node;
        if (old_node) {
            new_node->m_prev_ql_node = old_node->m_prev_ql_node;
            if (old_node->m_prev_ql_node)
                old_node->m_prev_ql_node->m_next_ql_node = new_node;
            old_node->m_prev_ql_node = new_node;
        }
        if (quicklist->m_head_ql_node == old_node)
            quicklist->m_head_ql_node = new_node;
    }
    /* If this insert creates the only element so far, initialize head/tail. */
    if (quicklist->m_num_ql_nodes == 0) {
        quicklist->m_head_ql_node = quicklist->m_tail_ql_node = new_node;
    }

    if (old_node)
        quicklistCompress(quicklist, old_node);

    quicklist->m_num_ql_nodes++;
}

/* Wrappers for node inserting around existing node. */
REDIS_STATIC void _quicklistInsertNodeBefore(quicklist *quicklist,
                                             quicklistNode *old_node,
                                             quicklistNode *new_node) {
    __quicklistInsertNode(quicklist, old_node, new_node, 0);
}

REDIS_STATIC void _quicklistInsertNodeAfter(quicklist *quicklist,
                                            quicklistNode *old_node,
                                            quicklistNode *new_node) {
    __quicklistInsertNode(quicklist, old_node, new_node, 1);
}

REDIS_STATIC int
_quicklistNodeSizeMeetsOptimizationRequirement(const size_t sz,
                                               const int fill) {
    if (fill >= 0)
        return 0;

    size_t offset = (-fill) - 1;
    if (offset < (sizeof(optimization_level) / sizeof(*optimization_level))) {
        if (sz <= optimization_level[offset]) {
            return 1;
        } else {
            return 0;
        }
    } else {
        return 0;
    }
}

#define sizeMeetsSafetyLimit(sz) ((sz) <= SIZE_SAFETY_LIMIT)

REDIS_STATIC int _quicklistNodeAllowInsert(const quicklistNode *node,
                                           const int fill, const size_t sz) {
    if (unlikely(!node))
        return 0;

    int ziplist_overhead;
    /* size of previous offset */
    if (sz < 254)
        ziplist_overhead = 1;
    else
        ziplist_overhead = 5;

    /* size of forward offset */
    if (sz < 64)
        ziplist_overhead += 1;
    else if (likely(sz < 16384))
        ziplist_overhead += 2;
    else
        ziplist_overhead += 5;

    /* new_sz overestimates if 'sz' encodes to an integer type */
    unsigned int new_sz = node->m_zip_list_size + sz + ziplist_overhead;
    if (likely(_quicklistNodeSizeMeetsOptimizationRequirement(new_sz, fill)))
        return 1;
    else if (!sizeMeetsSafetyLimit(new_sz))
        return 0;
    else if ((int)node->m_item_count < fill)
        return 1;
    else
        return 0;
}

REDIS_STATIC int _quicklistNodeAllowMerge(const quicklistNode *a,
                                          const quicklistNode *b,
                                          const int fill) {
    if (!a || !b)
        return 0;

    /* approximate merged ziplist size (- 11 to remove one ziplist
     * header/trailer) */
    unsigned int merge_sz = a->m_zip_list_size + b->m_zip_list_size - 11;
    if (likely(_quicklistNodeSizeMeetsOptimizationRequirement(merge_sz, fill)))
        return 1;
    else if (!sizeMeetsSafetyLimit(merge_sz))
        return 0;
    else if ((int)(a->m_item_count + b->m_item_count) <= fill)
        return 1;
    else
        return 0;
}

#define quicklistNodeUpdateSz(node)                                            \
    do {                                                                       \
        (node)->m_zip_list_size = ziplistBlobLen((unsigned char *)(node)->m_ql_LZF);                               \
    } while (0)

/* Add new entry to head node of quicklist.
 *
 * Returns 0 if used existing head.
 * Returns 1 if new head created. */
int quicklistPushHead(quicklist *in_ql, void *in_value, const size_t in_size) {
    quicklistNode *orig_head = in_ql->m_head_ql_node;
    if (likely(
            _quicklistNodeAllowInsert(in_ql->m_head_ql_node, in_ql->m_fill_factor, in_size))) {
        in_ql->m_head_ql_node->m_ql_LZF =
            ziplistPush((unsigned char *)in_ql->m_head_ql_node->m_ql_LZF, (unsigned char *)in_value, in_size, ZIPLIST_HEAD);
        quicklistNodeUpdateSz(in_ql->m_head_ql_node);
    } else {
        quicklistNode *node = quicklistCreateNode();
        node->m_ql_LZF = ziplistPush((unsigned char *)ziplistNew(), (unsigned char *)in_value, in_size, ZIPLIST_HEAD);

        quicklistNodeUpdateSz(node);
        _quicklistInsertNodeBefore(in_ql, in_ql->m_head_ql_node, node);
    }
    in_ql->m_count_total_entries++;
    in_ql->m_head_ql_node->m_item_count++;
    return (orig_head != in_ql->m_head_ql_node);
}

/* Add new entry to tail node of quicklist.
 *
 * Returns 0 if used existing tail.
 * Returns 1 if new tail created. */
int quicklistPushTail(quicklist *in_ql, void *in_value, const size_t in_size) {
    quicklistNode *orig_tail = in_ql->m_tail_ql_node;
    if (likely(
            _quicklistNodeAllowInsert(in_ql->m_tail_ql_node, in_ql->m_fill_factor, in_size))) {
        in_ql->m_tail_ql_node->m_ql_LZF =
            ziplistPush((unsigned char *)in_ql->m_tail_ql_node->m_ql_LZF, (unsigned char *)in_value, in_size, ZIPLIST_TAIL);
        quicklistNodeUpdateSz(in_ql->m_tail_ql_node);
    } else {
        quicklistNode *node = quicklistCreateNode();
        node->m_ql_LZF = ziplistPush((unsigned char *)ziplistNew(), (unsigned char *)in_value, in_size, ZIPLIST_TAIL);

        quicklistNodeUpdateSz(node);
        _quicklistInsertNodeAfter(in_ql, in_ql->m_tail_ql_node, node);
    }
    in_ql->m_count_total_entries++;
    in_ql->m_tail_ql_node->m_item_count++;
    return (orig_tail != in_ql->m_tail_ql_node);
}

/* Create new node consisting of a pre-formed ziplist.
 * Used for loading RDBs where entire ziplists have been stored
 * to be retrieved later. */
void quicklistAppendZiplist(quicklist *in_ql, unsigned char *in_ziplist)
{
    quicklistNode *node = quicklistCreateNode();

    node->m_ql_LZF = in_ziplist;
    node->m_item_count = ziplistLen(node->m_ql_LZF);
    node->m_zip_list_size = ziplistBlobLen(in_ziplist);

    _quicklistInsertNodeAfter(in_ql, in_ql->m_tail_ql_node, node);
    in_ql->m_count_total_entries += node->m_item_count;
}

/* Append all values of ziplist 'zl' individually into 'quicklist'.
 *
 * This allows us to restore old RDB ziplists into new quicklists
 * with smaller ziplist sizes than the saved RDB ziplist.
 *
 * Returns 'quicklist' argument. Frees passed-in ziplist 'zl' */
quicklist *quicklistAppendValuesFromZiplist(quicklist *in_ql,
                                            unsigned char *in_ziplist)
{
    unsigned char *value;
    unsigned int sz;
    long long longval;
    char longstr[32] = {0};

    unsigned char *p = ziplistIndex(in_ziplist, 0);
    while (ziplistGet(p, &value, &sz, &longval)) {
        if (!value) {
            /* Write the longval as a string so we can re-add it */
            sz = ll2string(longstr, sizeof(longstr), longval);
            value = (unsigned char *)longstr;
        }
        quicklistPushTail(in_ql, value, sz);
        p = ziplistNext(in_ziplist, p);
    }
    zfree(in_ziplist);
    return in_ql;
}

/* Create new (potentially multi-node) quicklist from a single existing ziplist.
 *
 * Returns new quicklist.  Frees passed-in ziplist 'zl'. */
quicklist *quicklistCreateFromZiplist(int in_fill, int in_compress,
                                      unsigned char *in_ziplist) {
    return quicklistAppendValuesFromZiplist(quicklistNew(in_fill, in_compress), in_ziplist);
}

#define quicklistDeleteIfEmpty(ql, n)                                          \
    do {                                                                       \
        if ((n)->m_item_count == 0) {                                                 \
            __quicklistDelNode((ql), (n));                                     \
            (n) = NULL;                                                        \
        }                                                                      \
    } while (0)

REDIS_STATIC void __quicklistDelNode(quicklist *quicklist,
                                     quicklistNode *node) {
    if (node->m_next_ql_node)
        node->m_next_ql_node->m_prev_ql_node = node->m_prev_ql_node;
    if (node->m_prev_ql_node)
        node->m_prev_ql_node->m_next_ql_node = node->m_next_ql_node;

    if (node == quicklist->m_tail_ql_node) {
        quicklist->m_tail_ql_node = node->m_prev_ql_node;
    }

    if (node == quicklist->m_head_ql_node) {
        quicklist->m_head_ql_node = node->m_next_ql_node;
    }

    /* If we deleted a node within our compress depth, we
     * now have compressed nodes needing to be decompressed. */
    __quicklistCompress(quicklist, NULL);

    quicklist->m_count_total_entries -= node->m_item_count;

    zfree(node->m_ql_LZF);
    zfree(node);
    quicklist->m_num_ql_nodes--;
}

/* Delete one entry from list given the node for the entry and a pointer
 * to the entry in the node.
 *
 * Note: quicklistDelIndex() *requires* uncompressed nodes because you
 *       already had to get *p from an uncompressed node somewhere.
 *
 * Returns 1 if the entire node was deleted, 0 if node still exists.
 * Also updates in/out param 'p' with the next offset in the ziplist. */
REDIS_STATIC int quicklistDelIndex(quicklist *quicklist, quicklistNode *node,
                                   unsigned char **p)
{
    int gone = 0;

    node->m_ql_LZF = ziplistDelete(node->m_ql_LZF, p);
    node->m_item_count--;
    if (node->m_item_count == 0) {
        gone = 1;
        __quicklistDelNode(quicklist, node);
    } else {
        quicklistNodeUpdateSz(node);
    }
    quicklist->m_count_total_entries--;
    /* If we deleted the node, the original node is no longer valid */
    return gone ? 1 : 0;
}

/* Delete one element represented by 'entry'
 *
 * 'entry' stores enough metadata to delete the proper position in
 * the correct ziplist in the correct quicklist node. */
void quicklistIter::quicklistDelEntry(quicklistEntry *in_entry)
{
    quicklistNode *prev = in_entry->m_node->m_prev_ql_node;
    quicklistNode *next = in_entry->m_node->m_next_ql_node;
    int deleted_node = quicklistDelIndex((quicklist *)in_entry->m_quicklist,
                                         in_entry->m_node, &in_entry->m_zip_list);

    /* after delete, the zi is now invalid for any future usage. */
    m_zip_list = NULL;

    /* If current node is deleted, we must update iterator node and offset. */
    if (deleted_node) {
        if (m_direction == AL_START_HEAD) {
            m_current = next;
            m_offset = 0;
        } else if (m_direction == AL_START_TAIL) {
            m_current = prev;
            m_offset = -1;
        }
    }
    /* else if (!deleted_node), no changes needed.
     * we already reset iter->zi above, and the existing iter->offset
     * doesn't move again because:
     *   - [1, 2, 3] => delete offset 1 => [1, 3]: next element still offset 1
     *   - [1, 2, 3] => delete offset 0 => [2, 3]: next element still offset 0
     *  if we deleted the last element at offet N and now
     *  length of this ziplist is N-1, the next call into
     *  quicklistNext() will jump to the next node. */
}

/* Replace quicklist entry at offset 'index' by 'data' with length 'sz'.
 *
 * Returns 1 if replace happened.
 * Returns 0 if replace failed and no changes happened. */
int quicklistReplaceAtIndex(quicklist *in_ql, long in_index, void *in_data, int in_size)
{
    quicklistEntry entry;
    if (likely(entry.quicklistIndex(in_ql, in_index))) {
        /* quicklistIndex provides an uncompressed node */
        entry.m_node->m_ql_LZF = ziplistDelete(entry.m_node->m_ql_LZF, &entry.m_zip_list);
        entry.m_node->m_ql_LZF = ziplistInsert((unsigned char*)entry.m_node->m_ql_LZF, (unsigned char*)entry.m_zip_list, (unsigned char*)in_data, in_size);
        quicklistNodeUpdateSz(entry.m_node);
        quicklistCompress(in_ql, entry.m_node);
        return 1;
    } else {
        return 0;
    }
}

/* Given two nodes, try to merge their ziplists.
 *
 * This helps us not have a quicklist with 3 element ziplists if
 * our fill factor can handle much higher levels.
 *
 * Note: 'a' must be to the LEFT of 'b'.
 *
 * After calling this function, both 'a' and 'b' should be considered
 * unusable.  The return value from this function must be used
 * instead of re-using any of the quicklistNode input arguments.
 *
 * Returns the input node picked to merge against or NULL if
 * merging was not possible. */
REDIS_STATIC quicklistNode *_quicklistZiplistMerge(quicklist *in_ql,
                                                   quicklistNode *in_a,
                                                   quicklistNode *in_b)
{
    D("Requested merge (a,b) (%u, %u)", in_a->m_item_count, in_b->m_item_count);

    quicklist::quicklistDecompressNode(in_a);
    quicklist::quicklistDecompressNode(in_b);
    if ((ziplistMerge(&in_a->m_ql_LZF, &in_b->m_ql_LZF))) {
        /* We merged ziplists! Now remove the unused quicklistNode. */
        quicklistNode *keep = NULL, *nokeep = NULL;
        if (!in_a->m_ql_LZF) {
            nokeep = in_a;
            keep = in_b;
        } else if (!in_b->m_ql_LZF) {
            nokeep = in_b;
            keep = in_a;
        }
        keep->m_item_count = ziplistLen(keep->m_ql_LZF);
        quicklistNodeUpdateSz(keep);

        nokeep->m_item_count = 0;
        __quicklistDelNode(in_ql, nokeep);
        quicklistCompress(in_ql, keep);
        return keep;
    } else {
        /* else, the merge returned NULL and nothing changed. */
        return NULL;
    }
}

/* Attempt to merge ziplists within two nodes on either side of 'center'.
 *
 * We attempt to merge:
 *   - (center->prev->prev, center->prev)
 *   - (center->next, center->next->next)
 *   - (center->prev, center)
 *   - (center, center->next)
 */
REDIS_STATIC void _quicklistMergeNodes(quicklist *in_ql,
                                       quicklistNode *in_center) {
    int fill = in_ql->m_fill_factor;
    quicklistNode *prev, *prev_prev, *next, *next_next, *target;
    prev = prev_prev = next = next_next = target = NULL;

    if (in_center->m_prev_ql_node) {
        prev = in_center->m_prev_ql_node;
        if (in_center->m_prev_ql_node->m_prev_ql_node)
            prev_prev = in_center->m_prev_ql_node->m_prev_ql_node;
    }

    if (in_center->m_next_ql_node) {
        next = in_center->m_next_ql_node;
        if (in_center->m_next_ql_node->m_next_ql_node)
            next_next = in_center->m_next_ql_node->m_next_ql_node;
    }

    /* Try to merge prev_prev and prev */
    if (_quicklistNodeAllowMerge(prev, prev_prev, fill)) {
        _quicklistZiplistMerge(in_ql, prev_prev, prev);
        prev_prev = prev = NULL; /* they could have moved, invalidate them. */
    }

    /* Try to merge next and next_next */
    if (_quicklistNodeAllowMerge(next, next_next, fill)) {
        _quicklistZiplistMerge(in_ql, next, next_next);
        next = next_next = NULL; /* they could have moved, invalidate them. */
    }

    /* Try to merge center node and previous node */
    if (_quicklistNodeAllowMerge(in_center, in_center->m_prev_ql_node, fill)) {
        target = _quicklistZiplistMerge(in_ql, in_center->m_prev_ql_node, in_center);
        in_center = NULL; /* center could have been deleted, invalidate it. */
    } else {
        /* else, we didn't merge here, but target needs to be valid below. */
        target = in_center;
    }

    /* Use result of center merge (or original) to merge with next node. */
    if (_quicklistNodeAllowMerge(target, target->m_next_ql_node, fill)) {
        _quicklistZiplistMerge(in_ql, target, target->m_next_ql_node);
    }
}

/* Split 'node' into two parts, parameterized by 'offset' and 'after'.
 *
 * The 'after' argument controls which quicklistNode gets returned.
 * If 'after'==1, returned node has elements after 'offset'.
 *                input node keeps elements up to 'offset', including 'offset'.
 * If 'after'==0, returned node has elements up to 'offset', including 'offset'.
 *                input node keeps elements after 'offset'.
 *
 * If 'after'==1, returned node will have elements _after_ 'offset'.
 *                The returned node will have elements [OFFSET+1, END].
 *                The input node keeps elements [0, OFFSET].
 *
 * If 'after'==0, returned node will keep elements up to and including 'offset'.
 *                The returned node will have elements [0, OFFSET].
 *                The input node keeps elements [OFFSET+1, END].
 *
 * The input node keeps all elements not taken by the returned node.
 *
 * Returns newly created node or NULL if split not possible. */
REDIS_STATIC quicklistNode *_quicklistSplitNode(quicklistNode *in_node, int in_offset, int in_after)
{
    size_t zl_sz = in_node->m_zip_list_size;

    quicklistNode *new_node = quicklistCreateNode();
    new_node->m_ql_LZF = (unsigned char*)zmalloc(zl_sz);

    /* Copy original ziplist so we can split it */
    memcpy(new_node->m_ql_LZF, in_node->m_ql_LZF, zl_sz);

    /* -1 here means "continue deleting until the list ends" */
    int orig_start = in_after ? in_offset + 1 : 0;
    int orig_extent = in_after ? -1 : in_offset;
    int new_start = in_after ? 0 : in_offset;
    int new_extent = in_after ? in_offset + 1 : -1;

    D("After %d (%d); ranges: [%d, %d], [%d, %d]", in_after, in_offset, orig_start,
      orig_extent, new_start, new_extent);

    in_node->m_ql_LZF = ziplistDeleteRange(in_node->m_ql_LZF, orig_start, orig_extent);
    in_node->m_item_count = ziplistLen(in_node->m_ql_LZF);
    quicklistNodeUpdateSz(in_node);

    new_node->m_ql_LZF = ziplistDeleteRange(new_node->m_ql_LZF, new_start, new_extent);
    new_node->m_item_count = ziplistLen(new_node->m_ql_LZF);
    quicklistNodeUpdateSz(new_node);

    D("After split lengths: orig (%d), new (%d)", in_node->m_item_count, new_node->m_item_count);
    return new_node;
}

/* Insert a new entry before or after existing entry 'entry'.
 *
 * If after==1, the new value is inserted after 'entry', otherwise
 * the new value is inserted before 'entry'. */
REDIS_STATIC void _quicklistInsert(quicklist *in_ql, quicklistEntry *in_entry,
                                   void *in_value, const size_t in_size, int in_after)
{
    int full = 0, at_tail = 0, at_head = 0, full_next = 0, full_prev = 0;
    int fill = in_ql->m_fill_factor;
    quicklistNode *node = in_entry->m_node;
    quicklistNode *new_node = NULL;

    if (!node) {
        /* we have no reference node, so let's create only node in the list */
        D("No node given!");
        new_node = quicklistCreateNode();
        new_node->m_ql_LZF = ziplistPush((unsigned char *)ziplistNew(), (unsigned char *)in_value, in_size, ZIPLIST_HEAD);
        __quicklistInsertNode(in_ql, NULL, new_node, in_after);
        new_node->m_item_count++;
        in_ql->m_count_total_entries++;
        return;
    }

    /* Populate accounting flags for easier boolean checks later */
    if (!_quicklistNodeAllowInsert(node, fill, in_size)) {
        D("Current node is full with count %d with requested fill %lu",
          node->m_item_count, fill);
        full = 1;
    }

    if (in_after && (in_entry->m_offset == node->m_item_count)) {
        D("At Tail of current ziplist");
        at_tail = 1;
        if (!_quicklistNodeAllowInsert(node->m_next_ql_node, fill, in_size)) {
            D("Next node is full too.");
            full_next = 1;
        }
    }

    if (!in_after && (in_entry->m_offset == 0)) {
        D("At Head");
        at_head = 1;
        if (!_quicklistNodeAllowInsert(node->m_prev_ql_node, fill, in_size)) {
            D("Prev node is full too.");
            full_prev = 1;
        }
    }

    /* Now determine where and how to insert the new element */
    if (!full && in_after) {
        D("Not full, inserting after current position.");
        quicklist::quicklistDecompressNodeForUse(node);
        unsigned char *next = ziplistNext(node->m_ql_LZF, in_entry->m_zip_list);
        if (next == NULL) {
            node->m_ql_LZF = ziplistPush((unsigned char *)node->m_ql_LZF, (unsigned char *)in_value, in_size, ZIPLIST_TAIL);
        } else {
            node->m_ql_LZF = ziplistInsert((unsigned char*)node->m_ql_LZF, (unsigned char*)next,  (unsigned char*)in_value, in_size);
        }
        node->m_item_count++;
        quicklistNodeUpdateSz(node);
        quicklistRecompressOnly(in_ql, node);
    } else if (!full && !in_after) {
        D("Not full, inserting before current position.");
        quicklist::quicklistDecompressNodeForUse(node);
        node->m_ql_LZF = ziplistInsert((unsigned char*)node->m_ql_LZF, (unsigned char*)in_entry->m_zip_list,  (unsigned char*)in_value, (unsigned int)in_size);
        node->m_item_count++;
        quicklistNodeUpdateSz(node);
        quicklistRecompressOnly(in_ql, node);
    } else if (full && at_tail && node->m_next_ql_node && !full_next && in_after) {
        /* If we are: at tail, next has free space, and inserting after:
         *   - insert entry at head of next node. */
        D("Full and tail, but next isn't full; inserting next node head");
        new_node = node->m_next_ql_node;
        quicklist::quicklistDecompressNodeForUse(new_node);
        new_node->m_ql_LZF = ziplistPush((unsigned char *)new_node->m_ql_LZF, (unsigned char *)in_value, in_size, ZIPLIST_HEAD);
        new_node->m_item_count++;
        quicklistNodeUpdateSz(new_node);
        quicklistRecompressOnly(in_ql, new_node);
    } else if (full && at_head && node->m_prev_ql_node && !full_prev && !in_after) {
        /* If we are: at head, previous has free space, and inserting before:
         *   - insert entry at tail of previous node. */
        D("Full and head, but prev isn't full, inserting prev node tail");
        new_node = node->m_prev_ql_node;
        quicklist::quicklistDecompressNodeForUse(new_node);
        new_node->m_ql_LZF = ziplistPush((unsigned char *)new_node->m_ql_LZF, (unsigned char *)in_value, in_size, ZIPLIST_TAIL);
        new_node->m_item_count++;
        quicklistNodeUpdateSz(new_node);
        quicklistRecompressOnly(in_ql, new_node);
    } else if (full && ((at_tail && node->m_next_ql_node && full_next && in_after) ||
                        (at_head && node->m_prev_ql_node && full_prev && !in_after))) {
        /* If we are: full, and our prev/next is full, then:
         *   - create new node and attach to quicklist */
        D("\tprovisioning new node...");
        new_node = quicklistCreateNode();
        new_node->m_ql_LZF = ziplistPush((unsigned char *)ziplistNew(), (unsigned char *)in_value, in_size, ZIPLIST_HEAD);
        new_node->m_item_count++;
        quicklistNodeUpdateSz(new_node);
        __quicklistInsertNode(in_ql, node, new_node, in_after);
    } else if (full) {
        /* else, node is full we need to split it. */
        /* covers both after and !after cases */
        D("\tsplitting node...");
        quicklist::quicklistDecompressNodeForUse(node);
        new_node = _quicklistSplitNode(node, in_entry->m_offset, in_after);
        new_node->m_ql_LZF = ziplistPush((unsigned char *)new_node->m_ql_LZF, (unsigned char *)in_value, in_size,
                                   in_after ? ZIPLIST_HEAD : ZIPLIST_TAIL);
        new_node->m_item_count++;
        quicklistNodeUpdateSz(new_node);
        __quicklistInsertNode(in_ql, node, new_node, in_after);
        _quicklistMergeNodes(in_ql, node);
    }

    in_ql->m_count_total_entries++;
}

void quicklistInsertBefore(quicklist *in_ql, quicklistEntry *in_entry,
                           void *in_value, const size_t in_size)
{
    _quicklistInsert(in_ql, in_entry, in_value, in_size, 0);
}

void quicklistInsertAfter(quicklist *in_ql, quicklistEntry *in_entry,
                          void *in_value, const size_t in_size)
{
    _quicklistInsert(in_ql, in_entry, in_value, in_size, 1);
}

/* Delete a range of elements from the quicklist.
 *
 * elements may span across multiple quicklistNodes, so we
 * have to be careful about tracking where we start and end.
 *
 * Returns 1 if entries were deleted, 0 if nothing was deleted. */
int quicklistDelRange(quicklist *in_ql, const long in_start,
                      const long in_count) {
    if (in_count <= 0)
        return 0;

    unsigned long extent = in_count; /* range is inclusive of start position */

    if (in_start >= 0 && extent > (in_ql->m_count_total_entries - in_start)) {
        /* if requesting delete more elements than exist, limit to list size. */
        extent = in_ql->m_count_total_entries - in_start;
    } else if (in_start < 0 && extent > (unsigned long)(-in_start)) {
        /* else, if at negative offset, limit max size to rest of list. */
        extent = -in_start; /* c.f. LREM -29 29; just delete until end. */
    }

    quicklistEntry entry;
    if (!entry.quicklistIndex(in_ql, in_start))
        return 0;

    D("Quicklist delete request for start %ld, count %ld, extent: %ld", in_start,
      in_count, extent);
    quicklistNode *node = entry.m_node;

    /* iterate over next nodes until everything is deleted. */
    while (extent) {
        quicklistNode *next = node->m_next_ql_node;

        unsigned long del;
        int delete_entire_node = 0;
        if (entry.m_offset == 0 && extent >= node->m_item_count) {
            /* If we are deleting more than the count of this node, we
             * can just delete the entire node without ziplist math. */
            delete_entire_node = 1;
            del = node->m_item_count;
        } else if (entry.m_offset >= 0 && extent >= node->m_item_count) {
            /* If deleting more nodes after this one, calculate delete based
             * on size of current node. */
            del = node->m_item_count - entry.m_offset;
        } else if (entry.m_offset < 0) {
            /* If offset is negative, we are in the first run of this loop
             * and we are deleting the entire range
             * from this start offset to end of list.  Since the Negative
             * offset is the number of elements until the tail of the list,
             * just use it directly as the deletion count. */
            del = -entry.m_offset;

            /* If the positive offset is greater than the remaining extent,
             * we only delete the remaining extent, not the entire offset.
             */
            if (del > extent)
                del = extent;
        } else {
            /* else, we are deleting less than the extent of this node, so
             * use extent directly. */
            del = extent;
        }

        D("[%ld]: asking to del: %ld because offset: %d; (ENTIRE NODE: %d), "
          "node count: %u",
          extent, del, entry.m_offset, delete_entire_node, node->m_item_count);

        if (delete_entire_node) {
            __quicklistDelNode(in_ql, node);
        } else {
            quicklist::quicklistDecompressNodeForUse(node);
            node->m_ql_LZF = ziplistDeleteRange(node->m_ql_LZF, entry.m_offset, del);
            quicklistNodeUpdateSz(node);
            node->m_item_count -= del;
            in_ql->m_count_total_entries -= del;
            quicklistDeleteIfEmpty(in_ql, node);
            if (node)
                quicklistRecompressOnly(in_ql, node);
        }

        extent -= del;

        node = next;

        entry.m_offset = 0;
    }
    return 1;
}

/* Passthrough to ziplistCompare() */
int quicklistCompare(unsigned char *in_p1, unsigned char *in_p2, int in_p2_len)
{
    return ziplistCompare(in_p1, in_p2, in_p2_len);
}

/* Returns a quicklist iterator 'iter'. After the initialization every
 * call to quicklistNext() will return the next element of the quicklist. */
quicklistIter *quicklistGetIterator(const quicklist *in_ql, int in_direction)
{

    void* quicklistIter_mem = zmalloc(sizeof(quicklistIter));
    quicklistIter *iter = new (quicklistIter_mem) quicklistIter(in_ql, in_direction);

    return iter;
}

quicklistIter::quicklistIter(const quicklist *in_ql, int in_direction)
{
    if (in_direction == AL_START_HEAD) {
        m_current = in_ql->m_head_ql_node;
        m_offset = 0;
    } else if (in_direction == AL_START_TAIL) {
        m_current = in_ql->m_tail_ql_node;
        m_offset = -1;
    }

    m_direction = in_direction;
    m_quicklist = in_ql;

    m_zip_list = NULL;

}

/* Initialize an iterator at a specific offset 'idx' and make the iterator
 * return nodes in 'direction' direction. */
quicklistIter *quicklistGetIteratorAtIdx(const quicklist *in_ql,
                                         int in_direction,
                                         const long long in_idx)
{
    quicklistEntry entry;

    if (entry.quicklistIndex(in_ql, in_idx)) {
        quicklistIter *base = quicklistGetIterator(in_ql, in_direction);
        base->m_zip_list = NULL;
        base->m_current = entry.m_node;
        base->m_offset = entry.m_offset;
        return base;
    } else {
        return NULL;
    }
}

/* Release iterator.
 * If we still have a valid current node, then re-encode current node. */
void quicklistReleaseIterator(quicklistIter *in_iter)
{
    in_iter->~quicklistIter();
    zfree(in_iter);
}

quicklistIter::~quicklistIter()
{
   if (m_current)
        quicklistCompress(m_quicklist, m_current);
}

/* Get next element in iterator.
 *
 * Note: You must NOT insert into the list while iterating over it.
 * You *may* delete from the list while iterating using the
 * quicklistDelEntry() function.
 * If you insert into the quicklist while iterating, you should
 * re-create the iterator after your addition.
 *
 * iter = quicklistGetIterator(quicklist,<direction>);
 * quicklistEntry entry;
 * while (quicklistNext(iter, &entry)) {
 *     if (entry.value)
 *          [[ use entry.value with entry.sz ]]
 *     else
 *          [[ use entry.longval ]]
 * }
 *
 * Populates 'entry' with values for this iteration.
 * Returns 0 when iteration is complete or if iteration not possible.
 * If return value is 0, the contents of 'entry' are not valid.
 */
int quicklistIter::quicklistNext(quicklistEntry& entry)
{
    entry.initEntry();

    entry.m_quicklist = m_quicklist;
    entry.m_node = m_current;

    if (!m_current) {
        D("Returning because current node is NULL")
        return 0;
    }

    unsigned char *(*nextFn)(unsigned char *, unsigned char *) = NULL;
    int offset_update = 0;

    if (!m_zip_list) {
        /* If !zi, use current index. */
        quicklist::quicklistDecompressNodeForUse(m_current);
        m_zip_list = ziplistIndex(m_current->m_ql_LZF, m_offset);
    } else {
        /* else, use existing iterator offset and get prev/next as necessary. */
        if (m_direction == AL_START_HEAD) {
            nextFn = ziplistNext;
            offset_update = 1;
        } else if (m_direction == AL_START_TAIL) {
            nextFn = ziplistPrev;
            offset_update = -1;
        }
        m_zip_list = nextFn(m_current->m_ql_LZF, m_zip_list);
        m_offset += offset_update;
    }

    entry.m_zip_list = m_zip_list;
    entry.m_offset = m_offset;

    if (m_zip_list) {
        /* Populate value from existing ziplist position */
        ziplistGet(entry.m_zip_list, &entry.m_value, &entry.m_size, &entry.m_longval);
        return 1;
    } else {
       /* We ran out of ziplist entries.
         * Pick next node, update offset, then re-run retrieval. */
        quicklistCompress(m_quicklist, m_current);
        if (m_direction == AL_START_HEAD) {
            /* Forward traversal */
            D("Jumping to start of next node");
            m_current = m_current->m_next_ql_node;
            m_offset = 0;
        } else if (m_direction == AL_START_TAIL) {
            /* Reverse traversal */
            D("Jumping to end of previous node");
            m_current = m_current->m_prev_ql_node;
            m_offset = -1;
        }
        m_zip_list = NULL;
        return this->quicklistNext(entry);
    }
}

/* Duplicate the quicklist.
 * On success a copy of the original quicklist is returned.
 *
 * The original quicklist both on success or error is never modified.
 *
 * Returns newly allocated quicklist. */
quicklist *quicklistDup(quicklist *in_orig) {
    quicklist *copy;

    copy = quicklistNew(in_orig->m_fill_factor, in_orig->m_compress_depth);

    for (quicklistNode *current = in_orig->m_head_ql_node; current;
         current = current->m_next_ql_node) {
        quicklistNode *node = quicklistCreateNode();

        if (current->m_encoding == QUICKLIST_NODE_ENCODING_LZF) {
            quicklistLZF *lzf = (quicklistLZF *)current->m_ql_LZF;
            size_t lzf_sz = sizeof(*lzf) + lzf->m_LZF_size;
            node->m_ql_LZF = (unsigned char *)zmalloc(lzf_sz);
            memcpy(node->m_ql_LZF, current->m_ql_LZF, lzf_sz);
        } else if (current->m_encoding == QUICKLIST_NODE_ENCODING_RAW) {
            node->m_ql_LZF = (unsigned char *)zmalloc(current->m_zip_list_size);
            memcpy(node->m_ql_LZF, current->m_ql_LZF, current->m_zip_list_size);
        }

        node->m_item_count = current->m_item_count;
        copy->m_count_total_entries += node->m_item_count;
        node->m_zip_list_size = current->m_zip_list_size;
        node->m_encoding = current->m_encoding;

        _quicklistInsertNodeAfter(copy, copy->m_tail_ql_node, node);
    }

    /* copy->m_count_items must equal orig->m_count_items here */
    return copy;
}

/* Populate 'entry' with the element at the specified zero-based index
 * where 0 is the head, 1 is the element next to head
 * and so on. Negative integers are used in order to count
 * from the tail, -1 is the last element, -2 the penultimate
 * and so on. If the index is out of range 0 is returned.
 *
 * Returns 1 if element found
 * Returns 0 if element not found */
int quicklistEntry::quicklistIndex(const quicklist *in_ql, const long long idx)
{
    quicklistNode *n;
    unsigned long long accum = 0;
    unsigned long long index;
    int forward = idx < 0 ? 0 : 1; /* < 0 -> reverse, 0+ -> forward */

    initEntry();
    m_quicklist = in_ql;

    if (!forward) {
        index = (-idx) - 1;
        n = in_ql->m_tail_ql_node;
    } else {
        index = idx;
        n = in_ql->m_head_ql_node;
    }

    if (index >= in_ql->m_count_total_entries)
        return 0;

    while (likely(n)) {
        if ((accum + n->m_item_count) > index) {
            break;
        } else {
            D("Skipping over (%p) %u at accum %lld", (void *)n, n->m_item_count,
              accum);
            accum += n->m_item_count;
            n = forward ? n->m_next_ql_node : n->m_prev_ql_node;
        }
    }

    if (!n)
        return 0;

    D("Found node: %p at accum %llu, idx %llu, sub+ %llu, sub- %llu", (void *)n,
      accum, index, index - accum, (-index) - 1 + accum);

    m_node = n;
    if (forward) {
        /* forward = normal head-to-tail offset. */
        m_offset = index - accum;
    } else {
        /* reverse = need negative offset for tail-to-head, so undo
         * the result of the original if (index < 0) above. */
        m_offset = (-index) - 1 + accum;
    }

    quicklist::quicklistDecompressNodeForUse(m_node);
    m_zip_list = ziplistIndex(m_node->m_ql_LZF, m_offset);
    ziplistGet(m_zip_list, &m_value, &m_size, &m_longval);
    /* The caller will use our result, so we don't re-compress here.
     * The caller can recompress or delete the node as needed. */
    return 1;
}

quicklistEntry::quicklistEntry(const quicklist *in_ql, const long long idx)
{
    quicklistIndex(in_ql, idx);
}

/* Rotate quicklist by moving the tail element to the head. */
void quicklistRotate(quicklist *in_ql) {
    if (in_ql->m_count_total_entries <= 1)
        return;

    /* First, get the tail entry */
    unsigned char *p = ziplistIndex(in_ql->m_tail_ql_node->m_ql_LZF, -1);
    unsigned char *value;
    long long longval;
    unsigned int sz;
    char longstr[32] = {0};
    ziplistGet(p, &value, &sz, &longval);

    /* If value found is NULL, then ziplistGet populated longval instead */
    if (!value) {
        /* Write the longval as a string so we can re-add it */
        sz = ll2string(longstr, sizeof(longstr), longval);
        value = (unsigned char *)longstr;
    }

    /* Add tail entry to head (must happen before tail is deleted). */
    quicklistPushHead(in_ql, value, sz);

    /* If quicklist has only one node, the head ziplist is also the
     * tail ziplist and PushHead() could have reallocated our single ziplist,
     * which would make our pre-existing 'p' unusable. */
    if (in_ql->m_num_ql_nodes == 1) {
        p = ziplistIndex(in_ql->m_tail_ql_node->m_ql_LZF, -1);
    }

    /* Remove tail entry. */
    quicklistDelIndex(in_ql, in_ql->m_tail_ql_node, &p);
}

/* pop from quicklist and return result in 'data' ptr.  Value of 'data'
 * is the return value of 'saver' function pointer if the data is NOT a number.
 *
 * If the quicklist element is a long long, then the return value is returned in
 * 'sval'.
 *
 * Return value of 0 means no elements available.
 * Return value of 1 means check 'data' and 'sval' for values.
 * If 'data' is set, use 'data' and 'sz'.  Otherwise, use 'sval'. */
int quicklistPopCustom(quicklist *in_ql, int in_where, unsigned char **in_data,
                       unsigned int *in_size, long long *in_sval,
                       void *(*in_saver)(unsigned char *data, unsigned int sz)) {
    unsigned char *p;
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    int pos = (in_where == QUICKLIST_HEAD) ? 0 : -1;

    if (in_ql->m_count_total_entries == 0)
        return 0;

    if (in_data)
        *in_data = NULL;
    if (in_size)
        *in_size = 0;
    if (in_sval)
        *in_sval = -123456789;

    quicklistNode *node;
    if (in_where == QUICKLIST_HEAD && in_ql->m_head_ql_node) {
        node = in_ql->m_head_ql_node;
    } else if (in_where == QUICKLIST_TAIL && in_ql->m_tail_ql_node) {
        node = in_ql->m_tail_ql_node;
    } else {
        return 0;
    }

    p = ziplistIndex(node->m_ql_LZF, pos);
    if (ziplistGet(p, &vstr, &vlen, &vlong)) {
        if (vstr) {
            if (in_data)
                *in_data = (unsigned char *)in_saver(vstr, vlen);
            if (in_size)
                *in_size = vlen;
        } else {
            if (in_data)
                *in_data = NULL;
            if (in_sval)
                *in_sval = vlong;
        }
        quicklistDelIndex(in_ql, node, &p);
        return 1;
    }
    return 0;
}

/* Return a malloc'd copy of data passed in */
REDIS_STATIC void *_quicklistSaver(unsigned char *data, unsigned int sz) {
    unsigned char *vstr;
    if (data) {
        vstr = (unsigned char *)zmalloc(sz);
        memcpy(vstr, data, sz);
        return vstr;
    }
    return NULL;
}

/* Default pop function
 *
 * Returns malloc'd value from quicklist */
int quicklistPop(quicklist *in_ql, int in_where, unsigned char **in_data,
                 unsigned int *in_size, long long *in_slong) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    if (in_ql->m_count_total_entries == 0)
        return 0;
    int ret = quicklistPopCustom(in_ql, in_where, &vstr, &vlen, &vlong,
                                 _quicklistSaver);
    if (in_data)
        *in_data = vstr;
    if (in_slong)
        *in_slong = vlong;
    if (in_size)
        *in_size = vlen;
    return ret;
}

/* Wrapper to allow argument-based switching between HEAD/TAIL pop */
void quicklistPush(quicklist *in_ql, void *in_value, const size_t in_size,
                   int in_where) {
    if (in_where == QUICKLIST_HEAD) {
        quicklistPushHead(in_ql, in_value, in_size);
    } else if (in_where == QUICKLIST_TAIL) {
        quicklistPushTail(in_ql, in_value, in_size);
    }
}

/* The rest of this file is test cases and test helpers. */
#ifdef REDIS_TEST
#include <stdint.h>
#include <sys/time.h>

#define assert(_e)                                                             \
    do {                                                                       \
        if (!(_e)) {                                                           \
            printf("\n\n=== ASSERTION FAILED ===\n");                          \
            printf("==> %s:%d '%s' is not true\n", __FILE__, __LINE__, #_e);   \
            err++;                                                             \
        }                                                                      \
    } while (0)

#define yell(str, ...) printf("ERROR! " str "\n\n", __VA_ARGS__)

#define OK printf("\tOK\n")

#define ERROR                                                                  \
    do {                                                                       \
        printf("\tERROR!\n");                                                  \
        err++;                                                                 \
    } while (0)

#define ERR(x, ...)                                                            \
    do {                                                                       \
        printf("%s:%s:%d:\t", __FILE__, __FUNCTION__, __LINE__);               \
        printf("ERROR! " x "\n", __VA_ARGS__);                                 \
        err++;                                                                 \
    } while (0)

#define TEST(name) printf("test — %s\n", name);
#define TEST_DESC(name, ...) printf("test — " name "\n", __VA_ARGS__);

#define QL_TEST_VERBOSE 0

#define UNUSED(x) (void)(x)
static void ql_info(quicklist *ql) {
#if QL_TEST_VERBOSE
    printf("Container length: %lu\n", ql->m_num_ql_nodes);
    printf("Container size: %lu\n", ql->m_count_total_entries);
    if (ql->m_head_ql_node)
        printf("\t(zsize head: %d)\n", ziplistLen(ql->m_head_ql_node->zl));
    if (ql->m_tail_ql_node)
        printf("\t(zsize tail: %d)\n", ziplistLen(ql->m_tail_ql_node->zl));
    printf("\n");
#else
    UNUSED(ql);
#endif
}

/* Return the UNIX time in microseconds */
static long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec) * 1000000;
    ust += tv.tv_usec;
    return ust;
}

/* Return the UNIX time in milliseconds */
static long long mstime(void) { return ustime() / 1000; }

/* Iterate over an entire quicklist.
 * Print the list if 'print' == 1.
 *
 * Returns physical count of elements found by iterating over the list. */
static int _itrprintr(quicklist *ql, int print, int forward) {
    quicklistIter *iter =
        quicklistGetIterator(ql, forward ? AL_START_HEAD : AL_START_TAIL);
    quicklistEntry entry;
    int i = 0;
    int p = 0;
    quicklistNode *prev = NULL;
    while (iter->quicklistNext(entry)) {
        if (entry.node != prev) {
            /* Count the number of list nodes too */
            p++;
            prev = entry.node;
        }
        if (print) {
            printf("[%3d (%2d)]: [%.*s] (%lld)\n", i, p, entry.sz,
                   (char *)entry.value, entry.longval);
        }
        i++;
    }
    quicklistReleaseIterator(iter);
    return i;
}
static int itrprintr(quicklist *ql, int print) {
    return _itrprintr(ql, print, 1);
}

static int itrprintr_rev(quicklist *ql, int print) {
    return _itrprintr(ql, print, 0);
}

#define ql_verify(a, b, c, d, e)                                               \
    do {                                                                       \
        err += _ql_verify((a), (b), (c), (d), (e));                            \
    } while (0)

/* Verify list metadata matches physical list contents. */
static int _ql_verify(quicklist *ql, uint32_t len, uint32_t count,
                      uint32_t head_count, uint32_t tail_count) {
    int errors = 0;

    ql_info(ql);
    if (len != ql->m_num_ql_nodes) {
        yell("quicklist length wrong: expected %d, got %u", len, ql->m_num_ql_nodes);
        errors++;
    }

    if (count != ql->m_count_total_entries) {
        yell("quicklist count wrong: expected %d, got %lu", count, ql->m_count_total_entries);
        errors++;
    }

    int loopr = itrprintr(ql, 0);
    if (loopr != (int)ql->m_count_total_entries) {
        yell("quicklist cached count not match actual count: expected %lu, got "
             "%d",
             ql->m_count_total_entries, loopr);
        errors++;
    }

    int rloopr = itrprintr_rev(ql, 0);
    if (loopr != rloopr) {
        yell("quicklist has different forward count than reverse count!  "
             "Forward count is %d, reverse count is %d.",
             loopr, rloopr);
        errors++;
    }

    if (ql->m_num_ql_nodes == 0 && !errors) {
        OK;
        return errors;
    }

    if (ql->m_head_ql_node && head_count != ql->m_head_ql_node->m_count_items &&
        head_count != ziplistLen(ql->m_head_ql_node->zl)) {
        yell("quicklist head count wrong: expected %d, "
             "got cached %d vs. actual %d",
             head_count, ql->m_head_ql_node->m_count_items, ziplistLen(ql->m_head_ql_node->zl));
        errors++;
    }

    if (ql->m_tail_ql_node && tail_count != ql->m_tail_ql_node->m_count_items &&
        tail_count != ziplistLen(ql->m_tail_ql_node->zl)) {
        yell("quicklist tail count wrong: expected %d, "
             "got cached %u vs. actual %d",
             tail_count, ql->m_tail_ql_node->m_count_items, ziplistLen(ql->m_tail_ql_node->zl));
        errors++;
    }

    if (quicklistAllowsCompression(ql)) {
        quicklistNode *node = ql->m_head_ql_node;
        unsigned int low_raw = ql->compress;
        unsigned int high_raw = ql->m_num_ql_nodes - ql->compress;

        for (unsigned int at = 0; at < ql->m_num_ql_nodes; at++, node = node->next) {
            if (node && (at < low_raw || at >= high_raw)) {
                if (node->encoding != QUICKLIST_NODE_ENCODING_RAW) {
                    yell("Incorrect compression: node %d is "
                         "compressed at depth %d ((%u, %u); total "
                         "nodes: %u; size: %u; recompress: %d)",
                         at, ql->compress, low_raw, high_raw, ql->m_num_ql_nodes, node->sz,
                         node->m_recompress);
                    errors++;
                }
            } else {
                if (node->encoding != QUICKLIST_NODE_ENCODING_LZF &&
                    !node->m_attempted_compress) {
                    yell("Incorrect non-compression: node %d is NOT "
                         "compressed at depth %d ((%u, %u); total "
                         "nodes: %u; size: %u; recompress: %d; attempted: %d)",
                         at, ql->compress, low_raw, high_raw, ql->m_num_ql_nodes, node->sz,
                         node->m_recompress, node->m_attempted_compress);
                    errors++;
                }
            }
        }
    }

    if (!errors)
        OK;
    return errors;
}

/* Generate new string concatenating integer i against string 'prefix' */
static char *genstr(char *prefix, int i) {
    static char result[64] = {0};
    snprintf(result, sizeof(result), "%s%d", prefix, i);
    return result;
}

/* main test, but callable from other files */
int quicklistTest(int argc, char *argv[]) {
    UNUSED(argc);
    UNUSED(argv);

    unsigned int err = 0;
    int optimize_start =
        -(int)(sizeof(optimization_level) / sizeof(*optimization_level));

    printf("Starting optimization offset at: %d\n", optimize_start);

    int options[] = {0, 1, 2, 3, 4, 5, 6, 10};
    size_t option_count = sizeof(options) / sizeof(*options);
    long long runtime[option_count];

    for (int _i = 0; _i < (int)option_count; _i++) {
        printf("Testing Option %d\n", options[_i]);
        long long start = mstime();

        TEST("create list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("add to tail of empty list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushTail(ql, "hello", 6);
            /* 1 for head and 1 for tail beacuse 1 node = head = tail */
            ql_verify(ql, 1, 1, 1, 1);
            quicklistRelease(ql);
        }

        TEST("add to head of empty list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, "hello", 6);
            /* 1 for head and 1 for tail beacuse 1 node = head = tail */
            ql_verify(ql, 1, 1, 1, 1);
            quicklistRelease(ql);
        }

        for (int f = optimize_start; f < 32; f++) {
            TEST_DESC("add to tail 5x at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 5; i++)
                    quicklistPushTail(ql, genstr("hello", i), 32);
                if (ql->m_count_total_entries != 5)
                    ERROR;
                if (f == 32)
                    ql_verify(ql, 1, 5, 5, 5);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 32; f++) {
            TEST_DESC("add to head 5x at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 5; i++)
                    quicklistPushHead(ql, genstr("hello", i), 32);
                if (ql->m_count_total_entries != 5)
                    ERROR;
                if (f == 32)
                    ql_verify(ql, 1, 5, 5, 5);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 512; f++) {
            TEST_DESC("add to tail 500x at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i), 64);
                if (ql->m_count_total_entries != 500)
                    ERROR;
                if (f == 32)
                    ql_verify(ql, 16, 500, 32, 20);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 512; f++) {
            TEST_DESC("add to head 500x at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushHead(ql, genstr("hello", i), 32);
                if (ql->m_count_total_entries != 500)
                    ERROR;
                if (f == 32)
                    ql_verify(ql, 16, 500, 20, 32);
                quicklistRelease(ql);
            }
        }

        TEST("rotate empty") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistRotate(ql);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        for (int f = optimize_start; f < 32; f++) {
            TEST("rotate one val once") {
                quicklist *ql = quicklistNew(f, options[_i]);
                quicklistPushHead(ql, "hello", 6);
                quicklistRotate(ql);
                /* Ignore compression verify because ziplist is
                 * too small to compress. */
                ql_verify(ql, 1, 1, 1, 1);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 3; f++) {
            TEST_DESC("rotate 500 val 5000 times at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                quicklistPushHead(ql, "900", 3);
                quicklistPushHead(ql, "7000", 4);
                quicklistPushHead(ql, "-1200", 5);
                quicklistPushHead(ql, "42", 2);
                for (int i = 0; i < 500; i++)
                    quicklistPushHead(ql, genstr("hello", i), 64);
                ql_info(ql);
                for (int i = 0; i < 5000; i++) {
                    ql_info(ql);
                    quicklistRotate(ql);
                }
                if (f == 1)
                    ql_verify(ql, 504, 504, 1, 1);
                else if (f == 2)
                    ql_verify(ql, 252, 504, 2, 2);
                else if (f == 32)
                    ql_verify(ql, 16, 504, 32, 24);
                quicklistRelease(ql);
            }
        }

        TEST("pop empty") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPop(ql, QUICKLIST_HEAD, NULL, NULL, NULL);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("pop 1 string from 1") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            char *populate = genstr("hello", 331);
            quicklistPushHead(ql, populate, 32);
            unsigned char *data;
            unsigned int sz;
            long long lv;
            ql_info(ql);
            quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
            assert(data != NULL);
            assert(sz == 32);
            if (strcmp(populate, (char *)data))
                ERR("Pop'd value (%.*s) didn't equal original value (%s)", sz,
                    data, populate);
            zfree(data);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("pop head 1 number from 1") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, "55513", 5);
            unsigned char *data;
            unsigned int sz;
            long long lv;
            ql_info(ql);
            quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
            assert(data == NULL);
            assert(lv == 55513);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("pop head 500 from 500") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            ql_info(ql);
            for (int i = 0; i < 500; i++) {
                unsigned char *data;
                unsigned int sz;
                long long lv;
                int ret = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
                assert(ret == 1);
                assert(data != NULL);
                assert(sz == 32);
                if (strcmp(genstr("hello", 499 - i), (char *)data))
                    ERR("Pop'd value (%.*s) didn't equal original value (%s)",
                        sz, data, genstr("hello", 499 - i));
                zfree(data);
            }
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("pop head 5000 from 500") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            for (int i = 0; i < 5000; i++) {
                unsigned char *data;
                unsigned int sz;
                long long lv;
                int ret = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
                if (i < 500) {
                    assert(ret == 1);
                    assert(data != NULL);
                    assert(sz == 32);
                    if (strcmp(genstr("hello", 499 - i), (char *)data))
                        ERR("Pop'd value (%.*s) didn't equal original value "
                            "(%s)",
                            sz, data, genstr("hello", 499 - i));
                    zfree(data);
                } else {
                    assert(ret == 0);
                }
            }
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("iterate forward over 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            quicklistIter iter(ql, AL_START_HEAD);
            quicklistEntry entry;
            int i = 499, count = 0;
            while (iter.quicklistNext(entry)) {
                char *h = genstr("hello", i);
                if (strcmp((char *)entry.value, h))
                    ERR("value [%s] didn't match [%s] at position %d",
                        entry.value, h, i);
                i--;
                count++;
            }
            if (count != 500)
                ERR("Didn't iterate over exactly 500 elements (%d)", i);
            ql_verify(ql, 16, 500, 20, 32);
            quicklistRelease(ql);
        }

        TEST("iterate reverse over 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            quicklistIter iter(ql, AL_START_TAIL);
            quicklistEntry entry;
            int i = 0;
            while (iter.quicklistNext(entry)) {
                char *h = genstr("hello", i);
                if (strcmp((char *)entry.value, h))
                    ERR("value [%s] didn't match [%s] at position %d",
                        entry.value, h, i);
                i++;
            }
            if (i != 500)
                ERR("Didn't iterate over exactly 500 elements (%d)", i);
            ql_verify(ql, 16, 500, 20, 32);
            quicklistRelease(ql);
        }

        TEST("insert before with 0 elements") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistEntry entry(ql, 0);
            quicklistInsertBefore(ql, &entry, "abc", 4);
            ql_verify(ql, 1, 1, 1, 1);
            quicklistRelease(ql);
        }

        TEST("insert after with 0 elements") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistEntry entry(ql, 0);
            quicklistInsertAfter(ql, &entry, "abc", 4);
            ql_verify(ql, 1, 1, 1, 1);
            quicklistRelease(ql);
        }

        TEST("insert after 1 element") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, "hello", 6);
            quicklistEntry entry(ql, 0);
            quicklistInsertAfter(ql, &entry, "abc", 4);
            ql_verify(ql, 1, 2, 2, 2);
            quicklistRelease(ql);
        }

        TEST("insert before 1 element") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, "hello", 6);
            quicklistEntry entry(ql, 0);
            quicklistInsertAfter(ql, &entry, "abc", 4);
            ql_verify(ql, 1, 2, 2, 2);
            quicklistRelease(ql);
        }

        for (int f = optimize_start; f < 12; f++) {
            TEST_DESC("insert once in elements while iterating at fill %d at "
                      "compress %d\n",
                      f, options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                quicklistPushTail(ql, "abc", 3);
                quicklistSetFill(ql, 1);
                quicklistPushTail(ql, "def", 3); /* force to unique node */
                quicklistSetFill(ql, f);
                quicklistPushTail(ql, "bob", 3); /* force to reset for +3 */
                quicklistPushTail(ql, "foo", 3);
                quicklistPushTail(ql, "zoo", 3);

                itrprintr(ql, 0);
                /* insert "bar" before "bob" while iterating over list. */
                quicklistIter iter(ql, AL_START_HEAD);
                quicklistEntry entry;
                while (iter.quicklistNext(entry)) {
                    if (!strncmp((char *)entry.value, "bob", 3)) {
                        /* Insert as fill = 1 so it spills into new node. */
                        quicklistInsertBefore(ql, &entry, "bar", 3);
                        break; /* didn't we fix insert-while-iterating? */
                    }
                }
                itrprintr(ql, 0);

                /* verify results */
                entry.quicklistIndex(ql, 0);
                if (strncmp((char *)entry.value, "abc", 3))
                    ERR("Value 0 didn't match, instead got: %.*s", entry.sz,
                        entry.value);
                entry.quicklistIndex(ql, 1);
                if (strncmp((char *)entry.value, "def", 3))
                    ERR("Value 1 didn't match, instead got: %.*s", entry.sz,
                        entry.value);
                entry.quicklistIndex(ql, 2);
                if (strncmp((char *)entry.value, "bar", 3))
                    ERR("Value 2 didn't match, instead got: %.*s", entry.sz,
                        entry.value);
                entry.quicklistIndex(ql, 3);
                if (strncmp((char *)entry.value, "bob", 3))
                    ERR("Value 3 didn't match, instead got: %.*s", entry.sz,
                        entry.value);
                entry.quicklistIndex(ql, 4);
                if (strncmp((char *)entry.value, "foo", 3))
                    ERR("Value 4 didn't match, instead got: %.*s", entry.sz,
                        entry.value);
                entry.quicklistIndex(ql, 5);
                if (strncmp((char *)entry.value, "zoo", 3))
                    ERR("Value 5 didn't match, instead got: %.*s", entry.sz,
                        entry.value);
                quicklistReleaseIterator(iter);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 1024; f++) {
            TEST_DESC(
                "insert [before] 250 new in middle of 500 elements at fill"
                " %d at compress %d",
                f, options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i), 32);
                for (int i = 0; i < 250; i++) {
                    quicklistEntry entry(ql, 250);
                    quicklistInsertBefore(ql, &entry, genstr("abc", i), 32);
                }
                if (f == 32)
                    ql_verify(ql, 25, 750, 32, 20);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 1024; f++) {
            TEST_DESC("insert [after] 250 new in middle of 500 elements at "
                      "fill %d at compress %d",
                      f, options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushHead(ql, genstr("hello", i), 32);
                for (int i = 0; i < 250; i++) {
                    quicklistEntry entry(ql, 250);
                    quicklistInsertAfter(ql, &entry, genstr("abc", i), 32);
                }

                if (ql->m_count_total_entries != 750)
                    ERR("List size not 750, but rather %ld", ql->m_count_total_entries);

                if (f == 32)
                    ql_verify(ql, 26, 750, 20, 32);
                quicklistRelease(ql);
            }
        }

        TEST("duplicate empty list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            ql_verify(ql, 0, 0, 0, 0);
            quicklist *copy = quicklistDup(ql);
            ql_verify(copy, 0, 0, 0, 0);
            quicklistRelease(ql);
            quicklistRelease(copy);
        }

        TEST("duplicate list of 1 element") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, genstr("hello", 3), 32);
            ql_verify(ql, 1, 1, 1, 1);
            quicklist *copy = quicklistDup(ql);
            ql_verify(copy, 1, 1, 1, 1);
            quicklistRelease(ql);
            quicklistRelease(copy);
        }

        TEST("duplicate list of 500") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            ql_verify(ql, 16, 500, 20, 32);

            quicklist *copy = quicklistDup(ql);
            ql_verify(copy, 16, 500, 20, 32);
            quicklistRelease(ql);
            quicklistRelease(copy);
        }

        for (int f = optimize_start; f < 512; f++) {
            TEST_DESC("index 1,200 from 500 list at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i + 1), 32);
                quicklistEntry entry(ql, 1);
                if (!strcmp((char *)entry.value, "hello2"))
                    OK;
                else
                    ERR("Value: %s", entry.value);
                entry.quicklistIndex(ql, 200);
                if (!strcmp((char *)entry.value, "hello201"))
                    OK;
                else
                    ERR("Value: %s", entry.value);
                quicklistRelease(ql);
            }

            TEST_DESC("index -1,-2 from 500 list at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i + 1), 32);
                quicklistEntry entry(ql, -1);
                if (!strcmp((char *)entry.value, "hello500"))
                    OK;
                else
                    ERR("Value: %s", entry.value);
                entry.quicklistIndex(ql, -2);
                if (!strcmp((char *)entry.value, "hello499"))
                    OK;
                else
                    ERR("Value: %s", entry.value);
                quicklistRelease(ql);
            }

            TEST_DESC("index -100 from 500 list at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i + 1), 32);
                quicklistEntry entry(ql, -100);
                if (!strcmp((char *)entry.value, "hello401"))
                    OK;
                else
                    ERR("Value: %s", entry.value);
                quicklistRelease(ql);
            }

            TEST_DESC("index too big +1 from 50 list at fill %d at compress %d",
                      f, options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 50; i++)
                    quicklistPushTail(ql, genstr("hello", i + 1), 32);
                quicklistEntry entry;
                if (entry.quicklistIndex(ql, 50))
                    ERR("Index found at 50 with 50 list: %.*s", entry.sz,
                        entry.value);
                else
                    OK;
                quicklistRelease(ql);
            }
        }

        TEST("delete range empty list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistDelRange(ql, 5, 20);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("delete range of entire node in list of one node") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            for (int i = 0; i < 32; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            ql_verify(ql, 1, 32, 32, 32);
            quicklistDelRange(ql, 0, 32);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("delete range of entire node with overflow counts") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            for (int i = 0; i < 32; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            ql_verify(ql, 1, 32, 32, 32);
            quicklistDelRange(ql, 0, 128);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("delete middle 100 of 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 16, 500, 32, 20);
            quicklistDelRange(ql, 200, 100);
            ql_verify(ql, 14, 400, 32, 20);
            quicklistRelease(ql);
        }

        TEST("delete negative 1 from 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 16, 500, 32, 20);
            quicklistDelRange(ql, -1, 1);
            ql_verify(ql, 16, 499, 32, 19);
            quicklistRelease(ql);
        }

        TEST("delete negative 1 from 500 list with overflow counts") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 16, 500, 32, 20);
            quicklistDelRange(ql, -1, 128);
            ql_verify(ql, 16, 499, 32, 19);
            quicklistRelease(ql);
        }

        TEST("delete negative 100 from 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            quicklistDelRange(ql, -100, 100);
            ql_verify(ql, 13, 400, 32, 16);
            quicklistRelease(ql);
        }

        TEST("delete -10 count 5 from 50 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 50; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 2, 50, 32, 18);
            quicklistDelRange(ql, -10, 5);
            ql_verify(ql, 2, 45, 32, 13);
            quicklistRelease(ql);
        }

        TEST("numbers only list read") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushTail(ql, "1111", 4);
            quicklistPushTail(ql, "2222", 4);
            quicklistPushTail(ql, "3333", 4);
            quicklistPushTail(ql, "4444", 4);
            ql_verify(ql, 1, 4, 4, 4);
            quicklistEntry entry;
            entry.quicklistIndex(ql, 0);
            if (entry.longval != 1111)
                ERR("Not 1111, %lld", entry.longval);
            entry.quicklistIndex(ql, 1);
            if (entry.longval != 2222)
                ERR("Not 2222, %lld", entry.longval);
            entry.quicklistIndex(ql, 2);
            if (entry.longval != 3333)
                ERR("Not 3333, %lld", entry.longval);
            entry.quicklistIndex(ql, 3);
            if (entry.longval != 4444)
                ERR("Not 4444, %lld", entry.longval);
            if (entry.quicklistIndex(ql, 4, &entry))
                ERR("Index past elements: %lld", entry.longval);
            entry.quicklistIndex(ql, -1);
            if (entry.longval != 4444)
                ERR("Not 4444 (reverse), %lld", entry.longval);
            entry.quicklistIndex(ql, -2);
            if (entry.longval != 3333)
                ERR("Not 3333 (reverse), %lld", entry.longval);
            entry.quicklistIndex(ql, -3);
            if (entry.longval != 2222)
                ERR("Not 2222 (reverse), %lld", entry.longval);
            entry.quicklistIndex(ql, -4);
            if (entry.longval != 1111)
                ERR("Not 1111 (reverse), %lld", entry.longval);
            if (entry.quicklistIndex(ql, -5, &entry))
                ERR("Index past elements (reverse), %lld", entry.longval);
            quicklistRelease(ql);
        }

        TEST("numbers larger list read") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            char num[32];
            long long nums[5000];
            for (int i = 0; i < 5000; i++) {
                nums[i] = -5157318210846258176 + i;
                int sz = ll2string(num, sizeof(num), nums[i]);
                quicklistPushTail(ql, num, sz);
            }
            quicklistPushTail(ql, "xxxxxxxxxxxxxxxxxxxx", 20);
            quicklistEntry entry;
            for (int i = 0; i < 5000; i++) {
                entry.quicklistIndex(ql, i);
                if (entry.longval != nums[i])
                    ERR("[%d] Not longval %lld but rather %lld", i, nums[i],
                        entry.longval);
                entry.longval = 0xdeadbeef;
            }
            entry.quicklistIndex(ql, 5000);
            if (strncmp((char *)entry.value, "xxxxxxxxxxxxxxxxxxxx", 20))
                ERR("String val not match: %s", entry.value);
            ql_verify(ql, 157, 5001, 32, 9);
            quicklistRelease(ql);
        }

        TEST("numbers larger list read B") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushTail(ql, "99", 2);
            quicklistPushTail(ql, "98", 2);
            quicklistPushTail(ql, "xxxxxxxxxxxxxxxxxxxx", 20);
            quicklistPushTail(ql, "96", 2);
            quicklistPushTail(ql, "95", 2);
            quicklistReplaceAtIndex(ql, 1, "foo", 3);
            quicklistReplaceAtIndex(ql, -1, "bar", 3);
            quicklistRelease(ql);
            OK;
        }

        for (int f = optimize_start; f < 16; f++) {
            TEST_DESC("lrem test at fill %d at compress %d", f, options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                char *words[] = {"abc", "foo", "bar",  "foobar", "foobared",
                                 "zap", "bar", "test", "foo"};
                char *result[] = {"abc", "foo",  "foobar", "foobared",
                                  "zap", "test", "foo"};
                char *resultB[] = {"abc",      "foo", "foobar",
                                   "foobared", "zap", "test"};
                for (int i = 0; i < 9; i++)
                    quicklistPushTail(ql, words[i], strlen(words[i]));

                /* lrem 0 bar */
                quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
                quicklistEntry entry;
                int i = 0;
                while (iter->quicklistNext(entry)) {
                    if (quicklistCompare(entry.zi, (unsigned char *)"bar", 3)) {
                        quicklistDelEntry(iter);
                    }
                    i++;
                }
                quicklistReleaseIterator(iter);

                /* check result of lrem 0 bar */
                iter = quicklistGetIterator(ql, AL_START_HEAD);
                i = 0;
                int ok = 1;
                while (iter->quicklistNext(entry)) {
                    /* Result must be: abc, foo, foobar, foobared, zap, test,
                     * foo */
                    if (strncmp((char *)entry.value, result[i], entry.sz)) {
                        ERR("No match at position %d, got %.*s instead of %s",
                            i, entry.sz, entry.value, result[i]);
                        ok = 0;
                    }
                    i++;
                }
                quicklistReleaseIterator(iter);

                quicklistPushTail(ql, "foo", 3);

                /* lrem -2 foo */
                iter = quicklistGetIterator(ql, AL_START_TAIL);
                i = 0;
                int del = 2;
                while (iter->quicklistNext(entry)) {
                    if (quicklistCompare(entry.zi, (unsigned char *)"foo", 3)) {
                        quicklistDelEntry(iter);
                        del--;
                    }
                    if (!del)
                        break;
                    i++;
                }
                quicklistReleaseIterator(iter);

                /* check result of lrem -2 foo */
                /* (we're ignoring the '2' part and still deleting all foo
                 * because
                 * we only have two foo) */
                iter = quicklistGetIterator(ql, AL_START_TAIL);
                i = 0;
                size_t resB = sizeof(resultB) / sizeof(*resultB);
                while (iter->quicklistNext(entry)) {
                    /* Result must be: abc, foo, foobar, foobared, zap, test,
                     * foo */
                    if (strncmp((char *)entry.value, resultB[resB - 1 - i],
                                entry.sz)) {
                        ERR("No match at position %d, got %.*s instead of %s",
                            i, entry.sz, entry.value, resultB[resB - 1 - i]);
                        ok = 0;
                    }
                    i++;
                }

                quicklistReleaseIterator(iter);
                /* final result of all tests */
                if (ok)
                    OK;
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 16; f++) {
            TEST_DESC("iterate reverse + delete at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                quicklistPushTail(ql, "abc", 3);
                quicklistPushTail(ql, "def", 3);
                quicklistPushTail(ql, "hij", 3);
                quicklistPushTail(ql, "jkl", 3);
                quicklistPushTail(ql, "oop", 3);

                quicklistEntry entry;
                quicklistIter *iter = quicklistGetIterator(ql, AL_START_TAIL);
                int i = 0;
                while (iter->quicklistNext(entry)) {
                    if (quicklistCompare(entry.zi, (unsigned char *)"hij", 3)) {
                        quicklistDelEntry(iter);
                    }
                    i++;
                }
                quicklistReleaseIterator(iter);

                if (i != 5)
                    ERR("Didn't iterate 5 times, iterated %d times.", i);

                /* Check results after deletion of "hij" */
                iter = quicklistGetIterator(ql, AL_START_HEAD);
                i = 0;
                char *vals[] = {"abc", "def", "jkl", "oop"};
                while (iter->quicklistNext(entry)) {
                    if (!quicklistCompare(entry.zi, (unsigned char *)vals[i],
                                          3)) {
                        ERR("Value at %d didn't match %s\n", i, vals[i]);
                    }
                    i++;
                }
                quicklistReleaseIterator(iter);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 800; f++) {
            TEST_DESC("iterator at index test at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 760; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }

                quicklistEntry entry;
                quicklistIter *iter =
                    quicklistGetIteratorAtIdx(ql, AL_START_HEAD, 437);
                int i = 437;
                while (iter->quicklistNext(entry)) {
                    if (entry.longval != nums[i])
                        ERR("Expected %lld, but got %lld", entry.longval,
                            nums[i]);
                    i++;
                }
                quicklistReleaseIterator(iter);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 40; f++) {
            TEST_DESC("ltrim test A at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 32; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }
                if (f == 32)
                    ql_verify(ql, 1, 32, 32, 32);
                /* ltrim 25 53 (keep [25,32] inclusive = 7 remaining) */
                quicklistDelRange(ql, 0, 25);
                quicklistDelRange(ql, 0, 0);
                quicklistEntry entry;
                for (int i = 0; i < 7; i++) {
                    entry.quicklistIndex(ql, i);
                    if (entry.longval != nums[25 + i])
                        ERR("Deleted invalid range!  Expected %lld but got "
                            "%lld",
                            entry.longval, nums[25 + i]);
                }
                if (f == 32)
                    ql_verify(ql, 1, 7, 7, 7);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 40; f++) {
            TEST_DESC("ltrim test B at fill %d at compress %d", f,
                      options[_i]) {
                /* Force-disable compression because our 33 sequential
                 * integers don't compress and the check always fails. */
                quicklist *ql = quicklistNew(f, QUICKLIST_NOCOMPRESS);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }
                if (f == 32)
                    ql_verify(ql, 2, 33, 32, 1);
                /* ltrim 5 16 (keep [5,16] inclusive = 12 remaining) */
                quicklistDelRange(ql, 0, 5);
                quicklistDelRange(ql, -16, 16);
                if (f == 32)
                    ql_verify(ql, 1, 12, 12, 12);
                quicklistEntry entry;
                entry.quicklistIndex(ql, 0);
                if (entry.longval != 5)
                    ERR("A: longval not 5, but %lld", entry.longval);
                else
                    OK;
                entry.quicklistIndex(ql, -1);
                if (entry.longval != 16)
                    ERR("B! got instead: %lld", entry.longval);
                else
                    OK;
                quicklistPushTail(ql, "bobobob", 7);
                entry.quicklistIndex(ql, -1);
                if (strncmp((char *)entry.value, "bobobob", 7))
                    ERR("Tail doesn't match bobobob, it's %.*s instead",
                        entry.sz, entry.value);
                for (int i = 0; i < 12; i++) {
                    entry.quicklistIndex(ql, i);
                    if (entry.longval != nums[5 + i])
                        ERR("Deleted invalid range!  Expected %lld but got "
                            "%lld",
                            entry.longval, nums[5 + i]);
                }
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 40; f++) {
            TEST_DESC("ltrim test C at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }
                if (f == 32)
                    ql_verify(ql, 2, 33, 32, 1);
                /* ltrim 3 3 (keep [3,3] inclusive = 1 remaining) */
                quicklistDelRange(ql, 0, 3);
                quicklistDelRange(ql, -29,
                                  4000); /* make sure not loop forever */
                if (f == 32)
                    ql_verify(ql, 1, 1, 1, 1);
                quicklistEntry entry(ql, 0);
                if (entry.longval != -5157318210846258173)
                    ERROR;
                else
                    OK;
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 40; f++) {
            TEST_DESC("ltrim test D at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }
                if (f == 32)
                    ql_verify(ql, 2, 33, 32, 1);
                quicklistDelRange(ql, -12, 3);
                if (ql->m_count_total_entries != 30)
                    ERR("Didn't delete exactly three elements!  Count is: %lu",
                        ql->m_count_total_entries);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 72; f++) {
            TEST_DESC("create quicklist from ziplist at fill %d at compress %d",
                      f, options[_i]) {
                unsigned char *zl = ziplistNew();
                long long nums[64];
                char num[64];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    zl =
                        ziplistPush((unsigned char *)zl, (unsigned char *)num, sz, ZIPLIST_TAIL);
                }
                for (int i = 0; i < 33; i++) {
                    zl = ziplistPush((unsigned char *)zl, (unsigned char *)genstr("hello", i),
                                     32, ZIPLIST_TAIL);
                }
                quicklist *ql = quicklistCreateFromZiplist(f, options[_i], zl);
                if (f == 1)
                    ql_verify(ql, 66, 66, 1, 1);
                else if (f == 32)
                    ql_verify(ql, 3, 66, 32, 2);
                else if (f == 66)
                    ql_verify(ql, 1, 66, 66, 66);
                quicklistRelease(ql);
            }
        }

        long long stop = mstime();
        runtime[_i] = stop - start;
    }

    /* Run a longer test of compression depth outside of primary test loop. */
    int list_sizes[] = {250, 251, 500, 999, 1000};
    long long start = mstime();
    for (int list = 0; list < (int)(sizeof(list_sizes) / sizeof(*list_sizes));
         list++) {
        for (int f = optimize_start; f < 128; f++) {
            for (int depth = 1; depth < 40; depth++) {
                /* skip over many redundant test cases */
                TEST_DESC("verify specific compression of interior nodes with "
                          "%d list "
                          "at fill %d at compress %d",
                          list_sizes[list], f, depth) {
                    quicklist *ql = quicklistNew(f, depth);
                    for (int i = 0; i < list_sizes[list]; i++) {
                        quicklistPushTail(ql, genstr("hello TAIL", i + 1), 64);
                        quicklistPushHead(ql, genstr("hello HEAD", i + 1), 64);
                    }

                    quicklistNode *node = ql->m_head_ql_node;
                    unsigned int low_raw = ql->compress;
                    unsigned int high_raw = ql->m_num_ql_nodes - ql->compress;

                    for (unsigned int at = 0; at < ql->m_num_ql_nodes;
                         at++, node = node->next) {
                        if (at < low_raw || at >= high_raw) {
                            if (node->encoding != QUICKLIST_NODE_ENCODING_RAW) {
                                ERR("Incorrect compression: node %d is "
                                    "compressed at depth %d ((%u, %u); total "
                                    "nodes: %u; size: %u)",
                                    at, depth, low_raw, high_raw, ql->m_num_ql_nodes,
                                    node->sz);
                            }
                        } else {
                            if (node->encoding != QUICKLIST_NODE_ENCODING_LZF) {
                                ERR("Incorrect non-compression: node %d is NOT "
                                    "compressed at depth %d ((%u, %u); total "
                                    "nodes: %u; size: %u; attempted: %d)",
                                    at, depth, low_raw, high_raw, ql->m_num_ql_nodes,
                                    node->sz, node->m_attempted_compress);
                            }
                        }
                    }
                    quicklistRelease(ql);
                }
            }
        }
    }
    long long stop = mstime();

    printf("\n");
    for (size_t i = 0; i < option_count; i++)
        printf("Test Loop %02d: %0.2f seconds.\n", options[i],
               (float)runtime[i] / 1000);
    printf("Compressions: %0.2f seconds.\n", (float)(stop - start) / 1000);
    printf("\n");

    if (!err)
        printf("ALL TESTS PASSED!\n");
    else
        ERR("Sorry, not all tests passed!  In fact, %d tests failed.", err);

    return err;
}
#endif
