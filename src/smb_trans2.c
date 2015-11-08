/*****************************************************************************
 *  __________________    _________  _____            _____  .__         ._.
 *  \______   \______ \  /   _____/ /     \          /  _  \ |__| ____   | |
 *   |    |  _/|    |  \ \_____  \ /  \ /  \        /  /_\  \|  _/ __ \  | |
 *   |    |   \|    `   \/        /    Y    \      /    |    |  \  ___/   \|
 *   |______  /_______  /_______  \____|__  / /\   \____|__  |__|\___ |   __
 *          \/        \/        \/        \/  )/           \/        \/   \/
 *
 * This file is part of liBDSM. Copyright © 2014-2015 VideoLabs SAS
 *
 * Author: Julien 'Lta' BALLET <contact@lta.io>
 *
 * liBDSM is released under LGPLv2.1 (or later) and is also available
 * under a commercial license.
 *****************************************************************************
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include "bdsm_debug.h"
#include "smb_message.h"
#include "smb_session_msg.h"
#include "smb_utils.h"
#include "smb_stat.h"

/*
 * Receive trans2 management
 */

static smb_message *smb_tr2_recv(smb_session *s)
{
    smb_message           recv, *res;
    smb_trans2_resp       *tr2;
    size_t                growth;
    int                   remaining;

    if (!smb_session_recv_msg(s, &recv))
        return (NULL);
    tr2         = (smb_trans2_resp *)recv.packet->payload;
    growth      = tr2->total_data_count - tr2->data_count;
    res         = smb_message_grow(&recv, growth);
    if (!res)
        return (NULL);
    res->cursor = recv.payload_size;
    remaining   = (int)tr2->total_data_count -
                  (tr2->data_displacement + tr2->data_count);

    while (remaining > 0)
    {
        remaining = smb_session_recv_msg(s, &recv);
        if (remaining)
        {
            tr2   = (smb_trans2_resp *)recv.packet->payload;
            /*
             * XXX: Why does padding was necessary and is not anymore, i.e.
             * find a reproductible setup where it is
             */
            smb_message_append(res, tr2->payload /* + 2 pad */, tr2->data_count);
            remaining = (int)tr2->total_data_count -
                        (tr2->data_displacement + tr2->data_count);
        }
    }

    return (res);
}

/*
 * Find management
 */

static void smb_tr2_find2_parse_entries(smb_file **files_p, smb_tr2_find2_entry *iter, size_t count, uint8_t *eod)
{
    smb_file *tmp = NULL;
    size_t   i;

    for (i = 0; i < count && (uint8_t *)iter < eod; i++)
    {
        if (iter->name_len != 0)
        {
            // Create a smb_file and fill it
            tmp = calloc(1, sizeof(smb_file));
            if (!tmp)
                return NULL;

            char *dst = NULL;
            tmp->name_len = smb_from_utf16((const char *)iter->name, iter->name_len,
                                           &dst);
            tmp->name = calloc(1, tmp->name_len+1);
            memcpy(tmp->name, dst, tmp->name_len);

            tmp->created    = iter->created;
            tmp->accessed   = iter->accessed;
            tmp->written    = iter->written;
            tmp->changed    = iter->changed;
            tmp->size       = iter->size;
            tmp->alloc_size = iter->alloc_size;
            tmp->attr       = iter->attr;
            tmp->is_dir     = tmp->attr & SMB_ATTR_DIR;

            tmp->next = *files_p;
            *files_p  = tmp;
        }
        iter = (smb_tr2_find2_entry *)(((char *)iter) + iter->next_entry);
    }

    return;
}

static void smb_find_first_parse(smb_message *msg, smb_file **files_p)
{
    smb_trans2_resp       *tr2;
    smb_tr2_findfirst2_params  *params;
    smb_tr2_find2_entry   *iter;
    uint8_t               *eod;
    size_t                count;

    assert(msg != NULL);

    // Let's parse the answer we got from server
    tr2     = (smb_trans2_resp *)msg->packet->payload;
    params  = (smb_tr2_findfirst2_params *)tr2->payload;
    iter    = (smb_tr2_find2_entry *)(tr2->payload + sizeof(smb_tr2_findfirst2_params));
    eod     = msg->packet->payload + msg->payload_size;
    count   = params->count;

    smb_tr2_find2_parse_entries(files_p, iter, count, eod);
    return;
}

