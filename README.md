# libCCI

A small, dependency-light C library that reads and writes the **CCI** ("CCIM")
compressed container used for original-Xbox disc images. It is the reference
implementation of the format specified in [FORMAT.md](FORMAT.md); ports to other
languages (C#, Rust, …) should follow that document.

CCI stores a disc as fixed 2048-byte sectors, each either raw or a single LZ4
block, with a random-access index so any sector decodes independently. Large
images may be **split across multiple parts** (`name.1.cci`, `name.2.cci`, …),
which libCCI discovers and stitches transparently.

## Features

- Decode single-part and split (multi-part) images.
- Encode raw images to CCI (LZ4-HC), with optional splitting at a size target.
- **I/O-agnostic core** driven by read/write callbacks — usable over stdio, a
  QEMU block child, memory, etc. — plus a stdio convenience layer.
- No dependencies beyond LZ4 (bundled as a submodule, or use the system copy).

## Building

```sh
git clone --recursive https://github.com/Team-Resurgent/libCCI
cd libCCI
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

If you cloned without `--recursive`: `git submodule update --init --recursive`.
To link the system LZ4 instead of the bundled one: `-DCCI_SYSTEM_LZ4=ON`.

## CLI examples

```sh
build/cci_info    game.cci                 # show sector count / size
build/cci_extract game.cci  game.iso       # decode back to a raw image
build/cci_encode  game.iso  game           # -> game.cci
build/cci_encode  game.iso  game 4294967296 # split into game.1.cci, game.2.cci, ...
```

## Using the library

```c
#include <cci/cci.h>

cci_volume *v;
if (cci_open_file(&v, "game.cci") == CCI_OK) {          /* split-aware */
    uint8_t sector[CCI_SECTOR_SIZE];
    cci_volume_read_sector(v, lba, sector);
    /* or an arbitrary byte range: cci_volume_read(v, off, buf, len); */
    cci_volume_free(v);
}
```

For non-file backings (e.g. an emulator's block layer), open a `cci_reader`
directly with your own `cci_read_fn` and wrap parts in a `cci_volume`.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE). Bundled LZ4
(`third_party/lz4`) is BSD-2-Clause.

Format derived from Team-Resurgent's
[XboxToolkit](https://github.com/Team-Resurgent/XboxToolkit).
