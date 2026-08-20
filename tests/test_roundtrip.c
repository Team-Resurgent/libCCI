/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * libCCI round-trip test: encode a synthetic raw image (mixing compressible and
 * incompressible sectors), then decode it back through both a single-part and a
 * split (multi-part) container and verify byte-for-byte equality, including
 * arbitrary sub-sector read ranges.
 * Copyright (C) 2025 Team-Resurgent
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cci/cci.h"

#define N_SECTORS 40u
#define RAW_BYTES (N_SECTORS * CCI_SECTOR_SIZE)

static int fails;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);\
            fails++;                                                         \
        }                                                                    \
    } while (0)

/* Deterministic pseudo-random for the "incompressible" sectors. */
static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1103515245u + 12345u;
    return *s;
}

static void build_raw(uint8_t *raw)
{
    uint32_t s = 0x1234abcdu;
    unsigned i, j;
    for (i = 0; i < N_SECTORS; i++) {
        uint8_t *sec = raw + i * CCI_SECTOR_SIZE;
        if (i % 3 == 0) {
            memset(sec, 0, CCI_SECTOR_SIZE);               /* all zero */
        } else if (i % 3 == 1) {
            memset(sec, (int)(i * 7 + 1), CCI_SECTOR_SIZE); /* constant */
        } else {
            for (j = 0; j < CCI_SECTOR_SIZE; j++) {
                sec[j] = (uint8_t)(lcg(&s) >> 17);          /* incompressible */
            }
        }
    }
}

static int write_file(const char *path, const uint8_t *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    if (fwrite(buf, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static void verify_volume(const char *first_path, const uint8_t *raw)
{
    cci_volume *v = NULL;
    uint8_t *decoded;
    int ret;
    unsigned i;

    ret = cci_open_file(&v, first_path);
    CHECK(ret == CCI_OK, "cci_open_file");
    if (ret != CCI_OK) {
        return;
    }

    CHECK(cci_volume_sector_count(v) == N_SECTORS, "sector count");
    CHECK(cci_volume_size(v) == RAW_BYTES, "size");

    decoded = malloc(RAW_BYTES);
    CHECK(decoded != NULL, "alloc decoded");
    if (decoded) {
        /* whole-image read */
        ret = cci_volume_read(v, 0, decoded, RAW_BYTES);
        CHECK(ret == CCI_OK, "volume_read whole");
        CHECK(memcmp(decoded, raw, RAW_BYTES) == 0, "whole image matches");

        /* per-sector read */
        for (i = 0; i < N_SECTORS; i++) {
            uint8_t sec[CCI_SECTOR_SIZE];
            ret = cci_volume_read_sector(v, i, sec);
            CHECK(ret == CCI_OK, "read_sector");
            CHECK(memcmp(sec, raw + i * CCI_SECTOR_SIZE, CCI_SECTOR_SIZE) == 0,
                  "sector matches");
        }

        /* arbitrary sub-sector ranges crossing boundaries */
        {
            uint64_t offs[] = { 1, 2047, 2048, 4095, 5000, RAW_BYTES - 3 };
            size_t lens[] = { 100, 4, 4096, 2, 3000, 3 };
            size_t k;
            for (k = 0; k < sizeof(offs) / sizeof(offs[0]); k++) {
                uint8_t tmp[8192];
                ret = cci_volume_read(v, offs[k], tmp, lens[k]);
                CHECK(ret == CCI_OK, "volume_read range");
                CHECK(memcmp(tmp, raw + offs[k], lens[k]) == 0, "range matches");
            }
        }

        /* out-of-range read must be rejected */
        {
            uint8_t tmp[16];
            ret = cci_volume_read(v, RAW_BYTES, tmp, 1);
            CHECK(ret == CCI_ERR_RANGE, "past-EOF rejected");
        }
        free(decoded);
    }
    cci_volume_free(v);
}

int main(void)
{
    uint8_t *raw = malloc(RAW_BYTES);
    cci_writer_options opt;

    if (!raw) {
        fprintf(stderr, "alloc raw failed\n");
        return 1;
    }
    build_raw(raw);
    if (write_file("rt_input.iso", raw, RAW_BYTES) != 0) {
        fprintf(stderr, "could not write rt_input.iso\n");
        return 1;
    }

    /* Single-part encode. */
    memset(&opt, 0, sizeof(opt));
    CHECK(cci_encode_file("rt_input.iso", "rt_single", &opt) == CCI_OK,
          "encode single");
    verify_volume("rt_single.cci", raw);

    /* Split encode: small target to force several parts. */
    memset(&opt, 0, sizeof(opt));
    opt.split_point = 24000; /* ~11 sectors/part */
    CHECK(cci_encode_file("rt_input.iso", "rt_split", &opt) == CCI_OK,
          "encode split");
    {
        FILE *p1 = fopen("rt_split.1.cci", "rb");
        FILE *p2 = fopen("rt_split.2.cci", "rb");
        CHECK(p1 != NULL, "split produced .1.cci");
        CHECK(p2 != NULL, "split produced .2.cci");
        if (p1) fclose(p1);
        if (p2) fclose(p2);
    }
    verify_volume("rt_split.1.cci", raw);

    free(raw);

    /* Cleanup (best effort). */
    remove("rt_input.iso");
    remove("rt_single.cci");
    { int d; char p[64];
      for (d = 1; d <= 9; d++) { snprintf(p, sizeof(p), "rt_split.%d.cci", d);
                                 remove(p); }
      remove("rt_split.cci"); }

    if (fails) {
        fprintf(stderr, "\n%d check(s) failed\n", fails);
        return 1;
    }
    printf("all round-trip checks passed\n");
    return 0;
}
