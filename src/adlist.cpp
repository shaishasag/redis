/* adlist.c - A generic doubly linked list implementation
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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


#include <stdlib.h>
#include <new>
#include "adlist.h"
#include "zmalloc.h"

list::list()
: m_head(NULL)
, m_tail(NULL)
, m_len(0)
, m_dup(NULL)
, m_free(NULL)
, m_match(NULL)
{

}

list::~list()
{
    listEmpty();
}

/* Create a new list. The created list can be freed with
 * AlFreeList(), but private value of every node need to be freed
 * by the user before to call AlFreeList().
 *
 * On error, NULL is returned. Otherwise the pointer to the new list. */
list *listCreate()
{
    void* list_mem = zmalloc(sizeof(list));
    list *_list = new (list_mem) list;
    return _list;
}

/* Remove all the elements from the list without destroying the list itself. */
void list::listEmpty()
{
    unsigned long len;
    listNode *current, *next;

    current = m_head;
    len = m_len;
    while(len--) {
        next = current->listNextNode();
        if (m_free) m_free(current->m_value);
        zfree(current);
        current = next;
    }
    m_head = m_tail = NULL;
    m_len = 0;
}

/* Free the whole list.
 *
 * This function can't fail. */
void listRelease(list*& _list)
{
    _list->~list();
    zfree(_list);
    _list = NULL;
}

/* Add a new node to the list, to head, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
list* list::listAddNodeHead(void *value)
{
    listNode *node = (listNode *)zmalloc(sizeof(*node));

    if (node == NULL)
        return NULL;

    node->m_value = value;
    if (m_len == 0) {
        m_head = m_tail = node;
        node->m_prev = node->m_next = NULL;
    } else {
        node->m_prev = NULL;
        node->m_next = m_head;
        m_head->m_prev = node;
        m_head = node;
    }
    m_len++;
    return this;
}

/* Add a new node to the list, to tail, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
list* list::listAddNodeTail(void *value)
{
    listNode *node = (listNode *)zmalloc(sizeof(*node));

    if (node == NULL)
        return NULL;
    node->m_value = value;
    if (m_len == 0) {
        m_head = m_tail = node;
        node->m_prev = node->m_next = NULL;
    } else {
        node->m_prev = m_tail;
        node->m_next = NULL;
        m_tail->m_next = node;
        m_tail = node;
    }
    m_len++;
    return this;
}

list* list::listInsertNode(listNode *old_node, void *value, int after)
{
    listNode *node = (listNode *)zmalloc(sizeof(*node));

    if (node == NULL)
        return NULL;
    node->m_value = value;
    if (after) {
        node->m_prev = old_node;
        node->m_next = old_node->listNextNode();
        if (m_tail == old_node) {
            m_tail = node;
        }
    } else {
        node->m_next = old_node;
        node->m_prev = old_node->m_prev;
        if (m_head == old_node) {
            m_head = node;
        }
    }
    if (node->m_prev != NULL) {
        node->m_prev->m_next = node;
    }
    if (node->listNextNode() != NULL) {
        node->listNextNode()->m_prev = node;
    }
    m_len++;
    return this;
}

/* Remove the specified node from the specified list.
 * It's up to the caller to free the private value of the node.
 *
 * This function can't fail. */
void list::listDelNode(listNode *node)
{
    if (node->m_prev)
        node->m_prev->m_next = node->listNextNode();
    else
        m_head = node->listNextNode();
    if (node->listNextNode())
        node->listNextNode()->m_prev = node->m_prev;
    else
        m_tail = node->m_prev;
    if (m_free) m_free(node->m_value);
    zfree(node);
    m_len--;
}

#if 0
/* Returns a list iterator 'iter'. After the initialization every
 * call to listNext() will return the next element of the list.
 *
 * This function can't fail. */
listIter *listGetIterator(list *_list, int direction)
{
    listIter *iter = (listIter *)zmalloc(sizeof(*iter));

    if (iter == NULL)
        return NULL;
    if (direction == AL_START_HEAD)
        iter->listNextNode() = _list.m_head;
    else
        iter->listNextNode() = _list.m_tail;
    iter->direction = direction;
    return iter;
}

/* Release the iterator memory */
void listReleaseIterator(listIter *iter) {
    zfree(iter);
}
/* Create an iterator in the list private iterator structure */
void listRewind(list *list, listIter *li) {
    li->listNextNode() = m_head;
    li->direction = AL_START_HEAD;
}

