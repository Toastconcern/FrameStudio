/* stb_vorbis implementation TU — decodes Ogg Vorbis (the env's _BACKGROUND_LOOP.ogg).
   miniaudio ships WAV/FLAC/MP3 but NOT Vorbis, so we decode the whole ogg to PCM here and
   feed raw frames to miniaudio for looping playback. */
#define STB_VORBIS_NO_STDIO        /* decode from memory only */
#include "stb_vorbis.c"
