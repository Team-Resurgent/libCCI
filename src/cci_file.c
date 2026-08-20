/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libCCI - stdio file convenience: split-aware opening and file encoding.
 * Copyright (C) 2025 Team-Resurgent
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cci_internal.h"

/* Portable 64-bit file seek. */
#if defined(_WIN32)
#  define cci_fseek64(f, off) _fseeki64((f), (long long)(off), SEEK_SET)
#  define cci_fseekend(f)     _fseeki64((f), 0, SEEK_END)
#  define cci_ftell64(f)      _ftelli64(f)
#else
#  include <sys/types.h>
#  define cci_fseek64(f, off) fseeko((f), (off_t)(off), SEEK_SET)
#  define cci_fseekend(f)     fseeko((f), 0, SEEK_END)
#  define cci_ftell64(f)      ftello(f)
#endif

static int64_t stdio_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    FILE *f = ctx;
    if (cci_fseek64(f, off) != 0) {
        return CCI_ERR_IO;
    }
    return fread(buf, 1, len, f) == len ? (int64_t)len : CCI_ERR_IO;
}

static int64_t stdio_pwrite(void *ctx, uint64_t off, const void *buf, size_t len)
{
    FILE *f = ctx;
    if (cci_fseek64(f, off) != 0) {
        return CCI_ERR_IO;
    }
    return fwrite(buf, 1, len, f) == len ? (int64_t)len : CCI_ERR_IO;
}

static void stdio_close(void *ctx)
{
    fclose((FILE *)ctx);
}

static int64_t stdio_size(FILE *f)
{
    int64_t s;
    if (cci_fseekend(f) != 0) {
        return -1;
    }
    s = (int64_t)cci_ftell64(f);
    return s;
}

int cci_open_files(cci_volume **out, const char *const *paths, size_t nparts)
{
    cci_reader **readers;
    size_t i;
    int ret = CCI_OK;

    if (!out || !paths || nparts == 0) {
        return CCI_ERR_ARG;
    }
    *out = NULL;

    readers = calloc(nparts, sizeof(*readers));
    if (!readers) {
        return CCI_ERR_NOMEM;
    }

    for (i = 0; i < nparts; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) {
            ret = CCI_ERR_NOTFOUND;
            goto fail;
        }
        /* cci_reader_open closes f (via stdio_close) if it fails. */
        ret = cci_reader_open(&readers[i], stdio_read, f, stdio_close);
        if (ret != CCI_OK) {
            readers[i] = NULL;
            goto fail;
        }
    }

    ret = cci_volume_open(out, readers, nparts); /* takes ownership on success */
    if (ret != CCI_OK) {
        goto fail;
    }
    free(readers);
    return CCI_OK;

fail:
    for (i = 0; i < nparts; i++) {
        if (readers[i]) {
            cci_reader_free(readers[i]);
        }
    }
    free(readers);
    *out = NULL;
    return ret;
}

static int ends_with_ci(const char *s, const char *suf)
{
    size_t sl = strlen(s), fl = strlen(suf);
    size_t i;
    if (sl < fl) {
        return 0;
    }
    s += sl - fl;
    for (i = 0; i < fl; i++) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)suf[i])) {
            return 0;
        }
    }
    return 1;
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

