/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libCCI - reference reader/writer for the CCI ("CCIM") compressed Xbox disc
 * container. See FORMAT.md for the on-disk specification.
 *
 * Copyright (C) 2025 Team-Resurgent
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 */
#ifndef CCI_H
#define CCI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CCI_VERSION_MAJOR 0
#define CCI_VERSION_MINOR 1
#define CCI_VERSION_PATCH 0

/* Format constants (see FORMAT.md). */
#define CCI_SECTOR_SIZE     2048u
#define CCI_MAGIC           0x4D494343u  /* "CCIM" */
#define CCI_HEADER_SIZE     32u
#define CCI_FORMAT_VERSION  1u
#define CCI_INDEX_ALIGNMENT 2u

/* Return codes: 0 on success, negative on error. */
enum {
    CCI_OK          =  0,
    CCI_ERR_ARG     = -1, /* invalid argument */
    CCI_ERR_IO      = -2, /* a read/write callback failed */
    CCI_ERR_FORMAT  = -3, /* not a valid CCI container */
    CCI_ERR_UNSUP   = -4, /* valid but uses a feature we don't support */
    CCI_ERR_RANGE   = -5, /* request outside the image */
    CCI_ERR_LZ4     = -6, /* LZ4 (de)compression failed */
    CCI_ERR_NOMEM   = -7, /* allocation failed */
    CCI_ERR_NOTFOUND= -8, /* a file/part could not be opened */
};

/* Human-readable message for a return code. Never NULL. */
const char *cci_strerror(int err);

/*
 * I/O callbacks. The core is I/O-agnostic: it never touches a filesystem, so it
 * works equally over stdio, a QEMU BdrvChild, memory, etc.
 */

/* Random-access read. Must fill exactly @len bytes at @offset. Returns the
 * number of bytes read (== len) on success, or a negative CCI_ERR_* on error. */
typedef int64_t (*cci_read_fn)(void *ctx, uint64_t offset, void *buf, size_t len);

/* Random-access write, used by the encoder to lay out and patch a part file.
 * Returns bytes written (== len) or negative CCI_ERR_*. */
typedef int64_t (*cci_pwrite_fn)(void *ctx, uint64_t offset, const void *buf,
                                 size_t len);

/* Optional teardown for a callback's context. */
typedef void (*cci_close_fn)(void *ctx);

/* ------------------------------------------------------------------ */
/* Reader: one CCI container (a single part) over a byte stream.        */
/* ------------------------------------------------------------------ */

typedef struct cci_reader cci_reader;

/* Parse the header + index of a container. @read/@ctx supply its bytes;
 * @close (may be NULL) is invoked on cci_reader_free. On success *out owns ctx. */
int cci_reader_open(cci_reader **out, cci_read_fn read, void *ctx,
                    cci_close_fn close);
void cci_reader_free(cci_reader *r);

uint64_t cci_reader_sector_count(const cci_reader *r);
uint64_t cci_reader_size(const cci_reader *r); /* decoded bytes */

/* Decode one logical sector into @out (exactly CCI_SECTOR_SIZE bytes). */
int cci_reader_read_sector(cci_reader *r, uint64_t lba,
                           uint8_t out[CCI_SECTOR_SIZE]);
/* Decode an arbitrary byte range. */
int cci_reader_read(cci_reader *r, uint64_t offset, void *buf, size_t len);

/* ------------------------------------------------------------------ */
/* Volume: one or more parts stitched together by sector range.         */
/* ------------------------------------------------------------------ */

typedef struct cci_volume cci_volume;

/* Build a volume from @nparts already-open readers, in order. On success the
 * volume takes ownership of the readers (and frees them on cci_volume_free). */
int cci_volume_open(cci_volume **out, cci_reader **parts, size_t nparts);
void cci_volume_free(cci_volume *v);

uint64_t cci_volume_sector_count(const cci_volume *v);
uint64_t cci_volume_size(const cci_volume *v);
int cci_volume_read_sector(cci_volume *v, uint64_t lba,
                           uint8_t out[CCI_SECTOR_SIZE]);
int cci_volume_read(cci_volume *v, uint64_t offset, void *buf, size_t len);

/* ------------------------------------------------------------------ */
/* File convenience (stdio) - handles split discovery.                  */
/* ------------------------------------------------------------------ */

/* Open a .cci by path. If @path is a numbered part (name.N.cci) all sibling
 * parts are discovered and stitched; name.cci is treated as single-part. */
int cci_open_file(cci_volume **out, const char *path);

/* Open an explicit, ordered list of part paths as one volume. */
int cci_open_files(cci_volume **out, const char *const *paths, size_t nparts);

/* ------------------------------------------------------------------ */
/* Writer: encode raw 2048-byte sectors into CCI (single or split).     */
/* ------------------------------------------------------------------ */

typedef struct {
    int      lz4_level;   /* 1..12; 0 => reference default (12 / L12_MAX) */
    uint64_t split_point; /* 0 => never split; else target max bytes/part */
} cci_writer_options;

/* Factory that opens the pwrite sink for part @part_index (0-based). Set *pwrite
 * and *pctx; *close (may be left NULL) is called when the part is finished. */
typedef int (*cci_open_part_fn)(void *ctx, unsigned part_index,
                                cci_pwrite_fn *pwrite, void **pctx,
                                cci_close_fn *close);

typedef struct cci_writer cci_writer;

/* Create a streaming writer. @open_part is called lazily as parts roll over. */
int cci_writer_create(cci_writer **out, const cci_writer_options *opt,
                      cci_open_part_fn open_part, void *ctx);
/* Append one decoded sector (exactly CCI_SECTOR_SIZE bytes). */
int cci_writer_add_sector(cci_writer *w, const uint8_t sector[CCI_SECTOR_SIZE]);
/* Flush the current part's index/header. Must be called before free. */
int cci_writer_finish(cci_writer *w);
void cci_writer_free(cci_writer *w);

/* Convenience: encode a raw disc image file into CCI. With splitting, parts are
 * written as <out_base>.1.cci, .2.cci, ...; a single part becomes <out_base>.cci
 * (<out_base> should be given without the .cci extension). */
int cci_encode_file(const char *raw_path, const char *out_base,
                    const cci_writer_options *opt);

#ifdef __cplusplus
}
#endif

#endif /* CCI_H */
