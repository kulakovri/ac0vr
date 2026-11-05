// ps2_trails.c - contrails, wing vapour, heat haze, missile smoke light: the state zero_trails.c (EE thread)
// publishes for the renderer (ps2_video.c) and the smoke sprites (rn_vp_fx.c), and the switches. In ac5 and gsreplay
// (the replay has no publisher: nothing is drawn there). See runtime/include/ps2_trails.h.
#include "ps2_trails.h"
#include "ps2_runtime.h"
#include "ps2_menv.h"
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pt_cfg_t pt_c;
static int pt_c_ok;
static float env_f(const char *n, float d) {
    const char *e = ps2_menv(n);
    if (!e || !*e) return d;
    float v = (float)atof(e);
    return v == v ? v : d;
}
static void pt_read(pt_cfg_t *c) {
    c->contrails = env_f("ACZ_VR_CONTRAILS", 1.0f);
    c->vapour = env_f("ACZ_VR_VAPOUR", 1.0f);
    c->haze = env_f("ACZ_VR_HEAT_HAZE", 1.0f);
    c->smoke = env_f("ACZ_VR_SMOKE_LIGHT", 1.0f);
    c->wake = env_f("ACZ_VR_CLOUD_WAKE", 1.0f);
    c->ab = (int)env_f("ACZ_VR_TRAILS_AB", 0.0f);
    c->debug = (int)env_f("ACZ_DEBUG_TRAILS", 0.0f);
    if (c->contrails < 0.0f) c->contrails = 0.0f;
    if (c->vapour < 0.0f) c->vapour = 0.0f;
    if (c->haze < 0.0f) c->haze = 0.0f;
    if (c->smoke < 0.0f) c->smoke = 0.0f;
    if (!(c->wake >= 0.0f)) c->wake = 0.0f;
    if (c->wake > 2.0f) c->wake = 2.0f;
    if (c->contrails > 3.0f) c->contrails = 3.0f;
    if (c->vapour > 3.0f) c->vapour = 3.0f;
    if (c->haze > 3.0f) c->haze = 3.0f;
    if (c->smoke > 3.0f) c->smoke = 3.0f;
}
const pt_cfg_t *pt_cfg(void) {
    if (!pt_c_ok) {
        pt_read(&pt_c);
        pt_c_ok = 1;
    }
    return &pt_c;
}
/* the NEXT MISSION tier (ps2_menv.h): the game thread, at the edge; the renderer's trail pass reads these per frame, each value
   whole: from here on the new ones, with no gap between the two threads */
void pt_menv_reload(void) {
    if (!pt_c_ok) return;
    pt_cfg_t n = pt_c;
    pt_read(&n);
    ps2_menv_note("ACZ_VR_CONTRAILS", pt_c.contrails, n.contrails); ps2_menv_note("ACZ_VR_VAPOUR", pt_c.vapour, n.vapour);
    ps2_menv_note("ACZ_VR_HEAT_HAZE", pt_c.haze, n.haze); ps2_menv_note("ACZ_VR_SMOKE_LIGHT", pt_c.smoke, n.smoke);
    ps2_menv_note("ACZ_VR_CLOUD_WAKE", pt_c.wake, n.wake);
    pt_c = n;
}

static pthread_mutex_t pt_mx = PTHREAD_MUTEX_INITIALIZER;
static pt_frame pt_pub;
static double pt_pub_wall;
static double pt_wall_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void pt_copy(pt_frame *o, const pt_frame *f) {
    size_t head = offsetof(pt_frame, trail);
    memcpy(o, f, head);
    if (o->ntrail > PT_MAX_TRAILS) o->ntrail = PT_MAX_TRAILS;
    if (o->npt > PT_MAX_PTS) o->npt = PT_MAX_PTS;
    if (o->npuff > PT_MAX_PUFFS) o->npuff = PT_MAX_PUFFS;
    if (o->nhaze > PT_MAX_HAZE) o->nhaze = PT_MAX_HAZE;
    memcpy(o->trail, f->trail, o->ntrail * sizeof f->trail[0]);
    memcpy(o->puff, f->puff, o->npuff * sizeof f->puff[0]);
    memcpy(o->haze, f->haze, o->nhaze * sizeof f->haze[0]);
    memcpy(o->pt, f->pt, o->npt * sizeof f->pt[0]);
}

void pt_publish(const pt_frame *f) {
    pthread_mutex_lock(&pt_mx);
    uint32_t s = pt_pub.seq + 1u;
    pt_copy(&pt_pub, f);
    pt_pub.seq = s;
    pt_pub_wall = pt_wall_s();
    pthread_mutex_unlock(&pt_mx);
}

int pt_latest(pt_frame *out, double stale_s, uint32_t *seq) {
    int ok;
    pthread_mutex_lock(&pt_mx);
    ok = pt_pub.seq != 0u && pt_wall_s() - pt_pub_wall <= stale_s;
    if (ok && (!seq || *seq != pt_pub.seq)) {
        pt_copy(out, &pt_pub);
        if (seq) *seq = pt_pub.seq;
    }
    pthread_mutex_unlock(&pt_mx);
    return ok;
}

