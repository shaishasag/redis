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
class rio
{
public:
    rio()
    : m_update_cksum_func(NULL)  /* update_checksum */
    , m_checksum((size_t)0)              /* current checksum */
    , m_processed_bytes((size_t)0)       /* bytes read or written */
    , m_max_processing_chunk((size_t)0)  /* read/write chunk size */
    {}

    inline size_t rioWrite(const void *buf, size_t len);
    inline size_t rioRead(void *buf, size_t len);
    inline off_t rioTell();
    inline int rioFlush();

    size_t rioWriteBulkCount(char prefix, int count);
    size_t rioWriteBulkString(const char *buf, size_t len);
    size_t rioWriteBulkLongLong(long long l);
    size_t rioWriteBulkDouble(double d);
    int rioWriteBulkObject(struct redisObject *obj);

    static void rioGenericUpdateChecksum(rio* prio, const void *buf, size_t len);

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

protected:
    /* Backend functions.
     * Since this functions do not tolerate short writes or reads the return
     * value is simplified to: zero on error, non zero on complete success. */
    virtual size_t rioReadSelf(void *buf, size_t len) = 0;
    virtual size_t rioWriteSelf(const void *buf, size_t len) = 0;
    virtual off_t rioTellSelf() = 0;
    virtual int rioFlushSelf() {return 1;}/* default: do nothing. */
};

/* The following functions are our interface with the stream. They'll call the
 * actual implementation of read / write / tell, and will update the checksum
 * if needed. */

inline size_t rio::rioWrite(const void *buf, size_t len)
{
    while (len) {
        size_t bytes_to_write = (m_max_processing_chunk && m_max_processing_chunk < len) ? m_max_processing_chunk : len;
        if (m_update_cksum_func)
            m_update_cksum_func(this, buf, bytes_to_write);
        if (rioWriteSelf(buf,bytes_to_write) == 0)
            return (size_t)0;
        buf = (char*)buf + bytes_to_write;
        len -= bytes_to_write;
        m_processed_bytes += bytes_to_write;
    }
    return (size_t)1;
}

inline size_t rio::rioRead(void *buf, size_t len)
{
    while (len) {
        size_t bytes_to_read = (m_max_processing_chunk && m_max_processing_chunk < len) ? m_max_processing_chunk : len;
        if (rioReadSelf(buf,bytes_to_read) == 0)
            return (size_t)0;
        if (m_update_cksum_func)
            m_update_cksum_func(this, buf, bytes_to_read);
        buf = (char*)buf + bytes_to_read;
        len -= bytes_to_read;
        m_processed_bytes += bytes_to_read;
    }
    return (size_t)1;
}

inline off_t rio::rioTell()
{
    return rioTellSelf();
}

inline int rio::rioFlush()
{
    return rioFlushSelf();
}

class rioFileIO : public rio
{
public:
    rioFileIO(FILE *fp = NULL);

    void rioSetAutoSync(off_t bytes);

protected:
    virtual size_t rioReadSelf(void *buf, size_t len);
    virtual size_t rioWriteSelf(const void *buf, size_t len);
    virtual off_t rioTellSelf();
    virtual int rioFlushSelf();

    FILE* m_fp;
    off_t m_buffered; /* Bytes written since last fsync. */
    off_t m_autosync; /* fsync after 'autosync' bytes written. */
};

/* In-memory buffer target. */
class rioBufferIO : public rio
{
public:
    rioBufferIO(sds s);

    sds m_ptr;

protected:
    virtual size_t rioReadSelf (void *buf, size_t len);
    virtual size_t rioWriteSelf(const void *buf, size_t len);
    virtual off_t  rioTellSelf ();

    off_t m_pos;
};


class rioFdsetIO : public rio
{
public:
    rioFdsetIO(int *fds, int numfds);
    ~rioFdsetIO();

    int* m_state;     /* Error state of each fd. 0 (if ok) or errno. */

protected:
    virtual size_t rioReadSelf(void *buf, size_t len);
    virtual size_t rioWriteSelf(const void *buf, size_t len);
    virtual off_t rioTellSelf();
    virtual int rioFlushSelf();

    int* m_fds;       /* File descriptors. */
    int  m_numfds;
    off_t m_pos;
    sds m_buf;
};


#endif
