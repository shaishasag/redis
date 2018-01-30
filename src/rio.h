/*
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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


#ifndef __REDIS_RIO_H
#define __REDIS_RIO_H

#include <stdio.h>
#include <stdint.h>
#include "sds.h"

struct redisObject;
struct rio
{
    inline size_t rioWrite(const void *buf, size_t len);
    inline size_t rioRead(void *buf, size_t len);
    inline off_t rioTell()
    {
        return m_tell_func(this);
    }

    inline int rioFlush()
    {
        return m_flush_func(this);
    }

    size_t rioWriteBulkCount(char prefix, int count);
    size_t rioWriteBulkString(const char *buf, size_t len);
    size_t rioWriteBulkLongLong(long long l);
    size_t rioWriteBulkDouble(double d);
    int rioWriteBulkObject(struct redisObject *obj);

    static void rioGenericUpdateChecksum(rio* prio, const void *buf, size_t len);
    void rioSetAutoSync(off_t bytes);

    /* Backend functions.
     * Since this functions do not tolerate short writes or reads the return
     * value is simplified to: zero on error, non zero on complete success. */
    size_t (*m_read_func)(rio *, void *buf, size_t len);
    size_t (*m_write_func)(rio *, const void *buf, size_t len);
    off_t (*m_tell_func)(rio *);
    int (*m_flush_func)(rio *);
    /* The update_cksum method if not NULL is used to compute the checksum of
     * all the data that was read or written so far. The method should be
     * designed so that can be called with the current checksum, and the buf
     * and len fields pointing to the new block of data to add to the checksum
     * computation. */
    void (*m_update_cksum_func)(rio *, const void *buf, size_t len);

    /* The current checksum */
    uint64_t m_checksum;

    /* number of bytes read or written */
    size_t m_processed_bytes;

    /* maximum single read or write chunk size */
    size_t m_max_processing_chunk;

    /* Backend-specific vars. */
    union {
        /* In-memory buffer target. */
        struct {
            sds ptr;
            off_t pos;
        } buffer;
        /* Stdio file pointer target. */
        struct {
            FILE *fp;
            off_t buffered; /* Bytes written since last fsync. */
            off_t autosync; /* fsync after 'autosync' bytes written. */
        } file;
        /* Multiple FDs target (used to write to N sockets). */
        struct {
            int *fds;       /* File descriptors. */
            int *state;     /* Error state of each fd. 0 (if ok) or errno. */
            int numfds;
            off_t pos;
            sds buf;
        } fdset;
    } io;
};


/* The following functions are our interface with the stream. They'll call the
 * actual implementation of read / write / tell, and will update the checksum
 * if needed. */

inline size_t rio::rioWrite(const void *buf, size_t len) {
    while (len) {
        size_t bytes_to_write = (m_max_processing_chunk && m_max_processing_chunk < len) ? m_max_processing_chunk : len;
        if (m_update_cksum_func) m_update_cksum_func(this, buf,bytes_to_write);
        if (m_write_func(this,buf,bytes_to_write) == 0)
            return 0;
        buf = (char*)buf + bytes_to_write;
        len -= bytes_to_write;
        m_processed_bytes += bytes_to_write;
    }
    return 1;
}

inline size_t rio::rioRead(void *buf, size_t len) {
    while (len) {
        size_t bytes_to_read = (m_max_processing_chunk && m_max_processing_chunk < len) ? m_max_processing_chunk : len;
        if (m_read_func(this,buf,bytes_to_read) == 0)
            return 0;
        if (m_update_cksum_func) m_update_cksum_func(this,buf,bytes_to_read);
        buf = (char*)buf + bytes_to_read;
        len -= bytes_to_read;
        m_processed_bytes += bytes_to_read;
    }
    return 1;
}


void rioInitWithFile(rio *r, FILE *fp);
void rioInitWithBuffer(rio *r, sds s);
void rioInitWithFdset(rio *r, int *fds, int numfds);

void rioFreeFdset(rio *r);

#endif
