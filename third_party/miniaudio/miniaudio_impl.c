/* miniaudio implementation TU (kept separate so the 4 MB header only compiles once,
   not into main.cpp every build). Vorbis/WAV/MP3 decoders are built in. */
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING          /* we only play back */
#define MA_NO_GENERATION
#include "miniaudio.h"
