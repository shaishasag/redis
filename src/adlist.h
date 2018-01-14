/* adlist.h - A generic doubly linked list implementation
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
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

#ifndef __ADLIST_H__
#define __ADLIST_H__

/* Node, List, and Iterator are the only data structures used currently. */

class list;

class listNode
{
friend class list;
public:
    inline listNode* listPrevNode() { return m_prev; }
    inline listNode* listNextNode() { return m_next; }
    inline void*     listNodeValue(){ return m_value;}

    inline void     SetNodeValue(void* new_value){ m_value = new_value;}

private:
    listNode* m_prev;
    listNode* m_next;
    void *m_value;
};


/* Directions for iterators */
const int AL_START_HEAD = 0;
const int AL_START_TAIL = 1;

class listIter
{
friend class list;
public:
    listIter();
    listIter(list* list_to_iter, int direct = AL_START_HEAD);
    listNode *listNext();

private:
    listNode *m_next;
    int m_direction;
};

class list
{
friend class listIter;
public:
    list();
    ~list();
    void listEmpty();
    list *listDup();
    list* listAddNodeHead(void *value);
    list *listAddNodeTail(void *value);
    list *listInsertNode(listNode *old_node, void *value, int after);
    void listDelNode(listNode *node);
    listNode *listSearchKey(void *key);
    listNode *listIndex(long index);
    void listRotate();
    void listJoin(list *o);
    inline unsigned long listLength() {return m_len;}
    inline listNode* listFirst() {return m_head;}
    inline listNode* listLast() {return m_tail;}

    typedef void*(*dup_method_type)(void*);
    typedef void(*free_method_type)(void*);
    typedef int(*match_method_type)(void*, void*);

    void listSetDupMethod(dup_method_type in_dup) {m_dup = in_dup;}
    void listSetFreeMethod(free_method_type in_free) {m_free = in_free;}
    void listSetMatchMethod(match_method_type in_match) {m_match = in_match;}

    dup_method_type listGetDupMethod() {return m_dup;}
    free_method_type listGetFree() {return m_free;}
    match_method_type listGetMatchMethod() {return m_match;}

private:
    listNode *m_head;
    listNode *m_tail;
    dup_method_type m_dup;
    free_method_type m_free;
    match_method_type m_match;
    unsigned long m_len;
};

/* NO more Functions implemented as macros */

/* Prototypes */
list *listCreate();
void listRelease(list*& list);

#if 0
// after C++isation these functions are unsed and have replacments
listIter *listGetIterator(list *list, int direction);
void listReleaseIterator(listIter *iter);
void listRewind(list *list, listIter *li);
void listRewindTail(list *list, listIter *li);
#endif
#endif /* __ADLIST_H__ */
