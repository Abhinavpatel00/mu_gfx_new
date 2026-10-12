#ifndef CRYPT_RUN_H
#define CRYPT_RUN_H

/* Crypt Run — a third-person dungeon delve.
 *
 * The frame is a fixed-step simulation over tables plus a render-rate sync:
 *
 *   game_input      platform state -> intent (move axis, aim, actions)
 *   game_step       fixed 60 Hz: flow field, player, enemies, combat, waves
 *   game_sync       tables -> scene instances, once per rendered frame
 *
 * Nothing allocates inside a step or a frame. Spawns and deaths queue and drain
 * at a fixed point, so no system mutates the set it is walking. State is
 * existence: an enemy is alive because it is in `alive`, dying because it is in
 * `dying`; there is no per-entity flag that is usually false.
 */

#include "../../renderer.h"
#include "../../src/input.h"
#include "../../src/three_d/scene.h"

#include <cglm/cglm.h>

#include "../../external/mu/mu.h"
#include "../../external/mu/mu_roguelike.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct Game Game;

/* ------------------------------------------------------------------ level -- */

#define GRID_W 34
#define GRID_H 34
#define CELL 2.0f /* metres per cell; barrier_half is a 2 m wall segment */
#define GRID_X0 (-(float)GRID_W * CELL * 0.5f)
#define GRID_Z0 (-(float)GRID_H * CELL * 0.5f)

#define MAX_ROOMS 24
#define MAX_ENEMIES 320
#define MAX_PICKUPS 96
#define MAX_DUNGEON_INSTANCES 6000u
#define CHARACTER_COUNT 6u
#define MAX_ASSET_MESHES 16u

/* A cell is either carved or rock; which wall pieces get planted and where the
   torches go is derived from that at build time, never stored. */
#define CELL_FLOOR 0x1u
#define CELL_ROOM 0x2u  /* carved as a room, not a corridor */
#define CELL_DOOR 0x4u  /* a room opening onto a corridor */
#define CELL_EXIT 0x8u

typedef struct Room {
    int x, y, w, h;
} Room;

extern uint8_t   crypt_cell[GRID_W * GRID_H];
extern Room      crypt_room[MAX_ROOMS];
extern uint32_t  crypt_room_count;

int   crypt_cell_index(int x, int y);
bool  crypt_cell_inside(int x, int y);
bool  crypt_walkable(int x, int y);
float crypt_cell_x(int x);
float crypt_cell_z(int y);
int   crypt_world_x(float wx);
int   crypt_world_z(float wz);

/* level.c */
void crypt_level_generate(uint32_t seed, uint32_t depth);
void crypt_level_choose_ends(Game *g);
void crypt_level_build(Game *g);
void crypt_flow_rebuild(Game *g);

/* ------------------------------------------------------------------- rig -- */

typedef enum Joint {
    J_BODY, J_HEAD, J_ARM_L, J_ARM_R, J_LEG_L, J_LEG_R, J_COUNT
} Joint;

typedef struct RiggedModel {
    uint32_t first_mesh;
    uint32_t mesh_count;
    uint32_t part_count;
    struct {
        uint32_t mesh;
        uint32_t joint;
        uint8_t  rotate;
    } parts[MAX_ASSET_MESHES];
} RiggedModel;

extern RiggedModel crypt_character[CHARACTER_COUNT];
extern const char   *crypt_character_path[CHARACTER_COUNT];
extern const vec3   crypt_joint_pivot[J_COUNT];

/* rig.c */
bool crypt_rig_build(Game *g, uint32_t character);
/* Fills pos/rotation for every part of one rig instance from a root transform and a
   joint-angle set. `rot` is indexed by Joint and ignored for parts that ride
   their joint. */
void crypt_rig_solve(const RiggedModel *m, vec3 root, versor root_rot, versor rot[], vec3 *out_pos,
                     versor *out_quat);
void crypt_pose_idle(versor rot[], float anim, float speed01);
void crypt_pose_swing(versor rot[], float t);

/* ------------------------------------------------------------------- game -- */

typedef enum GameState { GS_PLAYING, GS_CLEARED, GS_CHOOSING, GS_DEAD } GameState;

typedef enum WavePhase { WS_WAIT, WS_SPAWN, WS_CLEAR, WS_DONE } WavePhase;

typedef enum EnemyKind { EK_STAFF, EK_AXE, EK_CROSSBOW, EK_SHIELD, EK_COUNT } EnemyKind;

typedef enum PickupKind { PK_GOLD, PK_HEART, PK_COUNT } PickupKind;

typedef struct Upgrade {
    const char *name;
    const char *detail;
    float       damage;
    float       swing;
    float       speed;
    float       lifesteal;
    float       armor;
    float       greed;
} Upgrade;

extern const Upgrade crypt_upgrades[];
extern const uint32_t crypt_upgrade_count;