int cci_open_file(cci_volume **out, const char *path)
{
    size_t len, stem_len;
    char base[4096];
    char cand[4200];
    const char **parts = NULL;
    int nparts = 0;
    int cap = 0;
    int d;
    int ret;

    if (!out || !path) {
        return CCI_ERR_ARG;
    }
    *out = NULL;

    len = strlen(path);
    /* Single-part unless the name is "<base>.<digit>.cci". */
    if (!ends_with_ci(path, ".cci") || len >= sizeof(base)) {
        return cci_open_files(out, &path, 1);
    }
    stem_len = len - 4; /* strip ".cci" */
    /* numbered part? stem ends with ".<single digit>" */
    if (stem_len >= 2 && path[stem_len - 2] == '.' &&
        isdigit((unsigned char)path[stem_len - 1])) {
        size_t base_len = stem_len - 2;
        memcpy(base, path, base_len);
        base[base_len] = '\0';
    } else {
        return cci_open_files(out, &path, 1); /* "<base>.cci" single */
    }

    /* Gather <base>.1.cci .. <base>.9.cci contiguously from 1. */
    for (d = 1; d <= 9; d++) {
        const char **np;
        char *dup;
        snprintf(cand, sizeof(cand), "%s.%d.cci", base, d);
        if (!file_exists(cand)) {
            break;
        }
        if (nparts == cap) {
            cap = cap ? cap * 2 : 4;
            np = realloc(parts, (size_t)cap * sizeof(*parts));
            if (!np) {
                ret = CCI_ERR_NOMEM;
                goto fail;
            }
            parts = np;
        }
        dup = malloc(strlen(cand) + 1);
        if (!dup) {
            ret = CCI_ERR_NOMEM;
            goto fail;
        }
        strcpy(dup, cand);
        parts[nparts++] = dup;
    }

    if (nparts == 0) {
        /* Named like a part but none found; try the literal path. */
        free(parts);
        return cci_open_files(out, &path, 1);
    }

    ret = cci_open_files(out, parts, (size_t)nparts);

fail:
    for (d = 0; d < nparts; d++) {
        free((void *)parts[d]);
    }
    free(parts);
    return ret;
}

/* ---- Encoding ---- */

typedef struct {
    const char *out_base;
    int split;
} enc_ctx;

static int enc_open_part(void *ctx, unsigned idx, cci_pwrite_fn *pw,
                         void **pctx, cci_close_fn *close)
{
    enc_ctx *e = ctx;
    char path[4200];
    FILE *f;

    if (e->split) {
        snprintf(path, sizeof(path), "%s.%u.cci", e->out_base, idx + 1);
    } else {
        snprintf(path, sizeof(path), "%s.cci", e->out_base);
    }
    f = fopen(path, "wb+");
    if (!f) {
        return CCI_ERR_NOTFOUND;
    }
    *pw = stdio_pwrite;
    *pctx = f;
    *close = stdio_close;
    return CCI_OK;
}

int cci_encode_file(const char *raw_path, const char *out_base,
                    const cci_writer_options *opt)
{
    FILE *in;
    int64_t sz;
    cci_writer_options o;
    enc_ctx e;
    cci_writer *w = NULL;
    uint8_t sec[CCI_SECTOR_SIZE];
    uint64_t remaining;
    int ret;

    if (!raw_path || !out_base) {
        return CCI_ERR_ARG;
    }

    memset(&o, 0, sizeof(o));
    if (opt) {
        o = *opt;
    }
    e.out_base = out_base;
    e.split = o.split_point > 0;

    in = fopen(raw_path, "rb");
    if (!in) {
        return CCI_ERR_NOTFOUND;
    }
    sz = stdio_size(in);
    if (sz <= 0) {
        fclose(in);
        return CCI_ERR_FORMAT;
    }
    if (cci_fseek64(in, 0) != 0) {
        fclose(in);
        return CCI_ERR_IO;
    }

    ret = cci_writer_create(&w, &o, enc_open_part, &e);
    if (ret != CCI_OK) {
        fclose(in);
        return ret;
    }

    remaining = (uint64_t)sz;
    while (remaining > 0) {
        size_t want = remaining >= CCI_SECTOR_SIZE ? CCI_SECTOR_SIZE
                                                   : (size_t)remaining;
        memset(sec, 0, sizeof(sec)); /* zero-pad a short final sector */
        if (fread(sec, 1, want, in) != want) {
            ret = CCI_ERR_IO;
            goto out;
        }
        ret = cci_writer_add_sector(w, sec);
        if (ret != CCI_OK) {
            goto out;
        }
        remaining -= want;
    }

    ret = cci_writer_finish(w);
    if (ret != CCI_OK) {
        goto out;
    }

    /* A split-enabled encode that produced a single part becomes <base>.cci. */
    if (e.split && cci_writer_part_count(w) == 1) {
        char a[4200], b[4200];
        snprintf(a, sizeof(a), "%s.1.cci", out_base);
        snprintf(b, sizeof(b), "%s.cci", out_base);
        remove(b);
        rename(a, b);
    }

out:
    cci_writer_free(w);
    fclose(in);
    return ret;
}
