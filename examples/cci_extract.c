/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Decode a CCI image (single or split) back to a raw .iso. */
#include <stdint.h>
#include <stdio.h>

#include "cci/cci.h"

int main(int argc, char **argv)
{
    cci_volume *v = NULL;
    FILE *out;
    uint8_t sec[CCI_SECTOR_SIZE];
    uint64_t n, i;
    int r, rc = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <in.cci> <out.iso>\n", argv[0]);
        return 2;
    }

    r = cci_open_file(&v, argv[1]);
    if (r != CCI_OK) {
        fprintf(stderr, "open %s: %s\n", argv[1], cci_strerror(r));
        return 1;
    }

    out = fopen(argv[2], "wb");
    if (!out) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        cci_volume_free(v);
        return 1;
    }

    n = cci_volume_sector_count(v);
    for (i = 0; i < n; i++) {
        r = cci_volume_read_sector(v, i, sec);
        if (r != CCI_OK) {
            fprintf(stderr, "decode sector %llu: %s\n",
                    (unsigned long long)i, cci_strerror(r));
            rc = 1;
            break;
        }
        if (fwrite(sec, 1, CCI_SECTOR_SIZE, out) != CCI_SECTOR_SIZE) {
            fprintf(stderr, "write error\n");
            rc = 1;
            break;
        }
    }

    fclose(out);
    cci_volume_free(v);
    if (!rc) {
        printf("extracted %llu sectors to %s\n", (unsigned long long)n, argv[2]);
    }
    return rc;
}
