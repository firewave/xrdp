/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Jay Sorg 2019
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * MS-RDPEAI
 *
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "os_calls.h"
#include "chansrv.h"
#include "log.h"
#include "xrdp_constants.h"
#include "fifo.h"
#include "audin.h"
#include "sound.h"

#define MSG_SNDIN_VERSION       1
#define MSG_SNDIN_FORMATS       2
#define MSG_SNDIN_OPEN          3
#define MSG_SNDIN_OPEN_REPLY    4
#define MSG_SNDIN_DATA_INCOMING 5
#define MSG_SNDIN_DATA          6
#define MSG_SNDIN_FORMATCHANGE  7

#define AUDIN_VERSION 0x00000001

#define AUDIN_NAME "AUDIO_INPUT"
#define AUDIN_FLAGS  1 /* WTS_CHANNEL_OPTION_DYNAMIC */

struct xr_wave_format_ex
{
    int wFormatTag;
    int nChannels;
    int nSamplesPerSec;
    int nAvgBytesPerSec;
    int nBlockAlign;
    int wBitsPerSample;
    int cbSize;
    uint8_t *data;
};

static uint8_t s_pcm_44100_data[] = { 0 };
static struct xr_wave_format_ex s_pcm_44100 =
{
    WAVE_FORMAT_PCM, /* wFormatTag */
    2,               /* num of channels */
    44100,           /* samples per sec */
    176400,          /* avg bytes per sec */
    4,               /* block align */
    16,              /* bits per sample */
    0,               /* data size */
    s_pcm_44100_data /* data */
};

static struct chansrv_drdynvc_procs s_audin_info;
static int s_audin_chanid;
static struct stream *s_in_s;

static struct xr_wave_format_ex *s_server_formats[] =
{
    &s_pcm_44100,
    NULL
};

static struct xr_wave_format_ex **s_client_formats = NULL;

static int s_current_format = 0; /* index in s_client_formats */

/*****************************************************************************/

/*
 * This can be called from sound.c because it includes audin.h.
 */

const char *
audin_wave_format_tag_to_str(int tag)
{
    return
        (tag == WAVE_FORMAT_PCM)        ? "WAVE_FORMAT_PCM" :
        (tag == WAVE_FORMAT_ADPCM)      ? "WAVE_FORMAT_ADPCM" :
        (tag == WAVE_FORMAT_ALAW)       ? "WAVE_FORMAT_ALAW" :
        (tag == WAVE_FORMAT_MULAW)      ? "WAVE_FORMAT_MULAW" :
        (tag == WAVE_FORMAT_MPEGLAYER3) ? "WAVE_FORMAT_MPEGLAYER3" :
        (tag == WAVE_FORMAT_OPUS)       ? "WAVE_FORMAT_OPUS" :
        (tag == WAVE_FORMAT_AAC)        ? "WAVE_FORMAT_AAC" :
        "UNKNOWN";
}

/*****************************************************************************/
static int
cleanup_client_formats(void)
{
    int index;

    if (s_client_formats == NULL)
    {
        return 0;
    }
    index = 0;
    while (s_client_formats[index] != NULL)
    {
        g_free(s_client_formats[index]->data);
        g_free(s_client_formats[index]);
        index++;
    }
    g_free(s_client_formats);
    s_client_formats = NULL;
    return 0;
}

/*****************************************************************************/
static int
audin_send_version(int chan_id)
{
    int error;
    int bytes;
    struct stream *s;

    LOG_DEVEL(LOG_LEVEL_INFO, "audin_send_version:");
    make_stream(s);
    init_stream(s, 32);
    out_uint8(s, MSG_SNDIN_VERSION);
    out_uint32_le(s, AUDIN_VERSION);
    s_mark_end(s);
    bytes = (int) (s->end - s->data);
    error = chansrv_drdynvc_data(chan_id, s->data, bytes);
    free_stream(s);
    return error;
}

