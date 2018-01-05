/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
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
/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
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

#include <stdint.h>
#include <assert.h>

#ifndef __DICT_H
#define __DICT_H

#define DICT_OK 0
#define DICT_ERR 1

/* Unused arguments generate annoying warnings... */
#define DICT_NOTUSED(V) ((void) V)
class dict;
class dictEntry {
public:
    friend class dict;

    dictEntry(dictEntry *next_entry=NULL);

    inline void*      key()  {return m_key;};
    inline dictEntry* next() {return m_next;}

// previously macros
    inline void*    dictGetKey() const { return m_key; }
    inline void*    dictGetVal() const { return v.val; }
    inline int64_t  dictGetSignedIntegerVal() const { return v.s64; }
    inline uint64_t dictGetUnsignedIntegerVal() const { return v.u64; }
    inline double   dictGetDoubleVal() const { return v.d; }

    inline void dictSetKey(void* new_key) {m_key = new_key;}
    inline void dictSetVal(void* _val_) { v.val = _val_; }
    inline void dictSetSignedIntegerVal(long long _val_) { v.s64 = _val_; }
    inline void dictSetUnsignedIntegerVal(unsigned long long _val_) { v.u64 = _val_; }
    inline void dictSetDoubleVal(double _val_) { v.d = _val_; }

    dictEntry& operator=(const dictEntry& in_to_copy)
    {
        m_key = in_to_copy.m_key;
        v = in_to_copy.v;
        m_next = in_to_copy.m_next;
    }

private:
    void *m_key;
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
    dictEntry *m_next;
} ;

struct dictType {
    uint64_t (*hashFunction)(const void *key);
    void *(*keyDup)(void *privdata, const void *key);
    void *(*valDup)(void *privdata, const void *obj);
    int (*keyCompare)(void *privdata, const void *key1, const void *key2);
    void (*keyDestructor)(void *privdata, void *key);
    void (*valDestructor)(void *privdata, void *obj);
} ;

/* This is our hash table structure. Every dictionary has two of this as we
 * implement incremental rehashing, for the old to the new table. */
class dictht {
public:
    dictht(const unsigned long new_size = 0);
    dictht(dictht&& in_move_me);
    dictht& operator=(dictht&& in_move_me);
    ~dictht();

    void reset();
    void free_table();

    inline bool empty() const {return m_table == NULL;}
    inline dictEntry*& operator[](const size_t ind)
    {
        return m_table[ind];
    }
    inline unsigned long  size() const {return m_size;}
    inline unsigned long  sizemask() const {return m_sizemask;}
    inline unsigned long& used() {return m_used;}

    inline void* peek_table() {return (void*)m_table;} // for dict::dictFingerprint & debugging
private:
    dictEntry **  m_table;
    unsigned long m_size;
    unsigned long m_sizemask;
    unsigned long m_used;

    dictht(const dictht& in_move_me);
    dictht& operator=(const dictht& in_move_me);
};

std::ostream& operator<<(std::ostream& os, dictht& out_me);

typedef void (dictScanFunction)(void *privdata, const dictEntry *de);
typedef void (dictScanBucketFunction)(void *privdata, dictEntry **bucketref);

// remove declarations of functions that became class members
// get rid of macros that should be member functions
class dict {
public:
    dict();
    dict(dictType *in_type, void *in_privDataPtr);
    ~dict();

