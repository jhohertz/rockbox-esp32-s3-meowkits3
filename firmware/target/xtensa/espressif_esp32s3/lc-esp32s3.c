/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 by the Rockbox ESP32-S3 port
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

/* Loadable-image loader for ESP32-S3.
 *
 * Why this file exists
 * --------------------
 * ESP32-S3 reaches external RAM through two cache windows that index the same
 * MMU entries and therefore alias the same physical pages: 0x42000000 and
 * 0x3C000000 are both entry 0. They differ in what the hardware will do
 * through them:
 *
 *     0x42... instruction window  fetch yes, load yes, store FAULTS
 *     0x3c... data window         fetch no,  load yes, store yes
 *
 * Both halves were measured on this board rather than inferred -- an image
 * entered at 0x3c... dies with InstructionFetchError on its first
 * instruction, and an image whose addresses are 0x42... dies with
 * LoadStoreError on its first store. So no single base address can host a
 * loaded image, which is the assumption firmware/lc-rock.c is built on: it
 * does one contiguous read() to hdr.load_addr and treats that address as both
 * the buffer and the entry point.
 *
 * apps/plugins/plugin.lds therefore links a codec or plugin into two segments:
 * .text/.literal/.rodata in the instruction window, and .header/.data/.bss in
 * the data window. .header is in the data window because codec_load_ram() and
 * plugin_load() write the api pointer into it after loading.
 *
 * That makes the image two disjoint address ranges, which a flat binary cannot
 * express without spanning the 0x06000000 gap between them -- objcopy produces
 * a 100 MB file of zeros trying. The ELF program headers describe both segments
 * exactly, including the bss extent where p_memsz exceeds p_filesz, so the
 * image is shipped as a stripped ELF (PLUGIN_USE_ELF="yes" in tools/configure)
 * and this file places each segment.
 *
 * The one rule that makes it work: because the windows alias, a segment linked
 * at 0x42... is *written* through its data-window address (p_vaddr minus
 * XT_IROM_OFF) and *executed* through the address it was linked at. They are
 * the same bytes. That is also why the codec's own literal pools are correct
 * without any fixup here: they were linked with code addresses in the
 * instruction window and data addresses in the data window, which is exactly
 * what the hardware serves for each.
 *
 * Only lc_open() is replaced. firmware/export/lc-rock.h still supplies
 * lc_get_header(), lc_close() and lc_open_from_mem() as inline functions, and
 * lc_get_header() returns the handle unchanged, so the handle must be the
 * address of the image header -- which is the base of the data-window segment,
 * i.e. the buffer the caller passed in.
 */

#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "debug.h"
#include "load_code.h"
#include "stdio.h"
#include <string.h>
#include "xtensa.h"      /* XT_* layout constants, via config.h */

/* ELF32, little endian, as emitted by the xtensa toolchain. Parsed by hand
 * rather than through <elf.h> so that this does not depend on a hosted header
 * that a -nostdlib build may or may not provide. */
#define XT_ELF_NIDENT   16
#define XT_EI_CLASS     4
#define XT_ELFCLASS32   1
#define XT_EI_DATA      5
#define XT_ELFDATA2LSB  1
#define XT_ET_EXEC      2
#define XT_PT_LOAD      1
/* Enough of the ELF header to reach e_phoff (offset 28) and e_phnum (offset
 * 44, two bytes), without committing to the whole structure. */
#define XT_EHDR_READ_SIZE 46
#define XT_PHDR_SIZE      32

struct xt_phdr
{
    unsigned int type;
    unsigned int offset;
    unsigned int vaddr;
    unsigned int paddr;
    unsigned int filesz;
    unsigned int memsz;
    unsigned int flags;
    unsigned int align;
};

/* Everything a loaded image may be written to, in data-window addresses: the
 * two text halves below codecbuf plus the codec and plugin buffers themselves.
 * Bounding against the fixed bases rather than against the caller's buf is
 * what lets a plugin load reach its text half, which sits below the codec
 * region. */
