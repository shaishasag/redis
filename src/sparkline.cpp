/* sparkline.c -- ASCII Sparklines
 * This code is modified from http://github.com/antirez/aspark and adapted
 * in order to return SDS strings instead of outputting directly to
 * the terminal.
 *
 * ---------------------------------------------------------------------------
 *
 * Copyright(C) 2011-2014 Salvatore Sanfilippo <antirez@gmail.com>
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

#include "server.h"

#include <math.h>

/* This is the charset used to display the graphs, but multiple rows are used
 * to increase the resolution. */
static char charset[] = "_-`";
static char charset_fill[] = "_o#";
static int charset_len = sizeof(charset)-1;
static int label_margin_top = 1;

/* ----------------------------------------------------------------------------
 * Sequences are arrays of samples we use to represent data to turn
 * into sparklines. This is the API in order to generate a sparkline:
 *
 * sequence *seq = createSparklineSequence();
 * sparklineSequenceAddSample(seq, 10, NULL);
 * sparklineSequenceAddSample(seq, 20, NULL);
 * sparklineSequenceAddSample(seq, 30, "last sample label");
 * sds output = sparklineRender(sdsempty(), seq, 80, 4, SPARKLINE_FILL);
 * freeSparklineSequence(seq);
 * ------------------------------------------------------------------------- */
sequence::sequence()
: m_length(0)
, m_labels(0)
, m_samples(NULL)
, m_min(0.0)
, m_max(0.0)
{

}

sequence::~sequence()
{
    for (int j = 0; j < m_length; j++)
        m_samples[j].~sample();
    zfree(m_samples);
}

/* Create a new sequence. */
sequence *createSparklineSequence(void) {
    void* seq_mem = zmalloc(sizeof(sequence));
    sequence* seq = new (seq_mem) sequence;
    return seq;
}

/* Add a new sample into a sequence. */
void sequence::sparklineSequenceAddSample(double value, char *label) {
    label = (label == NULL || label[0] == '\0') ? NULL : zstrdup(label);
    if (m_length == 0) {
        m_min = m_max = value;
    } else {
        if (value < m_min) m_min = value;
        else if (value > m_max) m_max = value;
    }
    m_samples = (sample *)zrealloc(m_samples,sizeof(sample)*(m_length+1));
    m_samples[m_length].m_value = value;
    m_samples[m_length].m_label = label;
    m_length++;
    if (label) m_labels++;
}

sample::~sample()
{
    zfree(m_label);
}

/* Free a sequence. */
void freeSparklineSequence(sequence *seq) {
    seq->~sequence();
    zfree(seq);
}

/* ----------------------------------------------------------------------------
 * ASCII rendering of sequence
 * ------------------------------------------------------------------------- */

/* Render part of a sequence, so that render_sequence() call call this function
 * with different parts in order to create the full output without overflowing
 * the current terminal columns. */
sds sequence::sparklineRenderRange(sds output, int rows, int offset, int len, int flags) {
    int j;
    double relmax = m_max - m_min;
    int steps = charset_len*rows;
    int row = 0;
    char* chars = (char*)zmalloc(len);
    int loop = 1;
    int opt_fill = flags & SPARKLINE_FILL;
    int opt_log = flags & SPARKLINE_LOG_SCALE;

    if (opt_log) {
        relmax = log(relmax+1);
    } else if (relmax == 0) {
        relmax = 1;
    }

    while(loop) {
        loop = 0;
        memset(chars,' ',len);
        for (j = 0; j < len; j++) {
            sample *s = &m_samples[j+offset];
            double relval = s->m_value - m_min;
            int step;

            if (opt_log) relval = log(relval+1);
            step = (int) (relval*steps)/relmax;
            if (step < 0) step = 0;
            if (step >= steps) step = steps-1;

            if (row < rows) {
                /* Print the character needed to create the sparkline */
                int charidx = step-((rows-row-1)*charset_len);
                loop = 1;
                if (charidx >= 0 && charidx < charset_len) {
                    chars[j] = opt_fill ? charset_fill[charidx] :
                                          charset[charidx];
                } else if(opt_fill && charidx >= charset_len) {
                    chars[j] = '|';
                }
            } else {
                /* Labels spacing */
                if (m_labels && row-rows < label_margin_top) {
                    loop = 1;
                    break;
                }
                /* Print the label if needed. */
                if (s->m_label) {
                    int label_len = strlen(s->m_label);
                    int label_char = row - rows - label_margin_top;

                    if (label_len > label_char) {
                        loop = 1;
                        chars[j] = s->m_label[label_char];
                    }
                }
            }
        }
        if (loop) {
            row++;
            output = sdscatlen(output,chars,len);
            output = sdscatlen(output,"\n",1);
        }
    }
    zfree(chars);
    return output;
}

/* Turn a sequence into its ASCII representation */
sds sequence::sparklineRender(sds output, int columns, int rows, int flags) {
    int j;

    for (j = 0; j < m_length; j += columns) {
        int sublen = (m_length-j) < columns ? (m_length-j) : columns;

        if (j != 0) output = sdscatlen(output,"\n",1);
        output = sparklineRenderRange(output, rows, j, sublen, flags);
    }
    return output;
}

