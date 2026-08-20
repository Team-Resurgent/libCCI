/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libCCI - single-container reader.
 * Copyright (C) 2025 Team-Resurgent
 */
#include <stdlib.h>
#include <string.h>

#include "cci_internal.h"
#include "lz4.h"

struct cci_reader {
    cci_read_fn  read;
    void        *ctx;
    cci_close_fn close;
    uint32_t    *index;         /* n_index entries, decoded to host order */
    uint32_t     n_index;       /* total_sectors + 1 */
    uint64_t     total_sectors;
};

const char *cci_strerror(int err)
{
    switch (err) {
    case CCI_OK:           return "success";
    case CCI_ERR_ARG:      return "invalid argument";
    case CCI_ERR_IO:       return "I/O callback failed";
    case CCI_ERR_FORMAT:   return "not a valid CCI container";
    case CCI_ERR_UNSUP:    return "unsupported CCI feature";
    case CCI_ERR_RANGE:    return "request outside the image";
    case CCI_ERR_LZ4:      return "LZ4 (de)compression failed";
    case CCI_ERR_NOMEM:    return "out of memory";
    case CCI_ERR_NOTFOUND: return "file or part not found";
    default:               return "unknown error";
    }
}

int cci_header_parse(const uint8_t buf[CCI_HEADER_SIZE], cci_header *h)
{
    h->magic             = cci_rd32(buf + 0);
    h->header_size       = cci_rd32(buf + 4);
    h->uncompressed_size = cci_rd64(buf + 8);
    h->index_offset      = cci_rd64(buf + 16);
    h->block_size        = cci_rd32(buf + 24);
    h->version           = buf[28];
    h->index_alignment   = buf[29];

    if (h->magic != CCI_MAGIC) {
        return CCI_ERR_FORMAT;
    }
    if (h->header_size != CCI_HEADER_SIZE) {
        return CCI_ERR_FORMAT;
    }
    if (h->block_size != CCI_SECTOR_SIZE ||
        h->version != CCI_FORMAT_VERSION ||
        h->index_alignment != CCI_INDEX_ALIGNMENT) {
        return CCI_ERR_UNSUP;
    }
    return CCI_OK;
}

int cci_reader_open(cci_reader **out, cci_read_fn read, void *ctx,
                    cci_close_fn close)
{
    cci_reader *r;
    uint8_t hdr[CCI_HEADER_SIZE];
    cci_header h;
    uint64_t sectors;
    uint32_t entries;
    int ret;
    uint32_t i;

    if (!out || !read) {
        return CCI_ERR_ARG;
    }
    *out = NULL;

    r = calloc(1, sizeof(*r));
    if (!r) {
        return CCI_ERR_NOMEM;
    }
    r->read = read;
    r->ctx = ctx;
    r->close = close;

    if (read(ctx, 0, hdr, sizeof(hdr)) != (int64_t)sizeof(hdr)) {
        ret = CCI_ERR_IO;
        goto fail;
    }

    ret = cci_header_parse(hdr, &h);
    if (ret != CCI_OK) {
        goto fail;
    }

    /* sectors + 1 index entries; guard against silly sizes. */
    if (h.uncompressed_size % CCI_SECTOR_SIZE != 0 ||
        h.uncompressed_size / CCI_SECTOR_SIZE > UINT32_MAX - 1) {
        ret = CCI_ERR_FORMAT;
        goto fail;
    }
    sectors = h.uncompressed_size / CCI_SECTOR_SIZE;
    if (sectors == 0) {
        ret = CCI_ERR_FORMAT;
        goto fail;
    }
    entries = (uint32_t)sectors + 1;

    r->index = malloc((size_t)entries * sizeof(uint32_t));
    if (!r->index) {
        ret = CCI_ERR_NOMEM;
        goto fail;
    }
    r->n_index = entries;
    r->total_sectors = sectors;

    /* Read the raw LE index and decode each entry to host order. */
    if (read(ctx, h.index_offset, r->index, (size_t)entries * 4) !=
        (int64_t)entries * 4) {
        ret = CCI_ERR_IO;
        goto fail;
    }
    for (i = 0; i < entries; i++) {
        r->index[i] = cci_rd32((const uint8_t *)&r->index[i]);
    }

    *out = r;
    return CCI_OK;

fail:
    free(r->index);
    if (close) {
        close(ctx);
    }
    free(r);
    return ret;
}

