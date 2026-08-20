/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Encode a raw disc image (.iso/.xiso) into CCI, optionally split. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cci/cci.h"

int main(int argc, char **argv)
{
    cci_writer_options opt;
    int r;

    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <in.iso> <out_base> [split_bytes] [level 1-12]\n"
                "  out_base has no extension; a single part becomes\n"
                "  <out_base>.cci, split parts <out_base>.1.cci, .2.cci, ...\n"
                "  split_bytes 0 (default) = never split; level default 12\n",
                argv[0]);
        return 2;
    }

    memset(&opt, 0, sizeof(opt));
    if (argc >= 4) {
        opt.split_point = strtoull(argv[3], NULL, 0);
    }
    if (argc >= 5) {
        opt.lz4_level = atoi(argv[4]);
    }

    r = cci_encode_file(argv[1], argv[2], &opt);
    if (r != CCI_OK) {
        fprintf(stderr, "encode failed: %s\n", cci_strerror(r));
        return 1;
    }

    printf("encoded %s -> %s(.N).cci\n", argv[1], argv[2]);
    return 0;
}
