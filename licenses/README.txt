Kino's licences

Kino's own code (src/, apart from src/vendor) is MIT: see LICENSE next to this folder.

The kino program in this pak is built with these libraries, linked into it:

  FFmpeg 5.1.4      LGPL 2.1+, built with --enable-gpl (libpostproc, GPL filters): GPL 2+   LICENSE-FFmpeg.md
  mpv 0.35.1        GPL 2+ (libmpv)                                                        Copyright-mpv
  libass 0.15.2     ISC                                                                    COPYING-libass
  FreeType 2.13.2   FreeType License (FTL)                                                 LICENSE-FreeType.txt, FTL-FreeType.txt
  FriBidi 1.0.13    LGPL 2.1+                                                              COPYING-fribidi
  HarfBuzz 8.3.0    "Old MIT"                                                              COPYING-harfbuzz
  cJSON             MIT                                                                    LICENSE-cJSON

Because FFmpeg (as built) and mpv are GPL, the kino program as a whole is distributed under the GNU General Public
License, version 2 or later: COPYING.GPLv2.

Portions of this software are copyright © 2023 The FreeType Project (www.freetype.org). All rights reserved.

Source: https://github.com/evolver79/nextui-kino-pak. build/fetch.sh downloads the exact library versions above
(checked by SHA-256) and build/deps.sh shows how they are configured and built, including the Brick decoders added
to FFmpeg (src/cedar/cedardec.c). The fonts in fonts/ carry their own licences (SIL Open Font License).
