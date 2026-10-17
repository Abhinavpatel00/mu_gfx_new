/* scene_opaque.frag — early-Z route for scene.slang.
   SCENE_NO_DISCARD strips the alpha-test discard and the SDF-face path, so the
   driver keeps early-Z on for every group routed here. The compaction routes
   alpha-test and FACE materials to the scene.frag (late-Z) pipeline instead;
   the pair must stay in visual sync — this file is scene.slang verbatim via
   the include, so edits land where they belong. */
#define SCENE_NO_DISCARD 1
#include "scene.slang"
