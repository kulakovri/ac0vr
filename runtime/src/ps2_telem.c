/* ps2_telem.c -- the local telemetry stream for motion rigs, haptics and force-feedback sticks (ACZ_TELEMETRY, telem_udp
   2026-11-04; RELEASE_RESEARCH R13; docs/TELEMETRY.md is the reader's description).

   One UDP datagram per game frame (in-between frames send nothing) to ACZ_TELEMETRY=host:port (off by default;
   127.0.0.1:20777 is the documented local choice, 255.255.255.255:20777 the LAN broadcast), from the renderer thread right
   after the EE's publish: pump_events calls ps2_telem_tick every loop, and a datagram goes when zero_vr.c published a new
   flight frame (outside flight: when the field changed). The socket only sends: no bind, no listen, no Windows Firewall
   prompt, and it stays on the player's PC or LAN. sendto on a non-blocking socket; a datagram that would block is dropped;
   counters in the packet mean a reader at any rate misses nothing. A host name is resolved once on a thread of its own, so
   a slow DNS never holds the renderer.

   Formats (ACZ_TELEMETRY_FORMAT): json (default; one line, UTF-8, < 1200 bytes), forza (the Forza Motorsport "Data Out" Dash
   packet, 331 bytes, so SimHub / SimTools / haptics tools read it without a plugin), forza7 (the same without the 2023
   extension, 311 bytes, for tools that check the length). ENHANCEMENTS > TELEMETRY OUTPUT / FORMAT change both live.
   ACZ_DEBUG_TELEMETRY=N: every N datagrams a log line: the rate, the drops, the sizes, the cost on this thread and on the
   EE thread (off by default; the EE's part is only timed with it). */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif
#include "ps2_runtime.h"
#include "ps2_profile.h"                      /* ps2_version_line: the "version" field */
#include "zero_instr.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

u64 ps2_kernel_vblank_count(void);

static int (*volatile tg_source)(zero_telem *);
static double (*volatile tg_cpu)(void);
void ps2_telem_set_source(int (*fn)(zero_telem *), double (*cpu)(void)) { tg_source = fn; tg_cpu = cpu; }

enum { TF_JSON, TF_FORZA, TF_FORZA7 };
static struct {
    int inited;
    int format;
    volatile int on;                      /* a target is set */
#ifdef _WIN32
    SOCKET sock;
    struct sockaddr_in to;
    volatile int to_ready;                /* to holds the address (a host name: once resolved) */
    volatile unsigned gen;                /* the target's generation (a resolver answering late) */
#endif
    unsigned last_frame;
    u64 last_field;
    u64 sent, dropped, oversize;
    int dbg;
    u64 win_n, win_cost, win_max, win_bytes_max, win_t0, freq;
    char version[64];                     /* ps2_version_line, as a JSON string */
} tg;

/* the aircraft ids (docs/aircraft_ids/README.md) */
static const char *const tg_names[36] = {
    "J35J Draken", "Gripen C", "Typhoon", "Tornado GR.4", "F-4E", "F-15C", "F-15E", "F-15 S/MTD", "F/A-18C", "EA-18G", "F-16C",
    "F-16XL", "F-117A", "F/A-22A", "F-35C", "F-5E", "F-20A", "X-29A", "F-14D Super Tomcat", "YF-23A", "EA-6B", "A-10A",
    "Mirage 2000D", "Rafale M", "Su-27", "Su-32", "Su-37", "Su-47", "MiG-21bis", "MiG-29A", "MiG-31", "F-1", "F-2A",
    "X-02 Wyvern", "ADF-01 FALKEN", "ADFX-01 Morgan" };
static const char *const tg_state_names[] = { "menu", "flight", "paused", "briefing", "movie", "cutscene", "loading" };

