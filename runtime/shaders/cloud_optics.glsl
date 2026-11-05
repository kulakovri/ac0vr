/* cloud_optics.glsl - optical phenomena as rare rewards (ACZ_VR_OPTICS, bug #30; the C side is
   runtime/src/ps2_clouds_optics.c, which fills CloudFrame opt[] once per command buffer). Included by
   cloud_common.glsl; used by the cloud composite (cloud_comp.frag) and the optics probe (cloud_optics.comp).

   Each phenomenon is a cheap angular lobe evaluated per pixel in the composite pass that already runs (no pass of its
   own, no screen-space history): directions at infinity are the same in both eyes, so the halo and the rainbow sit at
   infinity as in nature; the glory and the Brocken spectre are centred per eye on the antisolar point OF THE CLOUD
   SURFACE at distance D (the eye's own shadow), so they lie on that surface in stereo. Colours: the game's sunlit
   cloud colour (C1 lit, slot-0 space) times per-channel lobes (R, G, B at about 630, 540, 460 nm).
   - 22 deg halo (+ parhelia at a low sun, + a faint 46 deg halo with ACZ_VR_OPTICS_HALO=2): ice crystals in a thin
     veil. Weight per pixel = the
     single-scattering profile of the pixel's own optical depth tau = -ln T (tau e^(1 - tau): 0 in clear sky, 1 at
     tau = 1, gone in thick cloud) x the ice share at the cloud's altitude (ACZ_VR_OPTICS_HALO_PHYS, bug #72: cloud
     at -20 C and colder, full at -35 C; before: above the freezing level, everywhere in the snow missions) x the
     sun's own light on that ice (PHYS: the cloud shadow map there) x the sun's visibility through the clouds from the
     eye (the probe; CPU-smoothed). Sharp red inner edge (minimum deviation 21.7 / 22.0 / 22.4 deg), white outward
     fade.
   - Glory: backscatter rings of cloud droplets around the antisolar point, J0(k a theta)^2 per channel (van de Hulst's
     axial-ray approximation) for droplets of radius a (ACZ_VR_OPTICS_DROP_UM, 10 um: first red ring ~2 deg, rings to
     ~5 deg), smoothed into a plain glow by the size spread; on the cloud surface the probe found below the aircraft
     (opacity of the pixel, sunlit top).
   - Brocken spectre: the aircraft's shadow on that surface. Seen from the cockpit (on the shadow's own line) the
     shadow is the aircraft's silhouette projected along the sun ray onto the plane at distance D, blurred by the sun's
     0.53 deg disc (a constant angle: thin parts fade with distance, everything beyond ~1.5-3 km is a faint smudge).
     The planform is five blurred parallelograms (fuselage, wings, tailplane, fin) the CPU projects from the aircraft's
     attitude; in the shadow the cloud keeps its own (palette) colour, darkened by the share of the direct sun it
     loses (the game's palette draws the cloud side facing away from the sun near DARK already, so a pull toward an
     ambient colour would not read).
   - Rainbow: primary 42 deg (red outside) and secondary 51 deg (reversed, 0.43x) around the antisolar point, a faint
     glow inside the primary and Alexander's dark band between; behind clouds and terrain: its strength is the rain's
     optical depth along the pixel's ray (the stage's precipitation volume below its band, up to the scene distance)
     x the sun on that rain (the cloud shadow map at the rain's mid-path) x the cloud in front of it.
   CloudFrame opt[] (vec4; all zero = everything off):
     0  x halo, y glory, z spectre, w rainbow strength (CPU gates x fades x ACZ_VR_OPTICS_K; 0 = off)
     1  xyz antisolar direction a (= -sun), w D: distance (m) of the cloud surface along a from the centre eye (0 none)
     2  rgb light colour (slot-0), w the halo's weight of the sun's light on its ice (the cloud shadow map at the
        pixel's cloud; 0 = none, ACZ_VR_OPTICS_HALO_PHYS=0)
     3  x ice level (m of altitude; below it no ice; full ice at 18.w), y 46 deg halo weight, z parhelion weight, w sun
        elevation (deg)
     4  x rain extinction (1/m), y rain band low (m: full below), z high (m: none above), w longest rain path (m)
     5  x 2 pi a / 0.54 um (glory), y the sun disc's blur (rad, box half-width), z owner of the surface (1 the near
        composite, 0 the far composite), w test bits (ACZ_VR_OPTICS_TEST: 1 halo, 2 glory, 4 spectre, 8 rainbow)
     6  xyz e1, 7 xyz e2: the shadow plane's axes (unit, perpendicular to a); 6.w the parts' bounding radius (m)
     8..17  five silhouette parts, two vec4 each: (centre x, y in the plane (m), row 0 of the inverse of the matrix of
            the half-extent vectors), (row 1, 1 / |half-extent A|, 1 / |half-extent B|)
     18 x parhelion azimuth offset from the sun (deg), y cos(sun elevation), z the spectre's darkening at full cover,
        w the altitude of full halo ice (m; the ice share ramps from 3.x to here)
     19, 20 xyz: the left / right eye's direction to the antisolar point of the surface, normalize(a D - eye)
     21, 22 the left / right eye's screen box of the glory / spectre cone (target pixels x0, y0, x1, y1): the near
            composite (glory and spectre on near cloud only) skips every pixel outside it */