/*****************************************************************************/
static int
audin_send_formats(int chan_id)
{
    int error;
    int bytes;
    int num_formats;
    int index;
    struct stream *s;
    struct xr_wave_format_ex *wf;

    LOG_DEVEL(LOG_LEVEL_INFO, "audin_send_formats:");
    num_formats = sizeof(s_server_formats) /
                  sizeof(s_server_formats[0]) - 1;
    make_stream(s);
    init_stream(s, 8192 * num_formats);
    out_uint8(s, MSG_SNDIN_FORMATS);
    out_uint32_le(s, num_formats);
    out_uint32_le(s, 0); /* cbSizeFormatsPacket */
    for (index = 0; index < num_formats; index++)
    {
        wf = s_server_formats[index];
        LOG_DEVEL(LOG_LEVEL_INFO, "audin_send_formats: sending format wFormatTag 0x%4.4x "
                  "nChannels %d nSamplesPerSec %d",
                  wf->wFormatTag, wf->nChannels, wf->nSamplesPerSec);
        out_uint16_le(s, wf->wFormatTag);
        out_uint16_le(s, wf->nChannels);
        out_uint32_le(s, wf->nSamplesPerSec);
        out_uint32_le(s, wf->nAvgBytesPerSec);
        out_uint16_le(s, wf->nBlockAlign);
        out_uint16_le(s, wf->wBitsPerSample);
        out_uint16_le(s, wf->cbSize);
        if (wf->cbSize > 0)
        {
            out_uint8p(s, wf->data, wf->cbSize);
        }
    }
    s_mark_end(s);
    bytes = (int) (s->end - s->data);
    error = chansrv_drdynvc_data(chan_id, s->data, bytes);
    free_stream(s);
    return error;
}

/*****************************************************************************/
static int
audin_send_open(int chan_id)
{
    int error;
    int bytes;
    struct stream *s;
    struct xr_wave_format_ex *wf = s_client_formats[s_current_format];

    LOG_DEVEL(LOG_LEVEL_INFO, "audin_send_open:");
    make_stream(s);
    /* wf->cbSize was checked when the format was received */
    init_stream(s, wf->cbSize + 64);

    out_uint8(s, MSG_SNDIN_OPEN);
    out_uint32_le(s, 2048); /* FramesPerPacket */
    out_uint32_le(s, s_current_format); /* initialFormat */
    out_uint16_le(s, wf->wFormatTag);
    out_uint16_le(s, wf->nChannels);
    out_uint32_le(s, wf->nSamplesPerSec);
    out_uint32_le(s, wf->nAvgBytesPerSec);
    out_uint16_le(s, wf->nBlockAlign);
    out_uint16_le(s, wf->wBitsPerSample);
    bytes = wf->cbSize;
    out_uint16_le(s, bytes);
    if (bytes > 0)
    {
        out_uint8p(s, wf->data, bytes);
    }
    s_mark_end(s);
    bytes = (int) (s->end - s->data);
    error = chansrv_drdynvc_data(chan_id, s->data, bytes);
    free_stream(s);
    return error;
}

/*****************************************************************************/
static int
audin_process_version(int chan_id, struct stream *s)
{
    int version;

    LOG_DEVEL(LOG_LEVEL_INFO, "audin_process_version:");
    if (!s_check_rem(s, 4))
    {
        LOG_DEVEL(LOG_LEVEL_ERROR, "audin_process_version: parse error");
        return 1;
    }
    in_uint32_le(s, version);
    LOG(LOG_LEVEL_INFO, "audin_process_version: version %d", version);
    return audin_send_formats(chan_id);
}

