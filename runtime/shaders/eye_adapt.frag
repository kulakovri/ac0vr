#version 450
/* Eye adaptation (ps2_video.c expo_*): the frame times the exposure E, after the 3D cockpit and before the HUD, by the
   blend dst.rgb x (src.rgb + src.a) with src = (max(E - 1, 0), min(E, 1)): one full-screen triangle (cockpit_comp.vert),
   no texture read, RGB only (the game's destination alpha stays), the same E for both eyes (multiview: one draw). */
layout(push_constant) uniform PC { vec4 e; } pc;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(vec3(pc.e.x), pc.e.y);
}
