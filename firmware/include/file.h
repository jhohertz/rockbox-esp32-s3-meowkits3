/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Björn Stenberg
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#ifndef _FILE_H_
#define _FILE_H_

#include <sys/types.h>
#include <stdbool.h>
#include <fcntl.h>
/* config.h has to be in scope before the conditional below, not after it:
 * HAVE_SYSTEM_LIBC comes from the target config, and a translation unit such as
 * common/zip.c includes file.h before anything else pulls config.h in, so testing
 * it earlier silently selects neither branch. */
#include "config.h"
#ifdef WIN32
/* this has SEEK_SET et al */
#include <stdio.h>
#elif defined(HAVE_SYSTEM_LIBC)
/* SEEK_SET/SEEK_CUR/SEEK_END belong to <unistd.h> in a POSIX libc. Rockbox's own
 * libc defines them in its fcntl.h, which is why no other target needs this --
 * and it must stay conditional, because Rockbox's libc has no unistd.h at all and
 * including one here would break every native build. Only a target that links a
 * system C library (ESP-IDF on xtensa) needs it, and callers such as
 * buffering.c and common/zip.c use lseek() with these constants without including
 * anything else. */
#include <unistd.h>
#endif
#include "fs_defines.h"
#include "gcc_extensions.h"

enum relate_result
{
                          /* < 0 == failure */
    RELATE_DIFFERENT = 0, /* the two paths are different objects */
    RELATE_SAME,          /* the two paths are the same object */
    RELATE_PREFIX,        /* the path2 contains path1 as a prefix */
};

#if (CONFIG_PLATFORM & PLATFORM_GAME_CONSOLE) && !defined(SIMULATOR)
#include "filesystem-console.h"
#elif defined(APPLICATION) || defined(CHECKWPS)
#include "filesystem-app.h"
#elif defined(SIMULATOR) || defined(DBTOOL)
#include "../../uisimulator/common/filesystem-sim.h"
#else
#include "filesystem-native.h"
#endif

#ifndef FILEFUNCTIONS_DECLARED
int fdprintf(int fildes, const char *fmt, ...) ATTRIBUTE_PRINTF(2, 3);
#endif /* FILEFUNCTIONS_DECLARED */

#ifndef FILEFUNCTIONS_DEFINED
#ifndef open
#define open            FS_PREFIX(open)
#endif
#ifndef creat
#define creat           FS_PREFIX(creat)
#endif
#ifndef close
#define close           FS_PREFIX(close)
#endif
#ifndef ftruncate
#define ftruncate       FS_PREFIX(ftruncate)
#endif
#ifndef fsync
#define fsync           FS_PREFIX(fsync)
#endif
#ifndef lseek
#define lseek           FS_PREFIX(lseek)
#endif
#ifndef read
#define read            FS_PREFIX(read)
#endif
#ifndef write
#define write           FS_PREFIX(write)
#endif
#ifndef remove
#define remove          FS_PREFIX(remove)
#endif
#ifndef rename
#define rename          FS_PREFIX(rename)
#endif
#ifndef modtime
#define modtime         FS_PREFIX(modtime)
#endif
#ifndef ffilesize
#define ffilesize       FS_PREFIX(ffilesize)
#endif
#ifndef fsamefile
#define fsamefile       FS_PREFIX(fsamefile)
#endif
#ifndef file_exists
#define file_exists     FS_PREFIX(file_exists)
#endif
#ifndef relate
#define relate          FS_PREFIX(relate)
#endif
#ifndef readlink
#define readlink        FS_PREFIX(readlink)
#endif
#endif /* FILEFUNCTIONS_DEFINED */

#endif /* _FILE_H_ */