/*****************************************************************************/
static int
audin_process_formats(int chan_id, struct stream *s)
{
    int index;
    int num_formats;
    struct xr_wave_format_ex *wf;

    LOG_DEVEL(LOG_LEVEL_INFO, "audin_process_formats:");
    cleanup_client_formats();
    if (!s_check_rem(s, 8))
    {
        LOG_DEVEL(LOG_LEVEL_ERROR, "audin_process_formats: parse error");
        return 1;
    }
    in_uint32_le(s, num_formats);
    in_uint8s(s, 4); /* cbSizeFormatsPacket */
    s_client_formats = g_new0(struct xr_wave_format_ex *, num_formats + 1);
    for (index = 0; index < num_formats; index++)
    {
        if (!s_check_rem(s, 18))
        {
            LOG_DEVEL(LOG_LEVEL_ERROR, "audin_process_formats: parse error");
            return 1;
        }
        wf = g_new0(struct xr_wave_format_ex, 1);
        s_client_formats[index] = wf;
        in_uint16_le(s, wf->wFormatTag);
        in_uint16_le(s, wf->nChannels);
        in_uint32_le(s, wf->nSamplesPerSec);
        in_uint32_le(s, wf->nAvgBytesPerSec);
        in_uint16_le(s, wf->nBlockAlign);
        in_uint16_le(s, wf->wBitsPerSample);
        in_uint16_le(s, wf->cbSize);

        LOG(LOG_LEVEL_INFO, "audin_process_formats:");
        LOG(LOG_LEVEL_INFO, "      wFormatNo       %d", index);
        LOG(LOG_LEVEL_INFO, "      wFormatTag      %s", audin_wave_format_tag_to_str(wf->wFormatTag));
        LOG(LOG_LEVEL_INFO, "      nChannels       %d", wf->nChannels);
        LOG(LOG_LEVEL_INFO, "      nSamplesPerSec  %d", wf->nSamplesPerSec);
        LOG(LOG_LEVEL_INFO, "      nAvgBytesPerSec %d", wf->nAvgBytesPerSec);
        LOG(LOG_LEVEL_INFO, "      nBlockAlign     %d", wf->nBlockAlign);
        LOG(LOG_LEVEL_INFO, "      wBitsPerSample  %d", wf->wBitsPerSample);
        LOG(LOG_LEVEL_INFO, "      cbSize          %d", wf->cbSize);

        if (wf->cbSize > 0)
        {
            if (!s_check_rem(s, wf->cbSize))
            {
                LOG_DEVEL(LOG_LEVEL_ERROR, "audin_process_formats: parse error");
                return 1;
            }
            wf->data = g_new0(uint8_t, wf->cbSize);
            in_uint8a(s, wf->data, wf->cbSize);
        }
    }
    audin_send_open(chan_id);
    return 0;
}

/*****************************************************************************/
static int
audin_process_open_reply(int chan_id, struct stream *s)
{
    int result;

    if (!s_check_rem(s, 4))
    {
        LOG_DEVEL(LOG_LEVEL_ERROR, "audin_process_open_reply: parse error");
        return 1;
    }
    in_uint32_le(s, result);
    LOG(LOG_LEVEL_INFO, "audin_process_open_reply: result 0x%8.8x", result);
    return 0;
}

/*****************************************************************************/
static int
audin_process_incoming_data(int chan_id, struct stream *s)
{
    LOG_DEVEL(LOG_LEVEL_DEBUG, "audin_process_incoming_data:");
    return 0;
}

/*****************************************************************************/
static int
audin_process_data(int chan_id, struct stream *s)
{
    int data_bytes;
    struct stream *ls;

    data_bytes = (int) (s->end - s->p);
    LOG_DEVEL(LOG_LEVEL_DEBUG, "audin_process_data: data_bytes %d", data_bytes);

    xstream_new(ls, data_bytes);
    g_memcpy(ls->data, s->p, data_bytes);
    ls->p += data_bytes;
    s_mark_end(ls);
    fifo_add_item(g_in_fifo, (void *) ls);
    g_bytes_in_fifo += data_bytes;

    return 0;
}

/*****************************************************************************/
static int
audin_process_format_change(int chan_id, struct stream *s)
{
    LOG_DEVEL(LOG_LEVEL_INFO, "audin_process_format_change:");
    if (!s_check_rem(s, 4))
    {
        LOG_DEVEL(LOG_LEVEL_ERROR, "audin_process_format_change: parse error");
        return 1;
    }
    in_uint32_le(s, s_current_format);
    LOG_DEVEL(LOG_LEVEL_INFO, "audin_process_format_change: s_current_format %d",
              s_current_format);
    return 0;
}

/*****************************************************************************/
static int
audin_process_msg(int chan_id, struct stream *s)
{
    int code;

    LOG_DEVEL(LOG_LEVEL_DEBUG, "audin_process_msg:");
    if (!s_check_rem(s, 1))
    {
        LOG_DEVEL(LOG_LEVEL_ERROR, "audin_process_msg: parse error");
        return 1;
    }
    in_uint8(s, code);
    LOG_DEVEL(LOG_LEVEL_DEBUG, "audin_process_msg: code %d", code);
    switch (code)
    {
        case MSG_SNDIN_VERSION:
            return audin_process_version(chan_id, s);
        case MSG_SNDIN_FORMATS:
            return audin_process_formats(chan_id, s);
        case MSG_SNDIN_OPEN_REPLY:
            return audin_process_open_reply(chan_id, s);
        case MSG_SNDIN_DATA_INCOMING:
            return audin_process_incoming_data(chan_id, s);
        case MSG_SNDIN_DATA:
            return audin_process_data(chan_id, s);
        case MSG_SNDIN_FORMATCHANGE:
            return audin_process_format_change(chan_id, s);
        default:
            LOG_DEVEL(LOG_LEVEL_ERROR, "audin_process_msg: unprocessed code %d", code);
            break;
    }
    return 0;
}

