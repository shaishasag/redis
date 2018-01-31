/* rio.c is a simple stream-oriented I/O abstraction that provides an interface
 * to write code that can consume/produce data using different concrete input
 * and output devices. For instance the same rdb.c code using the rio
 * abstraction can be used to read and write the RDB format using in-memory
 * buffers or files.
 *
 * A rio object provides the following methods:
 *  read: read from stream.
 *  write: write to stream.
 *  tell: get the current offset.
 *
 * It is also possible to set a 'checksum' method that is used by rio.c in order
 * to compute a checksum of the data written or read, or to query the rio object
 * for the current checksum.
 *
 * ----------------------------------------------------------------------------
 *
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


#include "fmacros.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "rio.h"
#include "util.h"
#include "crc64.h"
#include "config.h"
#include "server.h"

/* ------------------------- Buffer I/O implementation ----------------------- */

/* Returns 1 or 0 for success/failure. */
size_t rioBufferIO::rioWriteSelf(const void *buf, size_t len)
{
    m_ptr = sdscatlen(m_ptr, (char*)buf, len);
    m_pos += len;
    return (size_t)1;
}

/* Returns 1 or 0 for success/failure. */
size_t rioBufferIO::rioReadSelf(void *buf, size_t len)
{
    if (sdslen(m_ptr)-m_pos < len)
        return (size_t)0; /* not enough buffer to return len bytes. */
    memcpy(buf, m_ptr + m_pos, len);
    m_pos += len;
    return (size_t)1;
}

/* Returns read/write position in buffer. */
off_t rioBufferIO::rioTellSelf() {
    return m_pos;
}


rioBufferIO::rioBufferIO(sds s)
: rio()
, m_ptr(s)
, m_pos((off_t)0)
{}

/* --------------------- Stdio file pointer implementation ------------------- */

/* Returns 1 or 0 for success/failure. */
size_t rioFileIO::rioWriteSelf(const void *buf, size_t len)
{
    size_t retval = fwrite(buf, len, 1, m_fp);
    m_buffered += len;

    if (m_autosync &&
        m_buffered >= m_autosync)
    {
        fflush(m_fp);
        aof_fsync(fileno(m_fp));
        m_buffered = 0;
    }
    return retval;
}

/* Returns 1 or 0 for success/failure. */
size_t rioFileIO::rioReadSelf(void *buf, size_t len)
{
    return fread(buf, len, 1, m_fp);
}