/* the cloud wake's probe: requests (EE -> renderer) and answers (renderer -> EE) */
static pthread_mutex_t pt_wk_mx = PTHREAD_MUTEX_INITIALIZER;
static pt_wake_req pt_wk_req[PT_WAKE_MAX];
static uint32_t pt_wk_nreq;
static double pt_wk_req_wall;
static struct { uint32_t key, field; float d, dmax; } pt_wk_res[2u * PT_WAKE_MAX];
static uint32_t pt_wk_nres;

void pt_wake_request(const pt_wake_req *r, uint32_t n) {
    if (n > PT_WAKE_MAX) n = PT_WAKE_MAX;
    pthread_mutex_lock(&pt_wk_mx);
    memcpy(pt_wk_req, r, n * sizeof *r);
    pt_wk_nreq = n;
    pt_wk_req_wall = pt_wall_s();
    pthread_mutex_unlock(&pt_wk_mx);
}
uint32_t pt_wake_requests(pt_wake_req *out, uint32_t max, double stale_s) {
    uint32_t n = 0;
    pthread_mutex_lock(&pt_wk_mx);
    if (pt_wk_nreq && pt_wall_s() - pt_wk_req_wall <= stale_s) {
        n = pt_wk_nreq < max ? pt_wk_nreq : max;
        memcpy(out, pt_wk_req, n * sizeof *out);
    }
    pthread_mutex_unlock(&pt_wk_mx);
    return n;
}
void pt_wake_result(uint32_t key, uint32_t field, float dens, float dmax) {
    pthread_mutex_lock(&pt_wk_mx);
    uint32_t i, old = 0;
    for (i = 0; i < pt_wk_nres && pt_wk_res[i].key != key; i++)
        if ((int32_t)(pt_wk_res[i].field - pt_wk_res[old].field) < 0) old = i;
    if (i < pt_wk_nres && (int32_t)(field - pt_wk_res[i].field) < 0) i = 2u * PT_WAKE_MAX;   /* (an older answer) */
    else if (i == pt_wk_nres) {
        if (pt_wk_nres < 2u * PT_WAKE_MAX) pt_wk_nres++;
        else i = old;                                       /* (full: the stalest goes) */
    }
    if (i < 2u * PT_WAKE_MAX) {
        pt_wk_res[i].key = key; pt_wk_res[i].field = field; pt_wk_res[i].d = dens; pt_wk_res[i].dmax = dmax;
    }
    pthread_mutex_unlock(&pt_wk_mx);
}
int pt_wake_get(uint32_t key, float *dens, float *dmax, uint32_t *field) {
    int ok = 0;
    pthread_mutex_lock(&pt_wk_mx);
    for (uint32_t i = 0; i < pt_wk_nres; i++)
        if (pt_wk_res[i].key == key) {
            if (dens) *dens = pt_wk_res[i].d;
            if (dmax) *dmax = pt_wk_res[i].dmax;
            if (field) *field = pt_wk_res[i].field;
            ok = 1;
            break;
        }
    pthread_mutex_unlock(&pt_wk_mx);
    return ok;
}

volatile pt_smoke_t pt_smoke;
uint64_t pt_smoke_batches, pt_smoke_parts;

float pt_smoke_light(const float w[3], const float eye[3], float *Tout) {
    float k = pt_smoke.k;
    float T = ps2_vk_cloud_sun_T(w);
    if (Tout) *Tout = T;
    if (!(k > 0.0f)) return 1.0f;
    float s[3] = { pt_smoke.sun[0], pt_smoke.sun[1], pt_smoke.sun[2] };
    float f = 1.0f;
    float sl = s[0] * s[0] + s[1] * s[1] + s[2] * s[2];
    if (sl > 0.25f) {
        /* forward scattering of the smoke's droplets / soot: bright looking towards the sun, darker away from it;
           the same view-sun cosine the game's own cloud impostors are shaded by (t = 0.578 + 0.429 cos) */
        float v[3] = { w[0] - eye[0], w[1] - eye[1], w[2] - eye[2] };
        float vl = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (vl > 1e-3f) {
            float c = (v[0] * s[0] + v[1] * s[1] + v[2] * s[2]) / (vl * sqrtf(sl));
            /* a soft forward lobe: (c + 1)^2 / 4 in 0..1, 0.25 at the side; mean over the sphere 1/3 */
            float lobe = 0.25f * (c + 1.0f) * (c + 1.0f);
            float m = c >= 0.0f ? 1.0f + (pt_smoke.fwd - 1.0f) * (lobe - 0.25f) / 0.75f
                                : 1.0f - (1.0f - pt_smoke.back) * (0.25f - lobe) / 0.25f;
            /* in a cloud's shadow the direct sun (and with it the lobe) goes */
            f = 1.0f + (m - 1.0f) * T;
        }
    }
    f *= 1.0f - pt_smoke.shadow_k * (1.0f - T);
    /* (tunnel_light, bug #267) under a roof near the eye no sun reaches the smoke: the game's own colour as it drew it,
       neither the sun's lobe nor a cloud's shade */
    return 1.0f + k * (f - 1.0f) * ps2_vk_enclosure_sun_at(w);
}