/*****************************************************************************/
static int
audin_open_response(int chan_id, int creation_status)
{
    LOG_DEVEL(LOG_LEVEL_INFO, "audin_open_response: creation_status 0x%8.8x", creation_status);
    if (creation_status == 0)
    {
        return audin_send_version(chan_id);
    }
    return 0;
}

/*****************************************************************************/
static int
audin_close_response(int chan_id)
{
    LOG_DEVEL(LOG_LEVEL_INFO, "audin_close_response:");
    s_audin_chanid = 0;
    cleanup_client_formats();
    free_stream(s_in_s);
    s_in_s = NULL;
    return 0;
}

/*****************************************************************************/
static int
audin_data_fragment(int chan_id, char *data, int bytes)
{
    int rv;

    LOG_DEVEL(LOG_LEVEL_DEBUG, "audin_data_fragment:");
    if (!s_check_rem(s_in_s, bytes))
    {
        LOG_DEVEL(LOG_LEVEL_ERROR, "audin_data_fragment: error bytes %d left %d",
                  bytes, (int) (s_in_s->end - s_in_s->p));
        return 1;
    }
    out_uint8a(s_in_s, data, bytes);
    if (s_in_s->p == s_in_s->end)
    {
        s_in_s->p = s_in_s->data;
        rv = audin_process_msg(chan_id, s_in_s);
        free_stream(s_in_s);
        s_in_s = NULL;
        return rv;
    }
    return 0;
}

/*****************************************************************************/
static int
audin_data_first(int chan_id, char *data, int bytes, int total_bytes)
{
    LOG_DEVEL(LOG_LEVEL_DEBUG, "audin_data_first:");
    if (s_in_s != NULL)
    {
        LOG_DEVEL(LOG_LEVEL_ERROR, "audin_data_first: warning s_in_s is not nil");
        free_stream(s_in_s);
    }
    make_stream(s_in_s);
    init_stream(s_in_s, total_bytes);
    s_in_s->end = s_in_s->data + total_bytes;
    return audin_data_fragment(chan_id, data, bytes);
}

/*****************************************************************************/
static int
audin_data(int chan_id, char *data, int bytes)
{
    struct stream ls;

    LOG_DEVEL_HEXDUMP(LOG_LEVEL_TRACE, "audin_data:", data, bytes);
    if (s_in_s == NULL)
    {
        g_memset(&ls, 0, sizeof(ls));
        ls.data = data;
        ls.p = ls.data;
        ls.end = ls.p + bytes;
        return audin_process_msg(chan_id, &ls);
    }
    return audin_data_fragment(chan_id, data, bytes);
}

/*****************************************************************************/
int
audin_init(void)
{
    LOG_DEVEL(LOG_LEVEL_INFO, "audin_init:");
    g_memset(&s_audin_info, 0, sizeof(s_audin_info));
    s_audin_info.open_response = audin_open_response;
    s_audin_info.close_response = audin_close_response;
    s_audin_info.data_first = audin_data_first;
    s_audin_info.data = audin_data;
    s_audin_chanid = 0;
    s_in_s = NULL;
    return 0;
}

/*****************************************************************************/
int
audin_deinit(void)
{
    LOG_DEVEL(LOG_LEVEL_INFO, "audin_deinit:");
    return 0;
}

/*****************************************************************************/
int
audin_start(void)
{
    int error;

    LOG_DEVEL(LOG_LEVEL_INFO, "audin_start:");
    if (s_audin_chanid != 0 || g_in_fifo == NULL)
    {
        return 1;
    }

    /* if there is any data in FIFO, discard it */
    fifo_clear(g_in_fifo, NULL);
    g_bytes_in_fifo = 0;

    error = chansrv_drdynvc_open(AUDIN_NAME, AUDIN_FLAGS,
                                 &s_audin_info, /* callback functions */
                                 &s_audin_chanid); /* chansrv chan_id */
    LOG_DEVEL(LOG_LEVEL_ERROR, "audin_start: error %d s_audin_chanid %d", error, s_audin_chanid);
    return error;
}

/*****************************************************************************/
int
audin_stop(void)
{
    LOG_DEVEL(LOG_LEVEL_INFO, "audin_stop:");
    chansrv_drdynvc_close(s_audin_chanid);
    return 0;
}