    inline bool dictIsRehashing() { return rehashidx != -1;}
    int dictResize();
    int dictExpand(unsigned long size);
    int dictRehash(int n);
    void _dictRehashStep(); // should be private?
    int dictAdd(void *key, void *val);
    dictEntry* dictAddRaw(void *key, dictEntry **existing);
    dictEntry* dictAddOrFind(void *key);
    dictEntry* dictUnlink(const void *key);
    dictEntry* dictFind(const void *key);
    dictEntry* dictGetRandomKey();
    int dictReplace(void *key, void *val);
    int dictDelete(const void *key);
    void dictFreeUnlinkedEntry(dictEntry *he);
    void* dictFetchValue(const void *key);
    long long dictFingerprint();
    unsigned int dictGetSomeKeys(dictEntry **des, unsigned int count);
    unsigned long dictScan(unsigned long v, dictScanFunction *fn,
                       dictScanBucketFunction* bucketfn,
                       void *privdata);
    void dictEmpty(void(callback)(void*));
    unsigned int dictGetHash(const void *key);
    dictEntry** dictFindEntryRefByPtrAndHash(const void *oldptr, unsigned int hash);

// previously macros
    inline void dictFreeVal(dictEntry *entry)
    {
        if (type->valDestructor)
            type->valDestructor(privdata, entry->dictGetVal());
    }

    inline void dictSetVal(dictEntry *entry, void* _val_)
    {
        if (type->valDup)
            entry->dictSetVal(type->valDup(privdata, _val_));
        else
            entry->dictSetVal(_val_);
    }

    inline void dictFreeKey(dictEntry *entry)
    {
        if (type->keyDestructor)
            type->keyDestructor(privdata, entry->key());
    }

    inline void dictSetKey(dictEntry *entry, void* _key_)
    {
        if (type->keyDup)
            entry->dictSetKey(type->keyDup(privdata, _key_));
        else
            entry->dictSetKey(_key_);
    }

    inline bool dictCompareKeys(const void* key1, const void* key2)
    {
        bool retVal = type->keyCompare ?
                        type->keyCompare(privdata, key1, key2) :
                        key1 == key2;
        return retVal;
    }

    inline uint64_t dictHashKey(const void* key) { return type->hashFunction(key);}
    inline unsigned long dictSlots() { return ht[0].size()+ht[1].size(); }
    inline unsigned long dictSize() { return ht[0].used()+ht[1].used(); }
//private:
    int _dictKeyIndex(const void *key, unsigned int hash, dictEntry **existing);
    int _dictExpandIfNeeded();
    dictEntry *dictGenericDelete(const void *key, int nofree);
    int _dictClear(dictht *ht, void(callback)(void *));
    
    dictType *type;
    void *privdata;
    dictht ht[2];
    long rehashidx; /* rehashing not in progress if rehashidx == -1 */
    unsigned long iterators; /* number of iterators currently running */
} ;

std::ostream& operator<<(std::ostream& os, dict& out_me);

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only dictNext()
 * should be called while iterating. */
class dictIterator {
public:
    dictIterator(dict *in_d, int in_safe=0);
    ~dictIterator();
    
    dict *d;
    long index;
    int table;
    int safe;
    dictEntry *entry;
    dictEntry *nextEntry;
    /* unsafe iterator fingerprint for misuse detection. */
    long long fingerprint;
};

/* This is the initial size of every hash table */
#define DICT_HT_INITIAL_SIZE     4

/* ------------------------------- Macros ------------------------------------*/

/* API */
dict *dictCreate(dictType *type, void *privDataPtr);
void dictRelease(dict *d);
dictIterator *dictGetIterator(dict *d);
dictIterator *dictGetSafeIterator(dict *d);
dictEntry *dictNext(dictIterator *iter);
void dictReleaseIterator(dictIterator *iter);
void dictGetStats(char *buf, size_t bufsize, dict *d);
uint64_t dictGenHashFunction(const void *key, int len);
uint64_t dictGenCaseHashFunction(const unsigned char *buf, int len);
void dictEnableResize(void);
void dictDisableResize(void);

int dictRehashMilliseconds(dict *d, int ms);
void dictSetHashFunctionSeed(uint8_t *seed);
uint8_t *dictGetHashFunctionSeed(void);

/* Hash table types */
extern dictType dictTypeHeapStringCopyKey;
extern dictType dictTypeHeapStrings;
extern dictType dictTypeHeapStringCopyKeyValue;

#endif /* __DICT_H */