/* Returns read/write position in file. */
off_t rioFileIO::rioTellSelf()
{
    return ftello(m_fp);
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
int rioFileIO::rioFlushSelf()
{
    return (fflush(m_fp) == 0) ? 1 : 0;
}

rioFileIO::rioFileIO(FILE* in_fp)
: rio()
, m_fp(in_fp)
, m_buffered((off_t)0)
, m_autosync((off_t)0)
{
}

/* ------------------- File descriptors set implementation ------------------- */

/* Returns 1 or 0 for success/failure.
 * The function returns success as long as we are able to correctly write
 * to at least one file descriptor.
 *
 * When buf is NULL and len is 0, the function performs a flush operation
 * if there is some pending buffer, so this function is also used in order
 * to implement rioFdsetFlush(). */
size_t rioFdsetIO::rioWriteSelf(const void *buf, size_t len)
{
    ssize_t retval;
    int j;
    unsigned char *p = (unsigned char*) buf;
    int doflush = (buf == NULL && len == 0);

    /* To start we always append to our buffer. If it gets larger than
     * a given size, we actually write to the sockets. */
    if (len) {
        m_buf = sdscatlen(m_buf,buf,len);
        len = 0; /* Prevent entering the while below if we don't flush. */
        if (sdslen(m_buf) > PROTO_IOBUF_LEN) doflush = 1;
    }

    if (doflush) {
        p = (unsigned char*) m_buf;
        len = sdslen(m_buf);
    }

    /* Write in little chunks so that when there are big writes we
     * parallelize while the kernel is sending data in background to
     * the TCP socket. */
    while(len) {
        size_t count = len < 1024 ? len : 1024;
        int broken = 0;
        for (j = 0; j < m_numfds; j++) {
            if (m_state[j] != 0) {
                /* Skip FDs already in error. */
                broken++;
                continue;
            }

            /* Make sure to write 'count' bytes to the socket regardless
             * of short writes. */
            size_t nwritten = 0;
            while(nwritten != count) {
                retval = write(m_fds[j],p+nwritten,count-nwritten);
                if (retval <= 0) {
                    /* With blocking sockets, which is the sole user of this
                     * rio target, EWOULDBLOCK is returned only because of
                     * the SO_SNDTIMEO socket option, so we translate the error
                     * into one more recognizable by the user. */
                    if (retval == -1 && errno == EWOULDBLOCK) errno = ETIMEDOUT;
                    break;
                }
                nwritten += retval;
            }

            if (nwritten != count) {
                /* Mark this FD as broken. */
                m_state[j] = errno;
                if (m_state[j] == 0)
                    m_state[j] = EIO;
            }
        }
        if (broken == m_numfds)
            return (size_t)0; /* All the FDs in error. */

        p += count;
        len -= count;
        m_pos += count;
    }

    if (doflush)
        sdsclear(m_buf);

    return (size_t)1;
}

/* Returns 1 or 0 for success/failure. */
size_t rioFdsetIO::rioReadSelf(void *buf, size_t len)
{
    UNUSED(buf);
    UNUSED(len);
    return (size_t)0; /* Error, this target does not support reading. */
}

/* Returns read/write position in file. */
off_t rioFdsetIO::rioTellSelf()
{
    return m_pos;
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
int rioFdsetIO::rioFlushSelf()
{
    /* Our flush is implemented by the write method, that recognizes a
     * buffer set to NULL with a count of zero as a flush request. */
    return rioWriteSelf(NULL, 0);
}


rioFdsetIO::rioFdsetIO(int *fds, int numfds)
: rio()
{
    m_fds = (int *)zmalloc(sizeof(int)*numfds);
    m_state = (int *)zmalloc(sizeof(int)*numfds);
    memcpy(m_fds, fds, sizeof(int)*numfds);
    for (int j = 0; j < numfds; j++)
        m_state[j] = 0;
    m_numfds = numfds;
    m_pos = 0;
    m_buf = sdsempty();
}

/* release the rio stream. */
rioFdsetIO::~rioFdsetIO()
{
    zfree(m_fds);
    zfree(m_state);
    sdsfree(m_buf);
}

/* ---------------------------- Generic functions ---------------------------- */

/* This function can be installed both in memory and file streams when checksum
 * computation is needed. */
void rio::rioGenericUpdateChecksum(rio* prio, const void *buf, size_t len)
{
    prio->m_checksum = crc64(prio->m_checksum, (const unsigned char *)buf, len);
}

/* Set the file-based rio object to auto-fsync every 'bytes' file written.
 * By default this is set to zero that means no automatic file sync is
 * performed.
 *
 * This feature is useful in a few contexts since when we rely on OS write
 * buffers sometimes the OS buffers way too much, resulting in too many
 * disk I/O concentrated in very little time. When we fsync in an explicit
 * way instead the I/O pressure is more distributed across time. */
void rioFileIO::rioSetAutoSync(off_t bytes)
{
    m_autosync = bytes;
}

/* --------------------------- Higher level interface --------------------------
 *
 * The following higher level functions use lower level rio.c functions to help
 * generating the Redis protocol for the Append Only File. */

/* Write multi bulk count in the format: "*<count>\r\n". */
size_t rio::rioWriteBulkCount(char prefix, int count)
{
    char cbuf[128];

    cbuf[0] = prefix;
    int clen = 1+ll2string(cbuf+1,sizeof(cbuf)-1,count);
    cbuf[clen++] = '\r';
    cbuf[clen++] = '\n';
    if (rioWrite(cbuf, clen) == 0)
        return (size_t)0;
    return (size_t)clen;
}

/* Write binary-safe string in the format: "$<count>\r\n<payload>\r\n". */
size_t rio::rioWriteBulkString(const char *buf, size_t len)
{
    size_t nwritten;

    if ((nwritten = rioWriteBulkCount('$',len)) == 0)
        return (size_t)0;
    if (len > 0 && rioWrite(buf,len) == 0)
        return (size_t)0;
    if (rioWrite("\r\n", 2) == 0)
        return (size_t)0;
    return nwritten+len+2;
}

/* Write a long long value in format: "$<count>\r\n<payload>\r\n". */
size_t rio::rioWriteBulkLongLong(long long l)
{
    char lbuf[32];
    unsigned int llen = ll2string(lbuf, sizeof(lbuf), l);
    return rioWriteBulkString(lbuf, llen);
}

/* Write a double value in the format: "$<count>\r\n<payload>\r\n" */
size_t rio::rioWriteBulkDouble(double d)
{
    char dbuf[128];
    unsigned int dlen = snprintf(dbuf, sizeof(dbuf), "%.17g", d);
    return rioWriteBulkString(dbuf, dlen);
}