#ifndef CLOUD_OPTICS_GLSL
#define CLOUD_OPTICS_GLSL

#define OPT_RAD2DEG 57.29578
bool cl_opt_any() { return any(greaterThan(cl.opt[0], vec4(0.0))); }
int cl_opt_test() { return int(cl.opt[5].w + 0.5); }

/* 22 deg halo, 46 deg halo, parhelia (th: degrees from the sun; d: the pixel's direction) */
vec3 opt_halo_lobe(float th, vec3 d) {
    vec3 e = vec3(21.7, 22.0, 22.4);
    vec3 v = smoothstep(e - 0.35, e + 0.35, vec3(th)) * exp(-max(vec3(th) - e, 0.0) * (1.0 / 2.2));
    if (cl.opt[3].y > 0.0) {
        vec3 e2 = vec3(45.7, 46.1, 46.6);
        v += cl.opt[3].y * smoothstep(e2 - 0.6, e2 + 0.6, vec3(th)) * exp(-max(vec3(th) - e2, 0.0) * 0.2);
    }
    if (cl.opt[3].z > 0.0) {                 /* parhelia: on the parhelic circle (the sun's elevation), left and right */
        float vv = asin(clamp(d.y, -1.0, 1.0)) * OPT_RAD2DEG - cl.opt[3].w;
        if (abs(vv) < 4.0) {
            vec3 s = cl.sun.xyz;
            float dph = atan(abs(d.x * s.z - d.z * s.x), d.x * s.x + d.z * s.z) * OPT_RAD2DEG;
            float u = (dph - cl.opt[18].x) * cl.opt[18].y;      /* angular distance outward from the inner edge */
            vec3 du = vec3(-0.3, 0.0, 0.4);
            v += cl.opt[3].z * smoothstep(du - 0.3, du + 0.3, vec3(u)) * exp(-max(vec3(u) - du, 0.0) * (1.0 / 2.5))
                 * exp(-vv * vv);
        }
    }
    return v;
}

/* J0(x)^2 (series to x = 3, the asymptotic form beyond) */
float opt_j0sq(float x) {
    float j;
    if (x <= 3.0) {
        float x2 = x * x;
        j = 1.0 + x2 * (-0.25 + x2 * (1.0 / 64.0 + x2 * (-1.0 / 2304.0 + x2 * (1.0 / 147456.0 - x2 / 14745600.0))));
    } else j = sqrt(0.63662 / x) * cos(x - 0.785398 + 0.125 / x);
    return j * j;
}
/* glory rings (th: radians from the antisolar point of the surface); colours a little more saturated than the plain
   per-channel sum (a narrow droplet spectrum), faded out by 7 deg */
vec3 opt_glory_lobe(float th) {
    vec3 x = th * cl.opt[5].x * vec3(0.857, 1.0, 1.174);
    vec3 v;
    for (int c = 0; c < 3; c++) {
        float j = opt_j0sq(x[c]), sm = min(1.0, 0.31831 / max(x[c], 0.3)), s = smoothstep(4.0, 22.0, x[c]);
        v[c] = pow(mix(j, sm, s), 0.5);
    }
    float m = (v.r + v.g + v.b) * (1.0 / 3.0);
    return max(vec3(m) + (v - vec3(m)) * 1.6, vec3(0.0)) * (1.0 - smoothstep(0.075, 0.12, th));
}

/* a box-blurred unit interval [-1, 1] at x, blur half-width e (in the same units) */
float opt_box(float x, float e) {
    return e < 1.0e-4 ? step(abs(x), 1.0) : (clamp(x + 1.0, -e, e) - clamp(x - 1.0, -e, e)) / (2.0 * e);
}
/* the spectre: the share of the sun's disc the aircraft covers, seen along pixel direction d of this eye on the
   plane at distance D along a (0..1) */
float opt_spectre(vec3 d, vec3 a, float D, int eye) {
    vec3 E = cl.eye_m[eye].xyz;
    float t = (D - dot(E, a)) / max(dot(d, a), 1.0e-3);
    vec3 X = E + t * d;
    vec2 q = vec2(dot(X, cl.opt[6].xyz), dot(X, cl.opt[7].xyz));
    float b = D * cl.opt[5].y;
    float rb = cl.opt[6].w + 2.0 * b;
    if (dot(q, q) > rb * rb) return 0.0;
    float cov = 0.0;
    for (int k = 0; k < 5; k++) {
        vec4 p0 = cl.opt[8 + 2 * k], p1 = cl.opt[9 + 2 * k];
        vec2 r = q - p0.xy;
        cov = max(cov, opt_box(dot(p0.zw, r), b * p1.z) * opt_box(dot(p1.xy, r), b * p1.w));
    }
    return cov;
}

