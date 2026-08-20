/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libCCI - streaming encoder. Lays out each part as
 *   [32-byte header][sector blocks...][index], patching the header at the end,
 * and rolls over to a new part when split_point would be exceeded.
 * Copyright (C) 2025 Team-Resurgent
 */
#include <stdlib.h>
#include <string.h>

#include "cci_internal.h"
#include "lz4.h"
#include "lz4hc.h"

/* Highest byte offset representable by a 31-bit position shifted by alignment. */
#define CCI_MAX_POS (((uint64_t)0x7fffffffu) << CCI_INDEX_ALIGNMENT)
#define CCI_ALIGN   (1u << CCI_INDEX_ALIGNMENT) /* 4 */

struct cci_writer {
    cci_writer_options opt;
    cci_open_part_fn   open_part;
    void              *open_ctx;

    /* current part sink */
    int           part_open;
    unsigned      part_index;
    cci_pwrite_fn pwrite;
    void         *pctx;
    cci_close_fn  pclose;
    uint64_t      pos;          /* next write offset in the current part */
    uint64_t      part_sectors;

    uint32_t *entries;          /* encoded index entries for the current part */
    size_t    entries_cap;

    uint8_t  *cbuf;             /* LZ4 scratch */
    int       cbuf_cap;
};

static int ensure_entries(cci_writer *w, uint64_t need)
{
    if (need <= w->entries_cap) {
        return CCI_OK;
    }
    size_t cap = w->entries_cap ? w->entries_cap : 4096;
    uint32_t *p;
    while (cap < need) {
        cap *= 2;
    }
    p = realloc(w->entries, cap * sizeof(uint32_t));
    if (!p) {
        return CCI_ERR_NOMEM;
    }
    w->entries = p;
    w->entries_cap = cap;
    return CCI_OK;
}

static int start_part(cci_writer *w)
{
    uint8_t hdr[CCI_HEADER_SIZE];
    int ret;

    w->pwrite = NULL;
    w->pctx = NULL;
    w->pclose = NULL;
    ret = w->open_part(w->open_ctx, w->part_index, &w->pwrite, &w->pctx,
                       &w->pclose);
    if (ret != CCI_OK) {
        return ret;
    }
    if (!w->pwrite) {
        return CCI_ERR_ARG;
    }

    /* Reserve the header; the real values are patched in finish_part(). */
    memset(hdr, 0, sizeof(hdr));
    if (w->pwrite(w->pctx, 0, hdr, sizeof(hdr)) != (int64_t)sizeof(hdr)) {
        return CCI_ERR_IO;
    }
    w->pos = CCI_HEADER_SIZE;
    w->part_sectors = 0;
    w->part_open = 1;
    return CCI_OK;
}

static int finish_part(cci_writer *w)
{
    uint8_t hdr[CCI_HEADER_SIZE];
    uint64_t index_offset = w->pos;
    uint64_t nentries = w->part_sectors + 1;
    uint8_t *ibuf;
    uint64_t i;
    int ret = CCI_OK;

    if (!w->part_open) {
        return CCI_OK;
    }
    if (index_offset > CCI_MAX_POS) {
        ret = CCI_ERR_RANGE;
        goto done;
    }

    /* Sentinel entry: end offset of the last block (== index_offset). */
    ret = ensure_entries(w, nentries);
    if (ret != CCI_OK) {
        goto done;
    }
    w->entries[w->part_sectors] = (uint32_t)(index_offset >> CCI_INDEX_ALIGNMENT);

    ibuf = malloc((size_t)nentries * 4);
    if (!ibuf) {
        ret = CCI_ERR_NOMEM;
        goto done;
    }
    for (i = 0; i < nentries; i++) {
        cci_wr32(ibuf + i * 4, w->entries[i]);
    }
    if (w->pwrite(w->pctx, index_offset, ibuf, (size_t)nentries * 4) !=
        (int64_t)(nentries * 4)) {
        free(ibuf);
        ret = CCI_ERR_IO;
        goto done;
    }
    free(ibuf);

    /* Patch the header now that sizes are known. */
    memset(hdr, 0, sizeof(hdr));
    cci_wr32(hdr + 0, CCI_MAGIC);
    cci_wr32(hdr + 4, CCI_HEADER_SIZE);
    cci_wr64(hdr + 8, w->part_sectors * CCI_SECTOR_SIZE);
    cci_wr64(hdr + 16, index_offset);
    cci_wr32(hdr + 24, CCI_SECTOR_SIZE);
    hdr[28] = CCI_FORMAT_VERSION;
    hdr[29] = CCI_INDEX_ALIGNMENT;
    if (w->pwrite(w->pctx, 0, hdr, sizeof(hdr)) != (int64_t)sizeof(hdr)) {
        ret = CCI_ERR_IO;
        goto done;
    }

done:
    if (w->pclose) {
        w->pclose(w->pctx);
    }
    w->part_open = 0;
    w->part_index++;
    return ret;
}

