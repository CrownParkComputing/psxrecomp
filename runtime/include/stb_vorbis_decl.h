/* Declarations-only view of stb_vorbis (the implementation lives in
 * runtime/src/stb_vorbis_impl.c). */
#ifndef PSXRECOMP_STB_VORBIS_DECL_H
#define PSXRECOMP_STB_VORBIS_DECL_H
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#include "../../third_party/stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY
#endif
