/* quicklist.h - A generic doubly linked quicklist implementation
 *
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
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

#ifndef __QUICKLIST_H__
#define __QUICKLIST_H__

/* Node, quicklist, and Iterator are the only data structures used currently. */

#include "../../../../Applications/Xcode7.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/include/c++/v1/cstddef"
#include "../../../../Applications/Xcode7.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/include/c++/v1/cstdio"

/* quicklistNode is a 32 byte struct describing a ziplist for a quicklist.
 * We use bit fields keep the quicklistNode at 32 bytes.
 * m_count_items: 16 bits, max 65536 (max zl bytes is 65k, so max count actually < 32k).
 * encoding: 2 bits, RAW=1, LZF=2.
 * m_container: 2 bits, NONE=1, ZIPLIST=2.
 * m_recompress: 1 bit, bool, true if node is temporary decompressed for usage.
 * m_attempted_compress: 1 bit, boolean, used for verifying during testing.
 * m_extra: 12 bits, free for future use; pads out the remainder of 32 bits */
class quicklistNode
{
public:
    struct quicklistNode *m_prev_ql_node;
    struct quicklistNode *m_next_ql_node;
    unsigned char *m_ql_LZF;
    unsigned int m_zip_list_size;             /* ziplist size in bytes */
    unsigned int m_item_count : 16;     /* count of items in ziplist */
    unsigned int m_encoding : 2;   /* RAW==1 or LZF==2 */
    unsigned int m_container : 2;  /* NONE==1 or ZIPLIST==2 */
    unsigned int m_recompress : 1; /* was this node previous compressed? */
    unsigned int m_attempted_compress : 1; /* node can't compress; too small */
    unsigned int m_extra : 10; /* more bits to steal for future usage */
};

/* quicklistLZF is a 4+N byte struct holding 'sz' followed by 'compressed'.
 * 'sz' is byte length of 'compressed' field.
 * 'compressed' is LZF data with total (compressed) length 'sz'
 * NOTE: uncompressed length is stored in quicklistNode->m_zip_list_size.
 * When quicklistNode->m_ql_LZF is compressed, node->m_ql_LZF points to a quicklistLZF */
struct quicklistLZF
{
    unsigned int m_LZF_size; /* LZF size in bytes*/
    char m_compressed[];
};

/* quicklist is a 40 byte struct (on 64-bit systems) describing a quicklist.
 * 'm_count_total_entries' is the number of total entries.
 * 'm_num_ql_nodes' is the number of quicklist nodes.
 * 'compress' is: -1 if compression disabled, otherwise it's the number
 *                of quicklistNodes to leave uncompressed at ends of quicklist.
 * 'm_fill_factor' is the user-requested (or default) fill factor. */
class quicklist
{
public:
    quicklistNode *m_head_ql_node;
    quicklistNode *m_tail_ql_node;
    unsigned long m_count_total_entries;        /* total count of all entries in all ziplists */
    unsigned long m_num_ql_nodes;          /* number of quicklistNodes */
    int m_fill_factor : 16;              /* fill factor for individual nodes */
    unsigned int m_compress_depth : 16; /* depth of end nodes not to compress;0=off */
};

class quicklistIter
{
public:
    const quicklist *m_quicklist;
    quicklistNode *m_current;
    unsigned char *m_zip_list;
    long m_offset; /* offset in current ziplist */
    int m_direction;
};

class quicklistEntry
{
public:
    quicklistEntry();
    quicklistEntry(const quicklist *in_ql, const long long in_index);
    void initEntry();

    int quicklistIndex(const quicklist *in_ql, const long long in_index);


    const quicklist *m_quicklist;
    quicklistNode *m_node;
    unsigned char *m_zip_list;
    unsigned char *m_value;
    long long m_longval;
    unsigned int m_size;
    int m_offset;
};

#define QUICKLIST_HEAD 0
#define QUICKLIST_TAIL -1

/* quicklist node encodings */
#define QUICKLIST_NODE_ENCODING_RAW 1
#define QUICKLIST_NODE_ENCODING_LZF 2

/* quicklist compression disable */
#define QUICKLIST_NOCOMPRESS 0

/* quicklist container formats */
#define QUICKLIST_NODE_CONTAINER_NONE 1
#define QUICKLIST_NODE_CONTAINER_ZIPLIST 2

#define quicklistNodeIsCompressed(node)                                        \
    ((node)->m_encoding == QUICKLIST_NODE_ENCODING_LZF)

/* Prototypes */
quicklist *quicklistCreate();
quicklist *quicklistNew(int in_fill, int in_compress);
void quicklistSetCompressDepth(quicklist *in_ql, int in_depth);
void quicklistSetFill(quicklist *in_ql, int in_fill);
void quicklistSetOptions(quicklist *in_ql, int in_fill, int in_depth);
void quicklistRelease(quicklist *in_ql);
int quicklistPushHead(quicklist *in_ql, void *in_value, const size_t in_size);
int quicklistPushTail(quicklist *in_ql, void *in_value, const size_t in_size);
void quicklistPush(quicklist *in_ql, void *in_value, const size_t in_size, int in_where);
void quicklistAppendZiplist(quicklist *in_ql, unsigned char *in_ziplist);
quicklist *quicklistAppendValuesFromZiplist(quicklist *in_ql, unsigned char *in_ziplist);
quicklist *quicklistCreateFromZiplist(int in_fill, int in_compress, unsigned char *in_ziplist);
void quicklistInsertAfter(quicklist *in_ql, quicklistEntry *in_node, void *in_value, const size_t in_size);
void quicklistInsertBefore(quicklist *in_ql, quicklistEntry *in_node, void *in_value, const size_t in_size);
void quicklistDelEntry(quicklistIter *in_iter, quicklistEntry *in_entry);
int quicklistReplaceAtIndex(quicklist *in_ql, long in_index, void *in_data, int in_size);
int quicklistDelRange(quicklist *in_ql, const long in_start, const long in_stop);
quicklistIter *quicklistGetIterator(const quicklist *in_ql, int in_direction);
quicklistIter *quicklistGetIteratorAtIdx(const quicklist *in_ql, int in_direction, const long long in_idx);
int quicklistNext(quicklistIter *in_iter, quicklistEntry *in_node);
void quicklistReleaseIterator(quicklistIter *in_iter);
quicklist *quicklistDup(quicklist *in_orig);
void quicklistRewind(quicklist *in_ql, quicklistIter *in_ql_iter);
void quicklistRewindTail(quicklist *in_ql, quicklistIter *in_ql_iter);
void quicklistRotate(quicklist *in_ql);
int quicklistPopCustom(quicklist *in_ql, int in_where, unsigned char **in_data,
                       unsigned int *in_size, long long *in_sval,
                       void *(*in_saver)(unsigned char *data, unsigned int sz));
int quicklistPop(quicklist *in_ql, int in_where, unsigned char **in_data,
                 unsigned int *in_size, long long *in_slong);
unsigned long quicklistCount(const quicklist *in_ql);
int quicklistCompare(unsigned char *in_p1, unsigned char *in_p2, int in_p2_len);
size_t quicklistGetLzf(const quicklistNode *in_node, void **in_data);

#ifdef REDIS_TEST
int quicklistTest(int argc, char *argv[]);
#endif

/* Directions for iterators */
#define AL_START_HEAD 0
#define AL_START_TAIL 1

#endif /* __QUICKLIST_H__ */