#define XT_LOAD_FLOOR \
    ((unsigned char *)XT_PLUGIN_TEXT_ADDR)
#define XT_LOAD_CEIL \
    ((unsigned char *)(XT_PLUGINBUF_ADDR + PLUGIN_BUFFER_SIZE))

/* Little-endian, which is all this core produces. */
static unsigned int get_le32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

/* Read a program header. The file is read a whole header at a time; there are
 * only a handful and this runs once per codec load. */
static int read_phdr(int fd, unsigned int phoff, unsigned int n,
                     struct xt_phdr *ph)
{
    unsigned char buf[XT_PHDR_SIZE];

    if (lseek(fd, phoff + n * XT_PHDR_SIZE, SEEK_SET) < 0)
        return 0;
    if (read(fd, buf, sizeof buf) != (ssize_t)sizeof buf)
        return 0;

    ph->type   = get_le32(buf + 0);
    ph->offset = get_le32(buf + 4);
    ph->vaddr  = get_le32(buf + 8);
    ph->paddr  = get_le32(buf + 12);
    ph->filesz = get_le32(buf + 16);
    ph->memsz  = get_le32(buf + 20);
    ph->flags  = get_le32(buf + 24);
    ph->align  = get_le32(buf + 28);
    return 1;
}

/* Stores only work through the data window, so a segment linked at 0x42... is
 * written at the address 0x3c... that aliases the same physical page. */
static unsigned char *write_target(unsigned int vaddr)
{
    if (vaddr >= XT_IROM_BASE)
        return (unsigned char *)(vaddr - XT_IROM_OFF);
    return (unsigned char *)vaddr;
}

