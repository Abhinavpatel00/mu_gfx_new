#!/usr/bin/env bash
# Cooks the Crypt Run asset set out of the KayKit packs in data/threedassets.
#
# The cooker takes one model per invocation and writes one .mua, so this is a
# list, not a directory scan: the game loads a fixed roster and its scene
# capacities are sized from it before anything is uploaded. Re-run after adding
# a line; already-cooked files are skipped so a partial run is cheap to resume.
set -euo pipefail

cd "$(dirname "$0")/.."
COOK=build/muasset_cook
DUN=data/threedassets/KayKit_Dungeon_Pack_1.1_FREE/KayKit_Dungeon_Pack_1.1_FREE/Assets/gltf
SKEL=data/threedassets/KayKit_Skeletons_1.1_FREE/KayKit_Skeletons_1.1_FREE/assets/gltf
ADV=data/threedassets/KayKit_Adventurers_2.0_FREE/KayKit_Adventurers_2.0_FREE/Assets/gltf

# name:source-dir:model:lods
ASSETS=(
  # --- shell: floors, walls, openings ------------------------------------
  floor_tile_small:$DUN:floor_tile_small:1
  floor_dirt_large:$DUN:floor_dirt_large:1
  # Barriers are chest high, which is what lets a raised third-person camera
  # see over the shell instead of into it.
  wall:$DUN:barrier_half:1
  wall_corner:$DUN:barrier_corner:1
  wall_column:$DUN:barrier_colum_half:1
  pillar:$DUN:pillar:1
  stairs_wide:$DUN:stairs_wide:1
  # --- light and dressing -------------------------------------------------
  torch_lit:$DUN:torch_lit:2
  torch_wall:$DUN:torch_mounted:2
  candle_lit:$DUN:candle_lit:2
  candle_holder:$DUN:candle:2
  barrel_large:$DUN:barrel_large:2
  crates_stacked:$DUN:crates_stacked:2
  chest:$DUN:chest:2
  chest_gold:$DUN:chest_gold:2
  rubble_large:$DUN:rubble_large:2
  table_long:$DUN:table_long:2
  shelf_large:$DUN:shelf_large:2
  banner_red:$DUN:banner_red:2
  coin_stack:$DUN:coin_stack_large:2
  key_ring:$DUN:keyring:2
  # --- the dead -----------------------------------------------------------
  skel_staff:$SKEL:Skeleton_Staff:3
  skel_axe:$SKEL:Skeleton_Axe:3
  skel_crossbow:$SKEL:Skeleton_Crossbow:3
  skel_shield_a:$SKEL:Skeleton_Shield_Large_A:3
  skel_shield_b:$SKEL:Skeleton_Shield_Large_B:3
  skel_quiver:$SKEL:Skeleton_Quiver:3
  # --- player gear --------------------------------------------------------
  sword_1h:$ADV:sword_1handed:2
  sword_2h:$ADV:sword_2handed:2
  axe_1h:$ADV:axe_1handed:2
  wand:$ADV:wand:2
  staff:$ADV:staff:2
  shield_round:$ADV:shield_round:2
)

need_cook=0
for a in "${ASSETS[@]}"; do
  name=${a%%:*}
  [[ -f "assets/$name.mua" ]] || need_cook=1
done
if (( need_cook == 0 )); then
  echo "all ${#ASSETS[@]} assets cooked"
  exit 0
fi

for a in "${ASSETS[@]}"; do
  IFS=: read -r name dir model lods <<<"$a"
  if [[ -f "assets/$name.mua" ]]; then
    echo "  skip  $name"
    continue
  fi
  echo "  cook  $name"
  "$COOK" "$dir/$model.gltf" -o "assets/$name.mua" --lods "$lods" >/dev/null
done
echo "cooked ${#ASSETS[@]} assets into assets/"