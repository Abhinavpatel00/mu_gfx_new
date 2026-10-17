/* scene_prez.frag — depth-only alpha pre-Z route for scene.slang.
   SCENE_PREZ keeps both late-Z discards (alpha-test and SDF faces) but returns
   immediately after: no lighting runs, the depth write is the entire output.
   The pre-Z pass seeds the shared depth so the opaque pass gets early-Z
   rejections behind the grass field and the alpha shade pass re-tests EQUAL
   against it. This file is scene.slang verbatim via the include, so edits land
   where they belong. 1x only: the pre-Z runs on the shared depth buffer. */
#define SCENE_PREZ 1
#include "scene.slang"