/* ---- the target ---- */
#ifdef _WIN32
typedef struct { char host[200]; u16 port; unsigned gen; } tg_lookup;
static void *tg_resolve(void *arg) {
    tg_lookup *l = (tg_lookup *)arg;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    int r = getaddrinfo(l->host, NULL, &hints, &res);
    if (r == 0 && res && l->gen == tg.gen) {
        tg.to.sin_family = AF_INET;
        tg.to.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        tg.to.sin_port = htons(l->port);
        __atomic_store_n(&tg.to_ready, 1, __ATOMIC_RELEASE);
        ps2_log("telemetry: %s resolved", l->host);
    } else if (r != 0) ps2_log("telemetry: cannot resolve %s; nothing is sent", l->host);
    if (res) freeaddrinfo(res);
    free(l);
    return NULL;
}
#endif

/* "host:port"; "", "0", "off" = off. Renderer thread (the first tick, the menu). */
void ps2_telem_set_target(const char *v) {
#ifdef _WIN32
    char host[200] = "";
    const char *colon = v ? strrchr(v, ':') : NULL;
    long port = colon ? strtol(colon + 1, NULL, 10) : 0;
    __atomic_store_n(&tg.to_ready, 0, __ATOMIC_RELEASE);
    tg.gen++;
    if (!v || !*v || !strcmp(v, "0") || !SDL_strcasecmp(v, "off")) {
        if (tg.on) ps2_log("telemetry: off");
        tg.on = 0;
        return;
    }
    if (!colon || colon == v || port < 1 || port > 65535 || (size_t)(colon - v) >= sizeof host) {
        ps2_log("telemetry: ACZ_TELEMETRY=%s is not host:port; off", v);
        tg.on = 0;
        return;
    }
    memcpy(host, v, (size_t)(colon - v));
    host[colon - v] = 0;
    if (tg.sock == INVALID_SOCKET || !tg.sock) {
        WSADATA w;
        u_long nb = 1;
        int yes = 1;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { ps2_log("telemetry: no sockets (WSAStartup)"); tg.on = 0; return; }
        tg.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (tg.sock == INVALID_SOCKET) { ps2_log("telemetry: no socket (%d)", WSAGetLastError()); tg.on = 0; return; }
        ioctlsocket(tg.sock, FIONBIO, &nb);
        setsockopt(tg.sock, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof yes);   /* (the LAN choice) */
    }
    memset(&tg.to, 0, sizeof tg.to);
    tg.to.sin_family = AF_INET;
    tg.to.sin_port = htons((u_short)port);
    if (!SDL_strcasecmp(host, "localhost")) snprintf(host, sizeof host, "127.0.0.1");
    if (inet_pton(AF_INET, host, &tg.to.sin_addr) == 1) {
        __atomic_store_n(&tg.to_ready, 1, __ATOMIC_RELEASE);
    } else {
        tg_lookup *l = (tg_lookup *)malloc(sizeof *l);
        pthread_t t;
        if (l) {
            snprintf(l->host, sizeof l->host, "%s", host);
            l->port = (u16)port;
            l->gen = tg.gen;
            if (pthread_create(&t, NULL, tg_resolve, l) == 0) pthread_detach(t);
            else free(l);
        }
    }
    tg.on = 1;
    tg.last_field = 0;
    ps2_log("telemetry: %s, format %s, one datagram per game frame", v,
            tg.format == TF_JSON ? "json" : tg.format == TF_FORZA ? "forza (331 bytes)" : "forza7 (311 bytes)");
#else
    (void)v;
#endif
}
void ps2_telem_set_format(const char *v) {
    tg.format = v && !SDL_strcasecmp(v, "forza") ? TF_FORZA : v && !SDL_strcasecmp(v, "forza7") ? TF_FORZA7 : TF_JSON;
    if (tg.inited) ps2_log("telemetry: format %s", tg.format == TF_JSON ? "json" : tg.format == TF_FORZA ? "forza" : "forza7");
}

