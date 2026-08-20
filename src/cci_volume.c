/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libCCI - multi-part volume. Stitches N self-contained CCI parts together at
 * sector granularity (see FORMAT.md section 7).
 * Copyright (C) 2025 Team-Resurgent
 */
#include <stdlib.h>

#include "cci_internal.h"

struct cci_volume {
    cci_reader **parts;
    uint64_t    *start;   /* start[i] = first logical sector of part i */
    size_t       nparts;
    uint64_t     total_sectors;
};

int cci_volume_open(cci_volume **out, cci_reader **parts, size_t nparts)
{
    cci_volume *v;
    uint64_t acc = 0;
    size_t i;

    if (!out || !parts || nparts == 0) {
        return CCI_ERR_ARG;
    }
    *out = NULL;

    v = calloc(1, sizeof(*v));
    if (!v) {
        return CCI_ERR_NOMEM;
    }
    v->start = malloc(nparts * sizeof(uint64_t));
    v->parts = malloc(nparts * sizeof(cci_reader *));
    if (!v->start || !v->parts) {
        free(v->start);
        free(v->parts);
        free(v);
        return CCI_ERR_NOMEM;
    }

    for (i = 0; i < nparts; i++) {
        if (!parts[i]) {
            free(v->start);
            free(v->parts);
            free(v);
            return CCI_ERR_ARG;
        }
        v->parts[i] = parts[i];
        v->start[i] = acc;
        acc += cci_reader_sector_count(parts[i]);
    }
    v->nparts = nparts;
    v->total_sectors = acc;

    *out = v;
    return CCI_OK;
}

void cci_volume_free(cci_volume *v)
{
    size_t i;

    if (!v) {
        return;
    }
    for (i = 0; i < v->nparts; i++) {
        cci_reader_free(v->parts[i]);
    }
    free(v->parts);
    free(v->start);
    free(v);
}

uint64_t cci_volume_sector_count(const cci_volume *v)
{
    return v ? v->total_sectors : 0;
}

uint64_t cci_volume_size(const cci_volume *v)
{
    return v ? v->total_sectors * CCI_SECTOR_SIZE : 0;
}

int cci_volume_read_sector(cci_volume *v, uint64_t lba,
                           uint8_t out[CCI_SECTOR_SIZE])
{
    size_t lo, hi;

    if (!v || !out) {
        return CCI_ERR_ARG;
    }
    if (lba >= v->total_sectors) {
        return CCI_ERR_RANGE;
    }

    /* Binary search for the part whose range contains lba. */
    lo = 0;
    hi = v->nparts - 1;
    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1) / 2;
        if (v->start[mid] <= lba) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return cci_reader_read_sector(v->parts[lo], lba - v->start[lo], out);
}

static int volume_rs(void *self, uint64_t lba, uint8_t *out)
{
    return cci_volume_read_sector((cci_volume *)self, lba, out);
}

int cci_volume_read(cci_volume *v, uint64_t offset, void *buf, size_t len)
{
    if (!v) {
        return CCI_ERR_ARG;
    }
    return cci_read_range_impl(v, volume_rs, v->total_sectors, offset, buf, len);
}