/* the rainbow lobes (th: degrees from the antisolar point) */
vec3 opt_bow_lobe(float th) {
    vec3 t1 = vec3(42.2, 41.4, 40.6), t2 = vec3(50.5, 51.6, 52.8), wt = vec3(1.0, 0.8, 0.55);
    vec3 d1 = vec3(th) - t1, d2 = vec3(th) - t2;
    vec3 p = wt * exp(-(d1 * d1) / mix(vec3(0.81), vec3(0.25), step(vec3(0.0), d1)));
    vec3 s = wt * exp(-(d2 * d2) / mix(vec3(0.36), vec3(1.69), step(vec3(0.0), d2)));
    float gin = 0.10 * smoothstep(37.0, 40.5, th) * (1.0 - smoothstep(40.5, 42.0, th));
    return p + 0.43 * s + vec3(gin);
}
/* the antiderivative of the rain profile g(y) = clamp((hi - y) / (hi - lo), 0, 1) over altitude y */
float opt_rain_int(float y, float lo, float hi) {
    return y <= lo ? y : y >= hi ? 0.5 * (lo + hi) : y - (y - lo) * (y - lo) / (2.0 * (hi - lo));
}

/* Halo, glory and spectre on cloud content c (premultiplied rgb, transmittance a) seen along d by this eye.
   cd: the content's distance (m; the far map's mean distance, CL_DEPTH_NONE when unknown), near: 1 in the near
   composite (the probe says which composite owns the surface below), 0 in the far one. */
vec4 cl_optics_cloud(vec4 c, vec3 d, float cd, int eye, int near) {
    int tf = cl_opt_test();
    float T = c.a;
    vec3 L = cl.opt[2].rgb;
    float cs = near == 1 ? 0.0 : dot(d, cl.sun.xyz);       /* (the halo on the far content only) */
    /* only where a lobe can add half a level: the 22 deg ring and its outward fade (21 .. 30 deg), the 46 deg ring
       (44 .. 52 deg, opt-in), the parhelia out to their tails (opt[18].x + 12 deg); and only through cloud */
    bool hz = cs > 0.866 && cs < 0.934
              || (cl.opt[3].y > 0.0 && cs > 0.616 && cs < 0.719)
              || (cl.opt[3].z > 0.0 && cs < 0.94 && cs > cos((cl.opt[18].x + 12.0) / OPT_RAD2DEG));
    if (cl.opt[0].x > 0.0 && hz && (T < 0.999 || (tf & 1) != 0)) {
        float tau = -log(max(T, 1.0e-4));
        float w = (tf & 1) != 0 ? 1.0 : tau * exp(1.0 - tau);
        /* the cloud's altitude along the ray (the far map's mean distance; inside the band, which a long slanted
           mean distance can overshoot) */
        float dist = cd < CL_DEPTH_NONE ? cd : 300.0;
        float alt = clamp(cl.origin.y + d.y * dist, cl.band.x, cl.band.y);
        float ice, sdist = dist;
        if ((tf & 1) != 0) ice = 1.0;
        else if (cl.opt[2].w > 0.0 && abs(d.y) > 0.05) {
            /* ACZ_VR_OPTICS_HALO_PHYS: a deep layer is cold only near its top -- the ice share is the mean of the
               ice ramp over the ray's stretch through the band (up: eye .. top; down: base .. eye), not at the mean
               distance's single altitude; the sun on the ice is looked up in the middle of the stretch's cold part */
            float e = cl.origin.y, a0 = cl.opt[3].x, a1 = cl.opt[18].w, lo, hi;
            if (d.y > 0.0) { lo = clamp(e, cl.band.x, cl.band.y); hi = cl.band.y; }
            else { lo = cl.band.x; hi = clamp(e, cl.band.x, cl.band.y); }
            if (hi - lo > 1.0) {
                float t0 = clamp((lo - a0) / (a1 - a0), 0.0, 1.0), t1 = clamp((hi - a0) / (a1 - a0), 0.0, 1.0);
                float i0 = (a1 - a0) * t0 * t0 * t0 * (1.0 - 0.5 * t0) + max(lo - a1, 0.0);   /* antiderivative */
                float i1 = (a1 - a0) * t1 * t1 * t1 * (1.0 - 0.5 * t1) + max(hi - a1, 0.0);
                ice = (i1 - i0) / (hi - lo);
                float hm = 0.5 * (clamp(a0, lo, hi) + hi);
                sdist = max((hm - e) / d.y, 0.0);
            } else ice = smoothstep(a0, a1, hi);
        } else ice = smoothstep(cl.opt[3].x, cl.opt[18].w, alt);
        float k = cl.opt[0].x * w * ice;
        /* ACZ_VR_OPTICS_HALO_PHYS: the crystals scatter the sun's own beam -- the shadow map's transmittance at that
           ice (the ragged base of a thick deck gets none, a thin sunlit veil most) */
        if (cl.opt[2].w > 0.0 && k > 1.0e-3 && (tf & 1) == 0) k *= mix(1.0, cl_sun_shadow(d * sdist), cl.opt[2].w);
        if (k > 1.0e-3) c.rgb += L * k * opt_halo_lobe(acos(cs) * OPT_RAD2DEG, d);
    }
    float D = cl.opt[1].w;
    if ((cl.opt[0].y > 0.0 || cl.opt[0].z > 0.0) && D > 0.0 && int(cl.opt[5].z + 0.5) == near) {
        vec3 a = cl.opt[1].xyz;
        vec3 ae = cl.opt[19 + eye].xyz;                    /* this eye's antisolar point on the surface (CPU) */
        float ca = dot(d, ae);
        /* within 7 deg (the glory), or within the spectre's extent (a close surface: the shadow is larger) */
        float cs_reach = cl.opt[0].z > 0.0 ? cos(min((cl.opt[6].w + 2.0 * D * cl.opt[5].y) / D, 1.2)) : 1.0;
        if (ca > min(0.9925, cs_reach)) {
            bool forced = (tf & 6) != 0;
            float op = forced ? 1.0 : 1.0 - T;
            bool below = forced || near == 1 || cd >= CL_DEPTH_NONE || d.y * cd < -10.0;
            if (op > 0.02 && below) {
                if (cl.opt[0].y > 0.0 && ca > 0.9925)
                    c.rgb += L * (cl.opt[0].y * op) * opt_glory_lobe(acos(min(ca, 1.0)));
                if (cl.opt[0].z > 0.0) {
                    float cov = opt_spectre(d, a, D, eye) * cl.opt[0].z;
                    if (cov > 0.0) {
                        c.rgb *= 1.0 - clamp(cov * cl.opt[18].z, 0.0, 1.0);    /* the direct sun's share taken away */
                        if (forced) c.a *= 1.0 - 0.5 * clamp(cov, 0.0, 1.0);   /* (look test on clear sky) */
                    }
                }
            }
        }
    }
    return c;
}

