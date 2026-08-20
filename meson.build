# SPDX-License-Identifier: GPL-2.0-or-later
# Minimal Meson build so libCCI can be embedded as a subproject (e.g. by xemu).
# The full-featured build (examples, tests, install) lives in CMakeLists.txt.
project('libcci', 'c',
        version: '0.1.0',
        license: 'GPL-2.0-or-later',
        default_options: ['c_std=c11'])

fs = import('fs')

lz4_lib_dir = 'third_party/lz4/lib'
if not fs.exists(lz4_lib_dir / 'lz4.c')
  error('Bundled LZ4 not found. Clone recursively (git submodule update ' +
        '--init --recursive) or embed with clone-recursive = true.')
endif

libcci_public_inc = include_directories('include')

cci_lib = static_library('cci',
  files(
    'src/cci_reader.c',
    'src/cci_volume.c',
    'src/cci_file.c',
    'src/cci_writer.c',
    lz4_lib_dir / 'lz4.c',
    lz4_lib_dir / 'lz4hc.c',
  ),
  include_directories: [
    libcci_public_inc,
    include_directories(lz4_lib_dir),
  ])

libcci_dep = declare_dependency(
  link_with: cci_lib,
  include_directories: libcci_public_inc)