typedef enum GameAsset {
    A_FLOOR_TILE, A_FLOOR_DIRT, A_WALL, A_WALL_CORNER, A_WALL_COLUMN, A_PILLAR,
    A_STAIRS, A_TORCH, A_TORCH_WALL, A_CANDLE, A_BARREL, A_CRATES, A_CHEST,
    A_CHEST_GOLD, A_RUBBLE, A_TABLE, A_SHELF, A_BANNER, A_COIN,
    A_SKEL_STAFF, A_SKEL_AXE, A_SKEL_CROSSBOW, A_SKEL_SHIELD_A, A_SKEL_SHIELD_B,
    A_SWORD_1H, A_SWORD_2H, A_AXE_1H, A_WAND, A_SHIELD_ROUND,
    GAME_ASSET_COUNT
} GameAsset;

/* One weapon per character, so the hero's silhouette matches what they carry. */
extern const GameAsset crypt_weapon_for_character[CHARACTER_COUNT];

typedef struct Game {
    VkBackend *vk;
    Scene     *scene;
    Renderer  *renderer;

    SceneAssetLoad asset[GAME_ASSET_COUNT];
    uint32_t       asset_first[GAME_ASSET_COUNT];
    uint32_t       asset_meshes;
    uint32_t       dynamic_instances; /* set from the scene capacities at create */

    /* level */
    mu_pcg32   rng;
    uint32_t   seed;
    uint32_t   depth;
    uint32_t   wave;
    uint32_t   waves_per_floor;
    uint32_t   wave_timer;
    uint32_t   wave_to_spawn;
    WavePhase  wave_phase;
    bool       exit_open;
    int        exit_x, exit_y;
    int        start_x, start_y;
    Room       spawn_room[MAX_ROOMS];
    uint32_t   spawn_room_count;

    uint32_t *dungeon_slot;
    uint32_t  dungeon_used;

    /* player */
    vec3    pos;
    vec3    vel;
    float   yaw;
    float   anim;
    float   health, health_max;
    float   iframes;
    float   attack_cd, attack_t;
    float   dash_cd, dash_t;
    vec3    dash_dir;
    uint32_t character;
    uint32_t part_slot[MAX_ASSET_MESHES];
    uint32_t part_count;
    uint32_t weapon_slot;
    versor    joint_rot[J_COUNT];
    versor    joint_now[J_COUNT];

    /* camera */
    float cam_yaw, cam_pitch, cam_dist, cam_shake;
    vec3  cam_pos;

    /* run */
    float      run_time;
    uint32_t   kills, gold;
    GameState  state;
    Upgrade    roll;
    Upgrade    offer[3];

    /* flow field, rebuilt from the player a few times a second */
    int16_t *flow;
    int     *flow_queue;
    float    flow_timer;

    /* enemies, SoA; `alive` and `dying` are the only sets a system walks */
    vec3         e_pos[MAX_ENEMIES];
    vec3         e_vel[MAX_ENEMIES];
    float        e_yaw[MAX_ENEMIES];
    float        e_anim[MAX_ENEMIES];
    float        e_health[MAX_ENEMIES];
    float        e_speed[MAX_ENEMIES];
    float        e_flash[MAX_ENEMIES];
    float        e_death[MAX_ENEMIES];
    float        e_attack[MAX_ENEMIES];
    float        e_reach[MAX_ENEMIES];
    uint32_t     e_kind[MAX_ENEMIES];
    uint32_t     e_slot[MAX_ENEMIES];
    uint32_t     e_free[MAX_ENEMIES];
    uint32_t     e_free_count;
    uint32_t     e_spawn_q[MAX_ENEMIES];
    uint32_t     e_spawn_count;
    mu_sparse_set e_alive, e_dying;
    /* broad phase: enemy indices bucketed by cell, rebuilt once per step */
    int32_t *bucket_head;
    int32_t *bucket_next;

    /* pickups, SoA */
    vec3         p_pos[MAX_PICKUPS];
    float        p_life[MAX_PICKUPS];
    uint32_t     p_kind[MAX_PICKUPS];
    uint32_t     p_slot[MAX_PICKUPS];
    uint32_t     p_free[MAX_PICKUPS];
    uint32_t     p_free_count;
    uint32_t     p_spawn_q[MAX_PICKUPS];
    uint32_t     p_spawn_count;
    mu_sparse_set p_alive;

    float   accum;
    uint32_t frame;
    bool     uploaded;
} Game;

/* sim.c */
void crypt_sim_reset(Game *g, uint32_t seed);
void crypt_floor_start(Game *g);
void crypt_next_floor(Game *g);
void crypt_sim_step(Game *g, float in_x, float in_z);
void crypt_player_attack(Game *g);
void crypt_player_dash(Game *g);
void crypt_roll_offers(Game *g);
void crypt_take_offer(Game *g, uint32_t index);
void crypt_render_sync(Game *g);
bool crypt_player_alive(const Game *g);

#endif /* CRYPT_RUN_H */