void listRewindTail(list *list, listIter *li) {
    li->listNextNode() = m_tail;
    li->direction = AL_START_TAIL;
}
#endif

/* Return the next element of an iterator.
 * It's valid to remove the currently returned element using
 * listDelNode(), but not to remove other elements.
 *
 * The function returns a pointer to the next element of the list,
 * or NULL if there are no more elements, so the classical usage patter
 * is:
 *
 * iter = listGetIterator(list,<direction>);
 * while ((node = listNext(iter)) != NULL) {
 *     doSomethingWith(node->listNodeValue());
 * }
 *
 * */

listIter::listIter()
: m_next(NULL)
, m_direction(AL_START_HEAD)
{}

listIter::listIter(list* list_to_iter, int _direct)
//: m_next((_direct == AL_START_HEAD : list_to_iter->m_head ? list_to_iter->m_tail))
//, m_direction(_direct)
{
    if (AL_START_HEAD == _direct)
        m_next = list_to_iter->m_head;
    else
        list_to_iter->m_tail;
    m_direction = _direct;
}

listNode *listIter::listNext()
{
    listNode *current = m_next;

    if (current != NULL) {
        if (m_direction == AL_START_HEAD)
            m_next = current->listNextNode();
        else
            m_next = current->listPrevNode();
    }
    return current;
}

/* Duplicate the whole list. On out of memory NULL is returned.
 * On success a copy of the original list is returned.
 *
 * The 'Dup' method set with listSetDupMethod() function is used
 * to copy the node value. Otherwise the same pointer value of
 * the original node is used as value of the copied node.
 *
 * The original list both on success or error is never modified. */
list *list::listDup()
{
    list *copy;
    listNode *node;

    if ((copy = listCreate()) == NULL)
        return NULL;
    copy->m_dup = m_dup;
    copy->m_free = m_free;
    copy->m_match = m_match;

    listIter iter(this);
    while((node = iter.listNext()) != NULL) {
        void *value;

        if (copy->m_dup) {
            value = copy->m_dup(node->m_value);
            if (value == NULL) {
                listRelease(copy);
                return NULL;
            }
        } else
            value = node->m_value;
        if (copy->listAddNodeTail(value) == NULL) {
            listRelease(copy);
            return NULL;
        }
    }
    return copy;
}

/* Search the list for a node matching a given key.
 * The match is performed using the 'match' method
 * set with listSetMatchMethod(). If no 'match' method
 * is set, the 'value' pointer of every node is directly
 * compared with the 'key' pointer.
 *
 * On success the first matching node pointer is returned
 * (search starts from head). If no matching node exists
 * NULL is returned. */
listNode* list::listSearchKey(void *key)
{
    listIter iter(this);
    listNode *node;

    while((node = iter.listNext()) != NULL) {
        if (m_match) {
            if (m_match(node->m_value, key)) {
                return node;
            }
        } else {
            if (key == node->m_value) {
                return node;
            }
        }
    }
    return NULL;
}

/* Return the element at the specified zero-based index
 * where 0 is the head, 1 is the element next to head
 * and so on. Negative integers are used in order to count
 * from the tail, -1 is the last element, -2 the penultimate
 * and so on. If the index is out of range NULL is returned. */
listNode* list::listIndex(long index) {
    listNode *n;

    if (index < 0) {
        index = (-index)-1;
        n = m_tail;
        while(index-- && n) n = n->m_prev;
    } else {
        n = m_head;
        while(index-- && n) n = n->listNextNode();
    }
    return n;
}

/* Rotate the list removing the tail node and inserting it to the head. */
void list::listRotate() {
    listNode *tail = m_tail;

    if (listLength() <= 1) return;

    /* Detach current tail */
    m_tail = tail->m_prev;
    m_tail->m_next = NULL;
    /* Move it as head */
    m_head->m_prev = tail;
    tail->m_prev = NULL;
    tail->m_next = m_head;
    m_head = tail;
}

/* Add all the elements of the list 'o' at the end of the
 * list 'l'. The list 'other' remains empty but otherwise valid. */
void list::listJoin(list *o) {
    if (o->m_head)
        o->m_head->m_prev = m_tail;

    if (m_tail)
        m_tail->m_next = o->m_head;
    else
        m_head = o->m_head;

    if (o->m_tail) m_tail = o->m_tail;
    m_len += o->m_len;

    /* Setup other as an empty list. */
    o->m_head = o->m_tail = NULL;
    o->m_len = 0;
}
