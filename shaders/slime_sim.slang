/* slime_sim.slang — pass 1 of the slime compute chain.
   The whole crowd's hop integrator, expression state machine and per-slime
   randomness run here, one thread per slime, so the CPU never touches a slime
   after spawn. Integration is in place on state_in; the collision-corrected
   result is written to state_out by slime_resolve, and the two buffers swap
   roles each frame. */
#include "common.slang"
#include "../src/three_d/scene_shared.h"

[[vk::push_constant]] SceneSlimePush pc;

/* Per-thread xorshift seeded from the slime's own word and the frame. A shared
   global stream would make the crowd move in lockstep; a per-slime word gives
   each its own sequence deterministically. */
static float rnd(inout uint s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return float(s >> 8) * (1.0f / 16777216.0f);
}

[numthreads(64, 1, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= pc.counts[0])
        return;

    float dt = clamp(global_data.delta_time, 0.0f, 0.1f);
    if (dt <= 0.0f)
        return;

    SlimeState s   = pc.state_in[i];
    uint       rng = s.rng ^ (global_data.frame_count * 747796405u + i * 2891336453u + 1u);

    /* Ambient wrap: the town of slimes follows the player. A slime that drifts
       past the collide radius is re-homed to a fresh spot in the ring around the
       camera, so the crowd is always there without a per-chunk slime manager.
       The old position is simply abandoned — a slime off screen has no history
       worth keeping (rule: regenerate, do not persist). */
    {
        float vdx = s.pos[0] - global_data.camera_pos.x;
        float vdz = s.pos[2] - global_data.camera_pos.z;
        if (vdx * vdx + vdz * vdz > pc.collide_radius * pc.collide_radius)
        {
            float a   = rnd(rng) * 6.2831853f;
            float rad = pc.collide_radius * (0.12f + rnd(rng) * 0.82f);
            s.home[0] = global_data.camera_pos.x + cos(a) * rad;
            s.home[1] = global_data.camera_pos.z + sin(a) * rad;
            s.pos[0]  = s.home[0];
            s.pos[1]  = 0.0f;
            s.pos[2]  = s.home[1];
            s.vel[0] = s.vel[1] = s.vel[2] = 0.0f;
            s.flags &= ~(SLIME_FLAG_AIR | SLIME_FLAG_LAND_HARD);
            s.dwell  = rnd(rng) * 0.6f;
            s.squash = 1.0f;
            s.stretch = 1.0f;
        }
    }

    bool  air    = (s.flags & SLIME_FLAG_AIR) != 0u;
    float squash = 1.0f;   /* vertical: 1 = rest */
    float stretch = 1.0f;  /* forward along facing: 1 = rest */

    if (air)
    {
        s.vel[1] -= 22.0f * dt;
        s.pos[0] += s.vel[0] * dt;
        s.pos[1] += s.vel[1] * dt;
        s.pos[2] += s.vel[2] * dt;
        float hsp = sqrt(s.vel[0] * s.vel[0] + s.vel[2] * s.vel[2]);
        if (s.pos[1] <= 0.0f)
        {
            bool hard = s.vel[1] < -9.0f;
            s.pos[1]  = 0.0f;
            s.squash  = 0.55f; /* landing pancake: short and wide */
            s.stretch = 1.0f;
            s.flags   = (s.flags & ~(SLIME_FLAG_AIR | SLIME_FLAG_LAND_HARD)) |
                        (hard ? SLIME_FLAG_LAND_HARD : 0u);
            s.land_t  = 0.45f;
            s.event_t = 0.9f;
            s.event_id = hard ? 3u : 6u; /* FACE_SCARED : FACE_ANGRY */
            s.vel[0]  = 0.0f;
            s.vel[2]  = 0.0f;
            /* Short rest: the crowd keeps wandering instead of sitting still for
               seconds at a time. The dwell is the pause before the next hop. */
            s.dwell   = 0.05f + rnd(rng) * 0.5f;
            air       = false;
        }
        else
        {
            /* Fast fall reads as a long drop; horizontal speed reads as a
               forward lean along the yaw the hop already points down. */
            squash  = 1.0f + min(abs(s.vel[1]) * 0.03f, 0.35f);
            stretch = 1.0f + min(hsp * 0.045f, 0.35f);
        }
    }
    else
    {
        s.dwell -= dt;
        if (s.dwell <= 0.0f)
        {
            /* Wander: hop to a fresh spot near where the slime already is, not
               back to a fixed home. The step is clamped to a patch around home
               so the crowd roams an area instead of diffusing off the map. */
            float ang  = rnd(rng) * 6.2831853f;
            float step = 1.0f + rnd(rng) * 3.5f;
            float tx   = s.pos[0] + cos(ang) * step;
            float tz   = s.pos[2] + sin(ang) * step;
            float hx   = tx - s.home[0];
            float hz   = tz - s.home[1];
            float hd   = sqrt(hx * hx + hz * hz);
            if (hd > 14.0f)
            {
                tx = s.home[0] + hx / hd * 14.0f;
                tz = s.home[1] + hz / hd * 14.0f;
            }
            float dx  = tx - s.pos[0];
            float dz  = tz - s.pos[2];
            float dl  = sqrt(dx * dx + dz * dz);
            if (dl > 0.001f) { dx /= dl; dz /= dl; }
            else             { dx = cos(ang); dz = sin(ang); }
            float sp = 1.8f + rnd(rng) * 2.2f;
            s.vel[0] = dx * sp;
            s.vel[1] = 6.5f + rnd(rng) * 3.0f;
            s.vel[2] = dz * sp;
            s.squash = 0.8f; /* coiled push-off */
            s.yaw_tgt = atan2(dx, dz);
            s.flags |= SLIME_FLAG_AIR;
            air = true;
        }
    }

    /* One fast ease toward the target so the pancake reads as a single impact
       and the resting shape returns gradually (dt*15 = quick, ~0.1 s). */
    s.squash  += (squash - s.squash) * min(dt * 15.0f, 1.0f);
    s.stretch += (stretch - s.stretch) * min(dt * 12.0f, 1.0f);

    float dy = s.yaw_tgt - s.yaw;
    while (dy > 3.14159265f) dy -= 6.2831853f;
    while (dy < -3.14159265f) dy += 6.2831853f;
    s.yaw += dy * min(dt * 6.0f, 1.0f);

    s.wobble -= s.wobble * min(dt * 9.0f, 1.0f);
    if (s.land_t > 0.0f) s.land_t -= dt;
    if (s.event_t > 0.0f) s.event_t -= dt;

    /* Blink beats every other mood. */
    uint face = s.face_id;
    if (s.blink_len > 0.0f)
    {
        s.blink_len -= dt;
        face = 1u; /* FACE_BLINK */
        if (s.blink_len <= 0.0f)
            s.blink = 1.6f + rnd(rng) * 4.0f;
    }
    else if (s.blink > 0.0f)
    {
        s.blink -= dt;
    }
    else
    {
        s.blink_len = 0.12f;
    }

    if (s.blink_len <= 0.0f)
    {
        if (s.land_t > 0.0f)      face = ((s.flags & SLIME_FLAG_LAND_HARD) != 0u) ? 3u : 0u;
        else if (air)             face = 2u; /* FACE_ELATED */
        else if (s.event_t > 0.0f) face = s.event_id;
        else if (s.dwell > 1.8f)  face = 7u; /* FACE_SLEEPY */
        else if (s.dwell > 1.0f)  face = 4u; /* FACE_HUNGRY */
        else                      face = 0u; /* FACE_HAPPY */
    }
    s.face_id = face;

    s.flags = (s.flags & ~SLIME_FLAG_AIR) | (air ? SLIME_FLAG_AIR : 0u);
    s.rng   = rng;
    pc.state_in[i] = s;
}