/* ---- JSON: one line. Flight values are null when the game has none (menus, movies, a load), absent things always null. ---- */
#define J(...) do { if (k < cap) k += snprintf(o + k, (size_t)(cap - k), __VA_ARGS__); } while (0)
static int tg_json(char *o, int cap, const zero_telem *t, u64 ms) {
    int k = 0, have = t->in.valid && (t->state == ZT_FLIGHT || t->state == ZT_PAUSED || t->state == ZT_CUTSCENE);
    const zero_instr *v = &t->in;
    J("{\"v\":1,\"version\":\"%s\",\"field\":%llu,\"t_ms\":%llu,\"state\":\"%s\",", tg.version, (unsigned long long)t->field,
      (unsigned long long)ms, tg_state_names[t->state]);
    if (t->aircraft >= 0 && t->aircraft < 36) J("\"aircraft\":{\"id\":%d,\"name\":\"%s\"},", t->aircraft, tg_names[t->aircraft]);
    else J("\"aircraft\":null,");
    if (t->mission > 0) J("\"mission\":%d,", t->mission); else J("\"mission\":null,");
    if (have) {
        J("\"speed_kt\":%.1f,\"mach\":%.3f,\"alt_ft\":%.0f,\"heading_deg\":%.2f,\"pitch_deg\":%.2f,\"roll_deg\":%.2f,\"vsi_fpm\":%.0f,"
          "\"g\":%.2f,\"lat_g\":%.2f,\"long_g\":%.2f,\"aoa_deg\":%.2f,\"sideslip_deg\":%.2f,"
          "\"pitch_rate_dps\":%.2f,\"yaw_rate_dps\":%.2f,\"roll_rate_dps\":%.2f,\"vel_ms\":[%.1f,%.1f,%.1f],"
          "\"throttle\":%.3f,\"afterburner\":%.3f,\"airbrake\":%.3f,\"stick\":[%.3f,%.3f,%.3f],\"missiles\":%d,\"gun\":%d,\"special\":%d,",
          v->speed_kt, t->mach, v->alt_ft, v->heading_deg + 0.0f, v->pitch_deg, -v->roll_deg + 0.0f, v->vsi_fpm, v->g, t->lat_g,
          t->long_g, v->aoa_deg + 0.0f,
          t->sideslip_deg, t->rate_dps[0], t->rate_dps[1], t->rate_dps[2], t->vel[0], t->vel[1], t->vel[2], v->throttle, v->afterburner, v->airbrake, v->stick[0],
          v->stick[1], -v->stick[2] + 0.0f, v->missiles, v->gun, v->special);
    } else {
        J("\"speed_kt\":null,\"mach\":null,\"alt_ft\":null,\"heading_deg\":null,\"pitch_deg\":null,\"roll_deg\":null,\"vsi_fpm\":null,"
          "\"g\":null,\"lat_g\":null,\"long_g\":null,\"aoa_deg\":null,\"sideslip_deg\":null,"
          "\"pitch_rate_dps\":null,\"yaw_rate_dps\":null,\"roll_rate_dps\":null,\"vel_ms\":null,"
          "\"throttle\":null,\"afterburner\":null,\"airbrake\":null,\"stick\":null,\"missiles\":null,\"gun\":null,\"special\":null,");
    }
    J("\"gun_shots\":%u,\"missiles_fired\":%u,\"specials_fired\":%u,\"hits_taken\":%u,", t->gun_shots, t->missiles_fired,
      t->specials_fired, t->hits_taken);
#define JB(n, c) J("\"" n "\":%s,", !have ? "null" : (c) ? "true" : "false")
    int live = t->state == ZT_FLIGHT;     /* (a paused game fires nothing: the held values, the event flags down) */
    JB("gun_firing", live && (t->flags & ZTF_GUN));
    JB("stall", live && (t->flags & ZTF_STALL));
    JB("pull_up", live && (t->flags & ZTF_PULL_UP));
    JB("locked", live && (t->flags & ZTF_LOCKED));
    JB("missile_alert", live && (t->flags & ZTF_MISSILE_ALERT));
    JB("high_g", live && (t->flags & ZTF_HIGH_G));
    JB("airbrake_on", v->airbrake > 0.05f);
    JB("afterburner_on", v->afterburner > 0.05f);
    if (have && t->gear >= 0.0f) J("\"gear\":%s,", t->gear > 0.5f ? "true" : "false");
    else J("\"gear\":null,");
    JB("on_ground", t->flags & ZTF_ON_GROUND);
#undef JB
    if (have && t->health_pct >= 0.0f) J("\"health_pct\":%.1f,", t->health_pct);
    else J("\"health_pct\":null,");
    J("\"flaps\":null,\"hook\":null,\"post_stall\":null,\"overspeed\":null}");
    return k;
}
#undef J

