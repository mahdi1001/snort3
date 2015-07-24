//--------------------------------------------------------------------------
// Copyright (C) 2014-2015 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2012-2013 Sourcefire, Inc.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------
/*
**  Author(s):  Hui Cao <huica@cisco.com>
**
**  NOTES
**  9.25.2012 - Initial Source Code. Hui Cao
*/

#include "file_mime_log.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "main/snort_types.h"
#include "utils/util.h"
#include "utils/snort_bounds.h"
#include "file_api.h"

#define MAX_FILE                             1024
#define MAX_EMAIL                            1024

/* log flags */
#define MIME_FLAG_MAIL_FROM_PRESENT               0x00000001
#define MIME_FLAG_RCPT_TO_PRESENT                 0x00000002
#define MIME_FLAG_FILENAME_PRESENT                0x00000004
#define MIME_FLAG_EMAIL_HDRS_PRESENT              0x00000008


/* Extract the filename from the header */
int MailLogState::extract_file_name(const char** start, int length, bool* disp_cont)
{
    const char* tmp = NULL;
    const char* end = *start+length;

    if (length <= 0)
        return -1;

    if (!(*disp_cont))
    {
        tmp = SnortStrcasestr(*start, length, "filename");

        if ( tmp == NULL )
            return -1;

        tmp = tmp + 8;
        while ( (tmp < end) && ((isspace(*tmp)) || (*tmp == '=') ))
        {
            tmp++;
        }
    }
    else
        tmp = *start;

    if (tmp < end)
    {
        if (*tmp == '"' || (*disp_cont))
        {
            if (*tmp == '"')
            {
                if (*disp_cont)
                {
                    *disp_cont = false;
                    return (tmp - *start);
                }
                tmp++;
            }
            *start = tmp;
            tmp = SnortStrnPbrk(*start,(end - tmp),"\"");
            if (tmp == NULL )
            {
                if ((end - tmp) > 0 )
                {
                    tmp = end;
                    *disp_cont = true;
                }
                else
                    return -1;
            }
            else
                *disp_cont = false;
            end = tmp;
        }
        else
        {
            *start = tmp;
        }
        return (end - *start);
    }
    else
    {
        return -1;
    }
}

/* accumulate MIME attachment filenames. The filenames are appended by commas */
int MailLogState::log_file_name(const uint8_t* start, int length, bool* disp_cont)
{
    uint8_t* alt_buf;
    int alt_size;
    uint16_t* alt_len;
    int ret=0;
    int cont =0;
    int log_avail = 0;

    if (!start || (length <= 0))
    {
        *disp_cont = false;
        return -1;
    }

    if (*disp_cont)
        cont = 1;

    ret = extract_file_name((const char**)(&start), length, disp_cont);

    if (ret == -1)
        return ret;

    length = ret;

    alt_buf = log_state.filenames;
    alt_size =  MAX_FILE;
    alt_len = &(log_state.file_logged);
    log_avail = alt_size - *alt_len;

    if (!alt_buf || (log_avail <= 0))
        return -1;

    if ( *alt_len > 0 && ((*alt_len + 1) < alt_size))
    {
        if (!cont)
        {
            alt_buf[*alt_len] = ',';
            *alt_len = *alt_len + 1;
        }
    }

    ret = SafeMemcpy(alt_buf + *alt_len, start, length, alt_buf, alt_buf + alt_size);

    if (ret != SAFEMEM_SUCCESS)
    {
        if (*alt_len != 0)
            *alt_len = *alt_len - 1;
        return -1;
    }

    log_state.file_current = *alt_len;
    *alt_len += length;

    log_flags |= MIME_FLAG_FILENAME_PRESENT;

    return 0;
}


void MailLogState::set_file_name_from_log(void* pv)
{
    Flow* ssn = (Flow*)pv; // FIXIT-M eliminate need for cast

    if (log_state.file_logged > log_state.file_current)
    {
        file_api->set_file_name(ssn, log_state.filenames + log_state.file_current,
            log_state.file_logged -log_state.file_current);
    }
    else
    {
        file_api->set_file_name(ssn, NULL, 0);
    }
}

/* Accumulate EOL seperated headers, one or more at a time */
int MailLogState::log_email_hdrs(const uint8_t* start, int length)
{
    int log_avail = 0;
    uint8_t* log_buf;
    uint32_t logged;
    int ret = 0;

    if (length <= 0)
        return -1;

    log_avail = log_depth - hdrs_logged;
    logged = hdrs_logged;
    log_buf = (uint8_t*)emailHdrs;

    if (log_avail <= 0)
    {
        return -1;
    }

    if (length > log_avail )
    {
        length = log_avail;
    }

    /* appended by the EOL \r\n */

    ret = SafeMemcpy(log_buf + hdrs_logged, start, length, log_buf, log_buf+ log_depth);

    if (ret != SAFEMEM_SUCCESS)
    {
        return -1;
    }

    hdrs_logged += length;

    log_flags |= MIME_FLAG_EMAIL_HDRS_PRESENT;

    return 0;
}

