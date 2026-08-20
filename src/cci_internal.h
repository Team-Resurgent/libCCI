/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libCCI internal helpers (not a public API).
 * Copyright (C) 2025 Team-Resurgent
 */
#ifndef CCI_INTERNAL_H
#define CCI_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include "cci/cci.h"

/* Little-endian accessors (portable, no host-endianness assumptions). */
static inline uint32_t cci_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t cci_rd64(const uint8_t *p)
{
    return (uint64_t)cci_rd32(p) | ((uint64_t)cci_rd32(p + 4) << 32);
}
static inline void cci_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void cci_wr64(uint8_t *p, uint64_t v)
{
    cci_wr32(p, (uint32_t)v);
    cci_wr32(p + 4, (uint32_t)(v >> 32));
}

/* Parsed 32-byte header. */
typedef struct {
    uint32_t magic;
    uint32_t header_size;
    uint64_t uncompressed_size;
    uint64_t index_offset;
    uint32_t block_size;
    uint8_t  version;
    uint8_t  index_alignment;
} cci_header;

/* Parse and validate the 32 header bytes. Returns CCI_OK or CCI_ERR_*. */
int cci_header_parse(const uint8_t buf[CCI_HEADER_SIZE], cci_header *h);

/*
 * Generic "decode an arbitrary byte range" over any per-sector decoder.
 * @rs decodes one full CCI_SECTOR_SIZE sector of @self into its out buffer.
 */
typedef int (*cci_sector_reader)(void *self, uint64_t lba, uint8_t *out);
int cci_read_range_impl(void *self, cci_sector_reader rs,
                        uint64_t total_sectors, uint64_t offset,
                        void *buf, size_t len);

/* Number of parts the writer has completed (internal; used by the file API). */
unsigned cci_writer_part_count(const cci_writer *w);

#endif /* CCI_INTERNAL_H */
