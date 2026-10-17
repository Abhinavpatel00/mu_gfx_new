/* toon_prez.frag — depth-only alpha pre-Z route for toon.slang. Same contract
   as scene_prez.frag: SCENE_PREZ keeps the late-Z discards and returns before
   any shading. Sync by construction — the include. 1x only. */
#define SCENE_PREZ 1
#include "toon.slang"