/* ---- Forza Motorsport "Data Out", Dash (the table and its source: docs/TELEMETRY.md). 331 bytes, little-endian. ---- */
#pragma pack(push, 1)
typedef struct {
    int32_t IsRaceOn; uint32_t TimestampMS;
    float EngineMaxRpm, EngineIdleRpm, CurrentEngineRpm;
    float AccelerationX, AccelerationY, AccelerationZ;
    float VelocityX, VelocityY, VelocityZ;
    float AngularVelocityX, AngularVelocityY, AngularVelocityZ;
    float Yaw, Pitch, Roll;
    float NormSuspension[4], TireSlipRatio[4], WheelRotationSpeed[4];
    int32_t WheelOnRumbleStrip[4];
    float WheelInPuddleDepth[4], SurfaceRumble[4], TireSlipAngle[4], TireCombinedSlip[4], SuspensionTravelMeters[4];
    int32_t CarOrdinal, CarClass, CarPerformanceIndex, DrivetrainType, NumCylinders;
    /* Dash from here (byte 232) */
    float PositionX, PositionY, PositionZ, Speed, Power, Torque;
    float TireTemp[4];
    float Boost, Fuel, DistanceTraveled, BestLap, LastLap, CurrentLap, CurrentRaceTime;
    uint16_t LapNumber;
    uint8_t RacePosition, Accel, Brake, Clutch, HandBrake, Gear;
    int8_t Steer, NormalizedDrivingLine, NormalizedAIBrakeDifference;
    /* the Forza Motorsport (2023) extension (byte 311) */
    float TireWear[4];
    int32_t TrackOrdinal;
} tg_forza;
#pragma pack(pop)
_Static_assert(sizeof(tg_forza) == 331, "the Dash packet");

static uint8_t tg_u8(float v) { v = v * 255.0f + 0.5f; return (uint8_t)(v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v); }
static void tg_forza_fill(tg_forza *p, const zero_telem *t, u64 ms) {
    memset(p, 0, sizeof *p);
    p->TimestampMS = (uint32_t)ms;
    p->CarOrdinal = t->aircraft;
    p->TrackOrdinal = t->mission;
    if (t->state != ZT_FLIGHT || !t->in.valid) return;      /* as the game: all zero in menus and pauses */
    const zero_instr *v = &t->in;
    float th = v->throttle < 0.0f ? 0.0f : v->throttle > 1.0f ? 1.0f : v->throttle;
    p->IsRaceOn = 1;
    p->EngineMaxRpm = 8000.0f; p->EngineIdleRpm = 1000.0f; p->CurrentEngineRpm = 1000.0f + 7000.0f * th;
    p->AccelerationX = t->acc[0]; p->AccelerationY = t->acc[1]; p->AccelerationZ = t->acc[2];
    p->VelocityX = t->vel[0]; p->VelocityY = t->vel[1]; p->VelocityZ = t->vel[2];
    p->AngularVelocityX = t->rate_dps[0] * 0.01745329f;
    p->AngularVelocityY = t->rate_dps[1] * 0.01745329f;
    p->AngularVelocityZ = t->rate_dps[2] * 0.01745329f;
    {   float h = v->heading_deg > 180.0f ? v->heading_deg - 360.0f : v->heading_deg;
        p->Yaw = h * 0.01745329f;
    }
    p->Pitch = t->pitch_rad; p->Roll = t->roll_rad;
    p->PositionX = t->pos[0]; p->PositionY = t->pos[1]; p->PositionZ = t->pos[2];
    p->Speed = v->speed_kt * 0.51444f;
    p->Power = th * 100000.0f;
    p->Boost = v->afterburner;
    p->Accel = tg_u8(th);
    p->Brake = tg_u8(v->airbrake);
    {   float s = v->stick[1] * 127.0f;
        p->Steer = (int8_t)(s < -127.0f ? -127 : s > 127.0f ? 127 : (int)s);
    }
}

