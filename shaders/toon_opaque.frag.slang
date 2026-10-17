/* toon_opaque.frag — early-Z route for toon.slang. Same contract as
   scene_opaque.frag: SCENE_NO_DISCARD strips the alpha-test discard and the
   SDF-face path; the compaction routes those groups to the toon.frag
   (late-Z) pipeline. Sync is by construction — the include. */
#define SCENE_NO_DISCARD 1
#include "toon.slang"
