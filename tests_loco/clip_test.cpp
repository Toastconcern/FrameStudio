// Frame Home never puts your eyes inside things or your feet through a floor (Locomotion over a baked walkable area):
// walking down an open staircase along its side, and walking / leaning into a wall for real.
#include "locomotion.h"
#include <cstdio>
static int fails = 0;
static void ck(bool ok, const char* w, float a = 0, float b = 0, float c = 0) { std::printf("%s  %s  (%.2f %.2f %.2f)\n", ok ? "PASS" : "FAIL", w, a, b, c); fails += !ok; }
static WorldCollider box(float x0, float y0, float z0, float x1, float y1, float z1) {
    WorldCollider c; c.kind = WorldCollider::Box; c.role = WorldCollider::Solid;
    c.half[0] = (x1 - x0) / 2; c.half[1] = (y1 - y0) / 2; c.half[2] = (z1 - z0) / 2;
    c.world[12] = (x0 + x1) / 2; c.world[13] = (y0 + y1) / 2; c.world[14] = (z0 + z1) / 2;
    return c;
}
static void bake(HomeWorld& w) {
    std::vector<WalkSource> src;
    std::vector<std::vector<float>> P;
    std::vector<std::vector<uint32_t>> I;
    walkColliderSources(w, src, P, I);
    w.walkGridCache = buildWalkGrid(src, walkSeeds(w), WalkParams{}, nullptr);
}
int main() {
    {   // an open staircase: a 1.8 m landing, 8 treads 20 cm down, 30 cm deep, 1 m wide, the ground all round and under
        HomeWorld w;
        w.colliders.push_back(box(-8, -0.1f, -8, 8, 0, 8));
        w.colliders.push_back(box(-4, 0, -1, -2, 1.8f, 1));
        for (int i = 0; i < 8; ++i) w.colliders.push_back(box(-2 + 0.3f * i, 1.8f - 0.2f * (i + 1) - 0.05f, -0.5f, -2 + 0.3f * (i + 1), 1.8f - 0.2f * (i + 1), 0.5f));
        WorldPoint sp; sp.pos[0] = -3; sp.pos[1] = 1.8f; sp.pos[2] = 0.38f; sp.yaw = -1.5708f;   // facing +X, down the stairs
        w.spawns.push_back(sp);
        WorldPoint g; g.pos[0] = 4; g.pos[1] = 0; g.pos[2] = 0; w.hotspots.push_back(g);
        bake(w);
        Locomotion loco;
        loco.setWorld(w);
        float head[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.70f, 0, 1};
        loco.testInput(0, 1, 0, 0);
        float worst = 1e9f, x = -3, y = 0;
        for (int i = 0; i < 900 && x < 0.5f; ++i) {
            loco.update(1.0f / 90, head);
            float R[16];
            loco.rig(R);
            x = R[0] * head[12] + R[4] * head[13] + R[8] * head[14] + R[12];
            y = R[1] * head[12] + R[5] * head[13] + R[9] * head[14] + R[13];
            float tread = 0;                                           // the top under the eyes
            if (x < -2) tread = 1.8f;
            else if (x < 0.4f) tread = std::max(0.0f, 1.8f - 0.2f * (std::floor((x + 2) / 0.3f) + 1));
            worst = std::min(worst, (y - 1.7f) - tread);
        }
        loco.testInput(0, 0, 0, 0);
        for (int i = 0; i < 90; ++i) loco.update(1.0f / 90, head);   // the last step eases down
        { float R[16]; loco.rig(R); y = R[1] * head[12] + R[5] * head[13] + R[9] * head[14] + R[13]; }
        ck(x > 0.3f, "walked down the stairs along their open side", x, y);
        ck(worst > -0.25f, "the feet never sink more than a quarter step under the tread (never through it)", worst);
        ck(std::fabs(y - 1.7f) < 0.05f, "at the bottom, standing on the ground", y);
    }
    {   // a wall 1 m ahead: walk into it for real (no stick): the eyes stop short of it
        HomeWorld w;
        w.colliders.push_back(box(-8, -0.1f, -8, 8, 0, 8));
        w.colliders.push_back(box(0.9f, 0, -2, 1.1f, 2.5f, 2));
        WorldPoint sp; sp.pos[0] = 0; sp.pos[1] = 0; sp.pos[2] = 0; w.spawns.push_back(sp);
        bake(w);
        Locomotion loco;
        loco.setWorld(w);
        float head[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.70f, 0, 1};
        float most = -1e9f;
        for (int i = 0; i < 300; ++i) {
            head[12] = std::min(1.6f, i * 0.008f);                     // stepping forward 1.6 m in the room
            head[13] = 1.70f - (i > 200 ? 0.6f : 0.0f);                // then bending down
            loco.update(1.0f / 90, head);
            float R[16];
            loco.rig(R);
            most = std::max(most, R[0] * head[12] + R[4] * head[13] + R[8] * head[14] + R[12]);
        }
        ck(most <= 0.9f - 0.14f, "walking into a wall for real, the eyes stay 15 cm off it", most);
    }
    {   // a coffee table (45 cm high) in the way: walking into it for real, you're kept at its edge
        HomeWorld w;
        w.colliders.push_back(box(-8, -0.1f, -8, 8, 0, 8));
        w.colliders.push_back(box(0.8f, 0, -0.5f, 1.6f, 0.45f, 0.5f));
        WorldPoint sp; sp.pos[0] = 0; sp.pos[1] = 0; sp.pos[2] = 0; w.spawns.push_back(sp);
        bake(w);
        Locomotion loco;
        loco.setWorld(w);
        float head[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.70f, 0, 1};
        float most = -1e9f;
        for (int i = 0; i < 250; ++i) {
            head[12] = std::min(1.2f, i * 0.008f);
            loco.update(1.0f / 90, head);
            float R[16];
            loco.rig(R);
            most = std::max(most, R[0] * head[12] + R[4] * head[13] + R[8] * head[14] + R[12]);
        }
        ck(most <= 0.85f, "walking into a coffee table for real, you stop at its edge", most);
    }
    std::printf("%d failed\n", fails);
    return fails != 0;
}
