#!/usr/bin/env bash
# Cooks the outdoor meadow test scene out of the nature packs in data/threedassets.
#
# Same shape as cook_game_assets.sh: a fixed roster, one .mua per model, so the
# scene capacities stay a constant instead of a directory count. Re-run after
# adding a line; already-cooked files are skipped, so a partial run resumes.
set -euo pipefail

cd "$(dirname "$0")/.."
COOK=build/muasset_cook
FORCE_RECOOK=${FORCE_RECOOK:-0}

# Stylized Nature and the Medieval Village path contain spaces and brackets, so
# every roster element below is a single quoted word split on ':' alone.
SN="data/threedassets/Stylized Nature MegaKit[Standard]/glTF"
KF="data/threedassets/KayKit_Forest_Nature_Pack_1.0_FREE/KayKit_Forest_Nature_Pack_1.0_FREE/Assets/gltf"
DUN="data/threedassets/KayKit_Dungeon_Pack_1.1_FREE/KayKit_Dungeon_Pack_1.1_FREE/Assets/gltf"

# name:source-dir:model:lods
ASSETS=(
  # --- ground: dirt tiles, tiled into a clearing ---------------------------
  "md_ground:$DUN:floor_dirt_large:1"
  "md_ground_rocky:$DUN:floor_dirt_large_rocky:1"

  # --- hero trees (Stylized Nature, 2-4k bark, so sparing) -----------------
  "md_tree_common_a:$SN:CommonTree_1:2"
  "md_tree_common_b:$SN:CommonTree_3:2"
  "md_tree_common_c:$SN:CommonTree_5:2"
  "md_pine_a:$SN:Pine_1:2"
  "md_pine_b:$SN:Pine_3:2"
  "md_tree_twisted_a:$SN:TwistedTree_1:2"
  "md_tree_twisted_b:$SN:TwistedTree_3:2"

  # --- filler trees (KayKit Forest: one shared 1k atlas, cheap to place) ---
  "md_tree_birch_a:$KF:Tree_1_A_Color1:2"
  "md_tree_birch_b:$KF:Tree_2_A_Color1:2"
  "md_tree_oak_a:$KF:Tree_3_A_Color1:2"
  "md_tree_oak_b:$KF:Tree_4_A_Color1:2"
  "md_tree_bare_a:$KF:Tree_Bare_1_A_Color1:2"

  # --- bushes --------------------------------------------------------------
  "md_bush_a:$SN:Bush_Common:1"
  "md_bush_flowers:$SN:Bush_Common_Flowers:1"
  "md_bush_b:$KF:Bush_1_A_Color1:1"
  "md_bush_c:$KF:Bush_2_A_Color1:1"

  # --- grass tufts ---------------------------------------------------------
  "md_grass_tall:$SN:Grass_Common_Tall:1"
  "md_grass_wispy:$SN:Grass_Wispy_Tall:1"
  "md_grass_b:$KF:Grass_1_A_Singlesided_Color1:1"
  "md_grass_c:$KF:Grass_2_A_Singlesided_Color1:1"

  # --- rocks and pebbles ---------------------------------------------------
  "md_rock_a:$SN:Rock_Medium_1:1"
  "md_rock_b:$SN:Rock_Medium_2:1"
  "md_rock_c:$KF:Rock_1_A_Color1:1"
  "md_pebble_a:$SN:Pebble_Round_1:1"
  "md_rockpath:$SN:RockPath_Round_Wide:1"

  # --- small dressing ------------------------------------------------------
  "md_mushroom:$SN:Mushroom_Common:1"
  "md_fern:$SN:Fern_1:1"
  "md_flower_group:$SN:Flower_3_Group:1"
  "md_clover:$SN:Clover_1:1"
)

need_cook=0
for a in "${ASSETS[@]}"; do
  name=${a%%:*}
  [[ "$FORCE_RECOOK" == 1 || ! -f "assets/$name.mua" ]] || continue
  need_cook=1
done
if (( need_cook == 0 )); then
  echo "all ${#ASSETS[@]} meadow assets cooked"
  exit 0
fi

for a in "${ASSETS[@]}"; do
  IFS=: read -r name dir model lods <<<"$a"
  if [[ "$FORCE_RECOOK" != 1 && -f "assets/$name.mua" ]]; then
    echo "  skip  $name"
    continue
  fi
  echo "  cook  $name"
  "$COOK" "$dir/$model.gltf" -o "assets/$name.mua" --lods "$lods" >/dev/null
done
echo "cooked ${#ASSETS[@]} assets into assets/"