/* ---- every renderer loop ---- */
static void tg_debug(u64 now_ticks) {
    double secs = (double)(now_ticks - tg.win_t0) / (double)tg.freq;
    double (*cpu)(void) = tg_cpu;
    ps2_log("telemetry: %.1f datagrams/s (%llu sent, %llu dropped, %llu too long), largest %llu bytes; renderer thread %.1f us mean, %.1f max "
            "per datagram; EE thread %.2f us per game frame",
            secs > 0.0 ? (double)tg.win_n / secs : 0.0, (unsigned long long)tg.sent, (unsigned long long)tg.dropped,
            (unsigned long long)tg.oversize, (unsigned long long)tg.win_bytes_max,
            tg.win_n ? (double)tg.win_cost * 1e6 / (double)tg.freq / (double)tg.win_n : 0.0,
            (double)tg.win_max * 1e6 / (double)tg.freq, cpu ? cpu() : 0.0);
    tg.win_n = tg.win_cost = tg.win_max = tg.win_bytes_max = 0;
    tg.win_t0 = now_ticks;
}

void ps2_telem_tick(void) {
    if (!tg.inited) {
        tg.inited = 1;
        if (!tg_source) return;               /* (gsreplay: no game) */
        const char *e = getenv("ACZ_TELEMETRY_FORMAT");
        ps2_telem_set_format(e);
        e = getenv("ACZ_DEBUG_TELEMETRY");
        tg.dbg = e && *e ? atoi(e) : 0;
        if (tg.dbg == 1) tg.dbg = 600;
        tg.freq = SDL_GetPerformanceFrequency();
        {   /* the build's version, once (version.txt beside the exe, or CMake's), without the two JSON specials */
            const char *v = ps2_version_line();
            size_t k = 0;
            for (; v[k] && k + 1 < sizeof tg.version; k++) tg.version[k] = v[k] == '"' || v[k] == '\\' ? '\'' : v[k];
            tg.version[k] = 0;
        }
        ps2_telem_set_target(getenv("ACZ_TELEMETRY"));
    }
#ifdef _WIN32
    if (!tg.on) return;
    int (*src)(zero_telem *) = tg_source;
    if (!src || !__atomic_load_n(&tg.to_ready, __ATOMIC_ACQUIRE)) return;
    u64 t0 = SDL_GetPerformanceCounter();
    zero_telem t;
    src(&t);
    u64 f = ps2_kernel_vblank_count();
    if (t.publishing) {                                     /* the EE publishes: its frames */
        if (t.in.frame == tg.last_frame) return;
        tg.last_frame = t.in.frame;
    } else if (f == tg.last_field) return;                  /* menus, pauses, an intro, a crash: one per field, for the state */
    tg.last_field = f;
    u64 ms = (u64)(ps2_wall_seconds() * 1000.0);
    union { char j[1200]; tg_forza z; } b;
    int n;
    if (tg.format == TF_JSON) {
        n = tg_json(b.j, (int)sizeof b.j, &t, ms);
        if (n >= (int)sizeof b.j) { tg.oversize++; return; }
    } else {
        tg_forza_fill(&b.z, &t, ms);
        n = tg.format == TF_FORZA ? (int)sizeof b.z : 311;
    }
    if (sendto(tg.sock, (const char *)&b, n, 0, (const struct sockaddr *)&tg.to, sizeof tg.to) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (!tg.dropped && err != WSAEWOULDBLOCK) ps2_log("telemetry: sendto failed (%d); dropping", err);
        tg.dropped++;
    } else tg.sent++;
    if (tg.dbg) {
        u64 t1 = SDL_GetPerformanceCounter(), c = t1 - t0;
        if (!tg.win_t0) tg.win_t0 = t0;
        tg.win_n++; tg.win_cost += c;
        if (c > tg.win_max) tg.win_max = c;
        if ((u64)n > tg.win_bytes_max) tg.win_bytes_max = (u64)n;
        if (tg.win_n >= (u64)tg.dbg) tg_debug(t1);
    }
#endif
}