void cci_reader_free(cci_reader *r)
{
    if (!r) {
        return;
    }
    if (r->close) {
        r->close(r->ctx);
    }
    free(r->index);
    free(r);
}

uint64_t cci_reader_sector_count(const cci_reader *r)
{
    return r ? r->total_sectors : 0;
}

uint64_t cci_reader_size(const cci_reader *r)
{
    return r ? r->total_sectors * CCI_SECTOR_SIZE : 0;
}

int cci_reader_read_sector(cci_reader *r, uint64_t lba,
                           uint8_t out[CCI_SECTOR_SIZE])
{
    uint32_t e0, e1;
    uint64_t pos0, pos1;
    uint32_t span;
    int lz4;
    int64_t got;
    uint8_t blk[CCI_SECTOR_SIZE];

    if (!r || !out) {
        return CCI_ERR_ARG;
    }
    if (lba >= r->total_sectors) {
        return CCI_ERR_RANGE;
    }

    e0 = r->index[lba];
    e1 = r->index[lba + 1];
    pos0 = (uint64_t)(e0 & 0x7fffffffu) << CCI_INDEX_ALIGNMENT;
    pos1 = (uint64_t)(e1 & 0x7fffffffu) << CCI_INDEX_ALIGNMENT;
    lz4  = (e0 & 0x80000000u) != 0;

    if (pos1 < pos0 || pos1 - pos0 > CCI_SECTOR_SIZE) {
        return CCI_ERR_FORMAT;
    }
    span = (uint32_t)(pos1 - pos0);

    /* Raw sector: stored verbatim, no framing. */
    if (span == CCI_SECTOR_SIZE && !lz4) {
        got = r->read(r->ctx, pos0, out, CCI_SECTOR_SIZE);
        return got == CCI_SECTOR_SIZE ? CCI_OK : CCI_ERR_IO;
    }

    if (span < 2) {
        return CCI_ERR_FORMAT;
    }
    got = r->read(r->ctx, pos0, blk, span);
    if (got != (int64_t)span) {
        return CCI_ERR_IO;
    }

    {
        /* [pad][LZ4 block][pad zero bytes]; byte 0 is the trailing pad count. */
        unsigned pad = blk[0];
        int comp_len = (int)span - 1 - (int)pad;
        int dec;

        if (pad >= span - 1 || comp_len < 1) {
            return CCI_ERR_FORMAT;
        }
        dec = LZ4_decompress_safe((const char *)blk + 1, (char *)out,
                                  comp_len, CCI_SECTOR_SIZE);
        if (dec != (int)CCI_SECTOR_SIZE) {
            return CCI_ERR_LZ4;
        }
    }
    return CCI_OK;
}

/* Shared byte-range reader used by both the reader and the volume. */
int cci_read_range_impl(void *self, cci_sector_reader rs,
                        uint64_t total_sectors, uint64_t offset,
                        void *buf, size_t len)
{
    uint8_t *dst = buf;
    uint8_t sec[CCI_SECTOR_SIZE];
    uint64_t end, first, last, lba;

    if (!self || !rs || (!buf && len)) {
        return CCI_ERR_ARG;
    }
    if (len == 0) {
        return CCI_OK;
    }
    end = offset + len;
    if (end < offset || end > total_sectors * CCI_SECTOR_SIZE) {
        return CCI_ERR_RANGE;
    }

    first = offset / CCI_SECTOR_SIZE;
    last = (end - 1) / CCI_SECTOR_SIZE;

    for (lba = first; lba <= last; lba++) {
        uint64_t sec_start = lba * CCI_SECTOR_SIZE;
        uint64_t cstart = sec_start > offset ? sec_start : offset;
        uint64_t cend = sec_start + CCI_SECTOR_SIZE < end
                            ? sec_start + CCI_SECTOR_SIZE
                            : end;
        size_t off = (size_t)(cstart - sec_start);
        size_t n = (size_t)(cend - cstart);
        int ret = rs(self, lba, sec);
        if (ret != CCI_OK) {
            return ret;
        }
        memcpy(dst + (cstart - offset), sec + off, n);
    }
    return CCI_OK;
}

static int reader_rs(void *self, uint64_t lba, uint8_t *out)
{
    return cci_reader_read_sector((cci_reader *)self, lba, out);
}

int cci_reader_read(cci_reader *r, uint64_t offset, void *buf, size_t len)
{
    if (!r) {
        return CCI_ERR_ARG;
    }
    return cci_read_range_impl(r, reader_rs, r->total_sectors, offset, buf, len);
}