int cci_writer_create(cci_writer **out, const cci_writer_options *opt,
                      cci_open_part_fn open_part, void *ctx)
{
    cci_writer *w;

    if (!out || !open_part) {
        return CCI_ERR_ARG;
    }
    *out = NULL;

    w = calloc(1, sizeof(*w));
    if (!w) {
        return CCI_ERR_NOMEM;
    }
    if (opt) {
        w->opt = *opt;
    }
    if (w->opt.lz4_level <= 0) {
        w->opt.lz4_level = LZ4HC_CLEVEL_MAX; /* 12 (L12_MAX) */
    }
    w->open_part = open_part;
    w->open_ctx = ctx;
    w->cbuf_cap = LZ4_compressBound(CCI_SECTOR_SIZE);
    w->cbuf = malloc((size_t)w->cbuf_cap);
    if (!w->cbuf) {
        free(w);
        return CCI_ERR_NOMEM;
    }

    *out = w;
    return CCI_OK;
}

int cci_writer_add_sector(cci_writer *w, const uint8_t sector[CCI_SECTOR_SIZE])
{
    uint8_t blk[CCI_SECTOR_SIZE];
    const uint8_t *payload;
    uint32_t block_len;
    int flag;
    int comp;
    int ret;

    if (!w || !sector) {
        return CCI_ERR_ARG;
    }

    if (!w->part_open) {
        ret = start_part(w);
        if (ret != CCI_OK) {
            return ret;
        }
    } else if (w->opt.split_point && w->part_sectors > 0) {
        /* Would the next sector push us past the target? Roll to a new part. */
        uint64_t est = w->pos + (w->part_sectors + 2) * 4 + CCI_SECTOR_SIZE;
        if (est > w->opt.split_point) {
            ret = finish_part(w);
            if (ret != CCI_OK) {
                return ret;
            }
            ret = start_part(w);
            if (ret != CCI_OK) {
                return ret;
            }
        }
    }

    comp = LZ4_compress_HC((const char *)sector, (char *)w->cbuf,
                           (int)CCI_SECTOR_SIZE, w->cbuf_cap, w->opt.lz4_level);

    if (comp > 0 && comp < (int)(CCI_SECTOR_SIZE - (4 + CCI_ALIGN))) {
        /* Framed compressed block: [pad][LZ4][zero pad], 4-byte aligned. */
        uint32_t aligned = ((uint32_t)comp + 1 + CCI_ALIGN - 1) /
                           CCI_ALIGN * CCI_ALIGN;
        uint32_t pad = aligned - ((uint32_t)comp + 1);
        blk[0] = (uint8_t)pad;
        memcpy(blk + 1, w->cbuf, (size_t)comp);
        if (pad) {
            memset(blk + 1 + comp, 0, pad);
        }
        payload = blk;
        block_len = aligned;
        flag = 1;
    } else {
        payload = sector;
        block_len = CCI_SECTOR_SIZE;
        flag = 0;
    }

    ret = ensure_entries(w, w->part_sectors + 1);
    if (ret != CCI_OK) {
        return ret;
    }
    w->entries[w->part_sectors] =
        (uint32_t)((w->pos >> CCI_INDEX_ALIGNMENT) |
                   (flag ? 0x80000000u : 0u));

    if (w->pwrite(w->pctx, w->pos, payload, block_len) != (int64_t)block_len) {
        return CCI_ERR_IO;
    }
    w->pos += block_len;
    w->part_sectors++;

    if (w->pos > CCI_MAX_POS) {
        return CCI_ERR_RANGE; /* part exceeded the addressable range */
    }
    return CCI_OK;
}

int cci_writer_finish(cci_writer *w)
{
    if (!w) {
        return CCI_ERR_ARG;
    }
    return finish_part(w);
}

void cci_writer_free(cci_writer *w)
{
    if (!w) {
        return;
    }
    if (w->part_open && w->pclose) {
        w->pclose(w->pctx); /* incomplete part; finish() was not called */
    }
    free(w->entries);
    free(w->cbuf);
    free(w);
}

/* How many parts the writer has completed (used by the file convenience). */
unsigned cci_writer_part_count(const cci_writer *w)
{
    return w ? w->part_index : 0;
}
