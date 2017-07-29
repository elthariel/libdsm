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

#include "config.h"

#include <assert.h>
#include <iconv.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if HAVE_LANGINFO_H && !defined( __APPLE__ )
# include <langinfo.h>
#endif

#include "bdsm_debug.h"
#include "smb_utils.h"



#import <pthread.h>
#import <assert.h>

static pthread_mutex_t static_iconv_mutex = PTHREAD_MUTEX_INITIALIZER;
static char iconv_locked = 0;


#define iconv_lock() {if (pthread_mutex_lock(&static_iconv_mutex)!=0){\
assert(0);\
}\
set_iconv_locked(1);\
}\

#define iconv_unlock() {if (pthread_mutex_unlock(&static_iconv_mutex)!=0){\
assert(0);\
}\
set_iconv_locked(0);\
}\

static void set_iconv_locked(char locked){
    assert(iconv_locked!=locked);
    iconv_locked = locked;
}

static const char *current_encoding()
{
#if defined( __APPLE__ )
    return "UTF8";
#elif !HAVE_LANGINFO_H
    return "UTF-8";
#else
    static int locale_set = 0;

    if (!locale_set)
    {
        setlocale(LC_ALL, "");
        locale_set = 1;
    }
    //BDSM_dbg("%s\n", nl_langinfo(CODESET));
    return nl_langinfo(CODESET);
#endif
}

static size_t smb_iconv(const char *src, size_t src_len, char **dst,
                        const char *src_enc, const char *dst_enc)
{
    iconv_t   ic;
    size_t    ret = 0;

    assert(src != NULL && dst != NULL && src_enc != NULL && dst_enc != NULL);

    if(src != NULL && dst != NULL && src_enc != NULL && dst_enc != NULL){

        if (!src_len)
        {
            *dst = NULL;
            return 0;
        }
        
        iconv_lock();
        ic = iconv_open(dst_enc, src_enc);
        iconv_unlock();
        
        if (ic == (iconv_t)-1)
        {
            BDSM_dbg("Unable to open iconv to convert from %s to %s\n",
                     src_enc, dst_enc);
            *dst = NULL;
            return 0;
        }
        for (unsigned mul = 4; mul < 16; mul++)
        {
            size_t outlen = mul * src_len;
            char *out = malloc(outlen);

            const char *inp = src;
            size_t inb = src_len;
            char *outp = out;
            size_t outb = outlen;

            if (!out){
                break;
            }
            
            iconv_lock();
            size_t iconvres = iconv(ic, (char **)&inp, &inb, &outp, &outb);
            iconv_unlock();
            
            if (iconvres != (size_t)(-1)) {
                ret = outlen - outb;
                *dst = out;
                break;
            }
            free(out);
            if (errno != E2BIG){
                break;
            }
        }
        
        iconv_lock();
        iconv_close(ic);
        iconv_unlock();
        
        if (ret == 0){
            *dst = NULL;
        }
        
        return ret;
        
    }
    
    return 0;
}

size_t      smb_to_utf16(const char *src, size_t src_len, char **dst)
{
    return (smb_iconv(src, src_len, dst,
                      current_encoding(), "UCS-2LE"));
}

size_t      smb_from_utf16(const char *src, size_t src_len, char **dst)
{
    return (smb_iconv(src, src_len, dst,
                      "UCS-2LE", current_encoding()));
}