void *lc_open(const char *filename, unsigned char *buf, size_t buf_size)
{
    unsigned char ident[XT_ELF_NIDENT];
    unsigned char ehdr[XT_EHDR_READ_SIZE];
    struct xt_phdr ph;
    unsigned int phoff, phnum;
    unsigned char *data_hi = NULL;   /* top of the data-window image */
    unsigned char *buf_end = buf + buf_size;
    int fd, i, loaded = 0;

    fd = open(filename, O_RDONLY);
    if (fd < 0)
    {
        printf("load fail: could not open %s\n", filename);
        return NULL;
    }

    if (read(fd, ident, sizeof ident) != (ssize_t)sizeof ident
        || ident[0] != 0x7f || ident[1] != 'E' || ident[2] != 'L'
        || ident[3] != 'F'
        || ident[XT_EI_CLASS] != XT_ELFCLASS32
        || ident[XT_EI_DATA] != XT_ELFDATA2LSB)
    {
        printf("load fail: Not a 32-bit little-endian ELF\n");
        goto err;
    }

    if (lseek(fd, 0, SEEK_SET) < 0
        || read(fd, ehdr, sizeof ehdr) != (ssize_t)sizeof ehdr)
    {
        printf("load fail: Could not read ELF header\n");
        goto err;
    }

    phoff = get_le32(ehdr + 28);
    phnum = (unsigned int)ehdr[44] | ((unsigned int)ehdr[45] << 8);

    if (phoff == 0 || phnum == 0 || phnum > 16)
    {
        printf("load fail: Bogus program header table\n");
        goto err;
    }

    for (i = 0; i < (int)phnum; i++)
    {
        unsigned char *dest;
        ssize_t n;

        if (!read_phdr(fd, phoff, (unsigned int)i, &ph))
        {
            printf("load fail: Could not read program header %d\n", i);
            goto err;
        }
        if (ph.type != XT_PT_LOAD || ph.memsz == 0)
            continue;

        dest = write_target(ph.vaddr);

        /* The image must land in the external-RAM load region. Checking
         * against the fixed region rather than against buf is what allows the
         * text segment, which lives below the buffer the caller named. */
        if (dest < XT_LOAD_FLOOR || dest >= XT_LOAD_CEIL
            || dest + ph.memsz > XT_LOAD_CEIL)
        {
            printf("load fail: Segment %d at %p does not fit the load region\n", i,
                   (void *)dest);
            goto err;
        }

        if (ph.filesz > 0)
        {
            if (lseek(fd, ph.offset, SEEK_SET) < 0)
            {
                printf("load fail: Could not seek to segment %d\n", i);
                goto err;
            }
            n = read(fd, dest, ph.filesz);
            if (n != (ssize_t)ph.filesz)
            {
                printf("load fail: Short read on segment %d (%ld of %u)\n", i,
                       (long)n, ph.filesz);
                goto err;
            }
        }

        /* p_memsz beyond p_filesz is .bss: NOLOAD, absent from the file. The
         * codec zeroes its own bss in codec_crt0.c, but that is a courtesy,
         * not a contract, and a stale byte here is a wild function pointer. */
        if (ph.memsz > ph.filesz)
            memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);

        dbg_breadcrumb("seg", ph.vaddr, (unsigned long)dest);
        printf("codec load: seg %d vaddr=%08x -> write@%p filesz=%u memsz=%u\n",
               i, ph.vaddr, (void *)dest, ph.filesz, ph.memsz);

        /* Track the top of the data-window image so the heap above it can be
         * cleared below. The text segment is written through its data-window
         * alias, which sits below buf, so only the data segment bounds the
         * heap. */
        if (ph.vaddr < XT_IROM_BASE)
        {
            unsigned char *end = dest + ph.memsz;

            if (data_hi == NULL || end > data_hi)
                data_hi = end;
        }
        loaded++;
    }

    close(fd);

    if (loaded == 0)
    {
        printf("load fail: No loadable segments\n");
        return NULL;
    }

    /* Clear the heap the codec will be handed, above the loaded image.
     *
     * This is not hygiene, it is correctness. codecbuf is a pinned absolute
     * address declared with .set, so it belongs to no section and nothing ever
     * zeroes it -- unlike .ext_ram.bss, which IDF's startup clears. Rockbox's
     * codec allocator (lib/tlsf) keeps its pool state *in that very memory*, and
     * init_memory_pool() short-circuits when it finds its own signature there:
     *
     *     if (tlsf->tlsf_signature == TLSF_SIGNATURE) { mp = mem_pool;
     *         return b->size & BLOCK_SIZE; }    -- returns without re-initialising
     *
     * The signature is removed only by ogg_malloc_destroy(), which runs on a
     * clean CODEC_UNLOAD. A codec that crashes never reaches it, so the next
     * load inherits a pool laid out for a different codec at a different size,
     * tlsf_malloc returns NULL, and the codec longjmps out of its own
     * allocation failure. The fault therefore reproduces itself: one crash
     * poisons the buffer for every load afterwards -- which is exactly what
     * "consistent crash" looked like, and why it seemed unrelated to anything
     * changing.
     *
     * A freshly loaded image must never inherit allocator state from a
     * previous, possibly crashed, image. */
    if (data_hi != NULL && data_hi < buf_end)
    {
        memset(data_hi, 0, buf_end - data_hi);
        printf("codec load: cleared heap %p..%p (%u bytes)\n", (void *)data_hi,
               (void *)buf_end, (unsigned)(buf_end - data_hi));
    }

    /* The instruction cache may hold lines for pages that only just became
     * code. */
    commit_discard_idcache();

    /* The last thing recorded before the caller jumps into freshly placed
     * code. If the next boot's trail ends here, the fault is executing the
     * image, not loading it -- which is the distinction the two WAV-resume
     * crashes needed and could not make, because IDF's panic handler
     * re-entered before printing anything at all. */
    dbg_breadcrumb("placed", (unsigned long)buf, (unsigned long)loaded);
    printf("codec load: %d segment(s) placed, handle=%p\n", loaded,
           (void *)buf);

    /* lc_get_header() returns the handle unchanged and apps/codecs.c reads
     * hdr->load_addr from it, so the handle is the image header -- which is at
     * the base of the data-window segment, the buffer we were handed. */
    return buf;

err:
    close(fd);
    return NULL;
}
