/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Print basic information about a CCI image (handles split parts). */
#include <stdio.h>

#include "cci/cci.h"

int main(int argc, char **argv)
{
    cci_volume *v = NULL;
    int r;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <image.cci>\n", argv[0]);
        return 2;
    }

    r = cci_open_file(&v, argv[1]);
    if (r != CCI_OK) {
        fprintf(stderr, "open %s: %s\n", argv[1], cci_strerror(r));
        return 1;
    }

    printf("file:            %s\n", argv[1]);
    printf("logical sectors: %llu\n",
           (unsigned long long)cci_volume_sector_count(v));
    printf("logical size:    %llu bytes\n",
           (unsigned long long)cci_volume_size(v));

    cci_volume_free(v);
    return 0;
}