/* Accumulate email addresses from RCPT TO and/or MAIL FROM commands. Email addresses are separated
   by comma */
int MailLogState::log_email_id(const uint8_t* start, int length, EmailUserType type)
{
    uint8_t* alt_buf;
    int alt_size;
    uint16_t* alt_len;
    int ret;
    int log_avail=0;
    const uint8_t* tmp_eol;

    if (length <= 0)
        return -1;

    tmp_eol = (uint8_t*)memchr(start, ':', length);
    if (tmp_eol == NULL)
        return -1;

    if ((tmp_eol+1) < (start+length))
    {
        length = length - ( (tmp_eol+1) - start );
        start = tmp_eol+1;
    }
    else
        return -1;

    switch (type)
    {
    case EMAIL_SENDER:
        alt_buf = senders;
        alt_size = MAX_EMAIL;
        alt_len = &(snds_logged);
        break;

    case EMAIL_RECIPIENT:
        alt_buf = recipients;
        alt_size = MAX_EMAIL;
        alt_len = &(rcpts_logged);
        break;

    default:
        return -1;
    }

    log_avail = alt_size - *alt_len;

    if (log_avail <= 0 || !alt_buf)
        return -1;
    else if (log_avail < length)
        length = log_avail;

    if ( *alt_len > 0 && ((*alt_len + 1) < alt_size))
    {
        alt_buf[*alt_len] = ',';
        *alt_len = *alt_len + 1;
    }

    ret = SafeMemcpy(alt_buf + *alt_len, start, length, alt_buf, alt_buf + alt_size);

    if (ret != SAFEMEM_SUCCESS)
    {
        if (*alt_len != 0)
            *alt_len = *alt_len - 1;
        return -1;
    }

    *alt_len += length;

    if (type == EMAIL_SENDER)
        log_flags |= MIME_FLAG_MAIL_FROM_PRESENT;
    else
        log_flags |= MIME_FLAG_RCPT_TO_PRESENT;
    return 0;
}

void MailLogState::get_file_name(uint8_t** buf, uint32_t* len)
{
    *buf = log_state.filenames;
    *len = log_state.file_logged;
}

void MailLogState::get_email_hdrs(uint8_t** buf, uint32_t* len)
{
    *buf = emailHdrs;
    *len = hdrs_logged;

}

void MailLogState::get_email_id(uint8_t** buf, uint32_t* len, EmailUserType type)
{
    if (type == EMAIL_SENDER)
    {
        *buf = senders;
        *len = snds_logged;
    }
    else
    {
        *buf = recipients;
        *len = rcpts_logged;
    }
}

bool MailLogState::is_file_name_present()
{
    if (log_flags & MIME_FLAG_FILENAME_PRESENT)
        return true;
    return false;
}

bool MailLogState::is_email_hdrs_present()
{
    if (log_flags & MIME_FLAG_EMAIL_HDRS_PRESENT)
        return true;
    return false;
}

bool MailLogState::is_email_from_present()
{
    if (log_flags & MIME_FLAG_MAIL_FROM_PRESENT)
        return true;
    return false;
}

bool MailLogState::is_email_to_present()
{
    if (log_flags & MIME_FLAG_RCPT_TO_PRESENT)
        return true;
    return false;
}

MailLogState::MailLogState(MailLogConfig* conf)
{
    if (conf && (conf->log_email_hdrs || conf->log_filename
        || conf->log_mailfrom || conf->log_rcptto))
    {
        uint32_t bufsz = (2* MAX_EMAIL) + MAX_FILE + conf->email_hdrs_log_depth;
        buf = (uint8_t*)SnortAlloc(bufsz);

        if (buf != NULL)
        {
            log_depth = conf->email_hdrs_log_depth;
            recipients = buf;
            rcpts_logged = 0;
            senders = buf + MAX_EMAIL;
            snds_logged = 0;
            log_state.filenames = buf + (2*MAX_EMAIL);
            log_state.file_logged = 0;
            log_state.file_current = 0;
            emailHdrs = buf + (2*MAX_EMAIL) + MAX_FILE;
            hdrs_logged = 0;
        }
    }
}

MailLogState::~MailLogState()
{
    if (buf != NULL)
        free(buf);
}