/* The rainbow light (premultiplied, to add after the far content's scene test) along d: sd = the scene distance (m,
   CL_INF for sky), cd / T = the far content's distance and transmittance at this pixel. */
vec3 cl_optics_bow(vec3 d, float sd, float cd, float T) {
    float ca = -dot(d, cl.sun.xyz);
    if (cl.opt[0].w <= 0.0 || ca < 0.545 || ca > 0.799) return vec3(0.0);   /* 37 .. 57 deg from the antisolar point */
    vec3 lobe = opt_bow_lobe(acos(ca) * OPT_RAD2DEG);
    /* where even full rain and sun would add less than half a level (Alexander's band, the fading glows): nothing */
    float lmax = cl.opt[0].w * max(cl.opt[2].r, max(cl.opt[2].g, cl.opt[2].b));
    if (max(lobe.r, max(lobe.g, lobe.b)) * lmax < 0.002) return vec3(0.0);
    int tf = cl_opt_test();
    float y0 = cl.origin.y, dy = d.y, lo = cl.opt[4].y, hi = cl.opt[4].z;
    float tmax = min(sd, cl.opt[4].w), path, tin = 0.0;
    if (abs(dy) > 1.0e-3) path = (opt_rain_int(y0 + dy * tmax, lo, hi) - opt_rain_int(y0, lo, hi)) / dy;
    else path = clamp((hi - y0) / (hi - lo), 0.0, 1.0) * tmax;
    if (y0 > hi && dy < 0.0) tin = (y0 - hi) / -dy;
    float vis = 1.0 - exp(-cl.opt[4].x * max(path, 0.0));     /* (forced: the CPU's heavy rain everywhere below) */
    if (vis < 0.003) return vec3(0.0);
    float trep = min(tin + 0.5 * min(max(tmax - tin, 0.0), 1500.0), tmax);
    float sun = (tf & 8) != 0 ? 1.0 : cl_sun_shadow(d * trep);
    float kf = cd < CL_DEPTH_NONE ? smoothstep(1.2 * trep, 0.8 * trep, cd) : 0.0;
    float k = cl.opt[0].w * vis * sun * mix(1.0, T, kf);
    return k > 1.0e-3 ? cl.opt[2].rgb * k * lobe : vec3(0.0);
}

#endif