static smb_message  *smb_trans2_find_first (smb_session *s, smb_tid tid, const char *pattern)
{
    smb_message           *msg;
    smb_trans2_req        tr2;
    smb_tr2_findfirst2    find;
    size_t                utf_pattern_len, tr2_bct, tr2_param_count;
    char                  *utf_pattern;
    int                   res;
    unsigned int          padding = 0;

    assert(s != NULL && pattern != NULL && tid);

    utf_pattern_len = smb_to_utf16(pattern, strlen(pattern) + 1, &utf_pattern);
    if (utf_pattern_len == 0)
        return (NULL);

    tr2_bct = sizeof(smb_tr2_findfirst2) + utf_pattern_len;
    tr2_param_count = tr2_bct;
    tr2_bct += 3;
    // Adds padding at the end if necessary.
    while ((tr2_bct % 4) != 3)
    {
        padding++;
        tr2_bct++;
    }

    msg = smb_message_new(SMB_CMD_TRANS2);
    if (!msg) {
        free(utf_pattern);
        return (NULL);
    }
    msg->packet->header.tid = tid;

    SMB_MSG_INIT_PKT(tr2);
    tr2.wct                = 15;
    tr2.max_param_count    = 10; // ?? Why not the same or 12 ?
    tr2.max_data_count     = 0xffff;;
    tr2.param_offset       = 68; // Offset of find_first_params in packet;
    tr2.data_count         = 0;
    tr2.data_offset        = 88; // Offset of pattern in packet
    tr2.setup_count        = 1;
    tr2.cmd                = SMB_TR2_FIND_FIRST;
    tr2.total_param_count = tr2_param_count;
    tr2.param_count       = tr2_param_count;
    tr2.bct = tr2_bct; //3 == padding
    SMB_MSG_PUT_PKT(msg, tr2);

    SMB_MSG_INIT_PKT(find);
    find.attrs     = SMB_FIND2_ATTR_DEFAULT;
    find.count     = 1366;     // ??
    find.flags     = SMB_FIND2_FLAG_DEFAULT | SMB_FIND2_FLAG_CLOSE;
    find.interest  = 0x0104;   // 'Find file both directory info'
    SMB_MSG_PUT_PKT(msg, find);
    smb_message_append(msg, utf_pattern, utf_pattern_len);
    while (padding--)
        smb_message_put8(msg, 0);

    res = smb_session_send_msg(s, msg);
    smb_message_destroy(msg);
    free(utf_pattern);

    if (!res)
    {
        BDSM_dbg("Unable to query pattern: %s\n", pattern);
        return (NULL);
    }

    msg = smb_tr2_recv(s);
    return msg;
}

smb_file  *smb_find(smb_session *s, smb_tid tid, const char *pattern)
{
    smb_file              *files = NULL;
    smb_message           *msg;

    assert(s != NULL && pattern != NULL && tid);

    msg = smb_trans2_find_first(s,tid,pattern);
    if (msg != NULL)
    {
        smb_find_first_parse(msg,&files);
        smb_message_destroy(msg);
    }

    return (files);
}

/*
 * Query management
 */

smb_file  *smb_fstat(smb_session *s, smb_tid tid, const char *path)
{
    smb_message           *msg, reply;
    smb_trans2_req        tr2;
    smb_trans2_resp       *tr2_resp;
    smb_tr2_query         query;
    smb_tr2_path_info     *info;
    smb_file              *file;
    size_t                utf_path_len, msg_len;
    char                  *utf_path;
    int                   res, padding = 0;

    assert(s != NULL && path != NULL && tid);

    utf_path_len = smb_to_utf16(path, strlen(path) + 1, &utf_path);
    if (utf_path_len == 0)
        return (0);

    msg_len   = sizeof(smb_trans2_req) + sizeof(smb_tr2_query);
    msg_len  += utf_path_len;
    if (msg_len %4)
        padding = 4 - msg_len % 4;

    msg = smb_message_new(SMB_CMD_TRANS2);
    if (!msg) {
        free(utf_path);
        return (0);
    }
    msg->packet->header.tid = tid;

    SMB_MSG_INIT_PKT(tr2);
    tr2.wct                = 15;
    tr2.total_param_count  = utf_path_len + sizeof(smb_tr2_query);
    tr2.param_count        = tr2.total_param_count;
    tr2.max_param_count    = 2; // ?? Why not the same or 12 ?
    tr2.max_data_count     = 0xffff;
    tr2.param_offset       = 68; // Offset of find_first_params in packet;
    tr2.data_count         = 0;
    tr2.data_offset        = 96; // Offset of pattern in packet
    tr2.setup_count        = 1;
    tr2.cmd                = SMB_TR2_QUERY_PATH;
    tr2.bct                = sizeof(smb_tr2_query) + utf_path_len + padding;
    SMB_MSG_PUT_PKT(msg, tr2);

    SMB_MSG_INIT_PKT(query);
    query.interest   = 0x0107;   // Query File All Info
    SMB_MSG_PUT_PKT(msg, query);

    smb_message_append(msg, utf_path, utf_path_len);
    free(utf_path);

    // Adds padding at the end if necessary.
    while (padding--)
        smb_message_put8(msg, 0);

    res = smb_session_send_msg(s, msg);
    smb_message_destroy(msg);
    if (!res)
    {
        BDSM_dbg("Unable to query pattern: %s\n", path);
        return (NULL);
    }

    if (!smb_session_recv_msg(s, &reply)
        || reply.packet->header.status != NT_STATUS_SUCCESS)
    {
        BDSM_dbg("Unable to recv msg or failure for %s\n", path);
        return (NULL);
    }

    tr2_resp  = (smb_trans2_resp *)reply.packet->payload;
    info      = (smb_tr2_path_info *)(tr2_resp->payload + 4); //+4 is padding
    file      = calloc(1, sizeof(smb_file));
    if (!file)
        return (NULL);

    file->name_len  = smb_from_utf16((const char *)info->name, info->name_len,
                                     &file->name);
    file->name[info->name_len / 2] = 0;

    file->created     = info->created;
    file->accessed    = info->accessed;
    file->written     = info->written;
    file->changed     = info->changed;
    file->alloc_size  = info->alloc_size;
    file->size        = info->size;
    file->attr        = info->attr;
    file->is_dir      = info->is_dir;

    return (file);
}
