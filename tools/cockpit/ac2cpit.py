"""ac2cpit.py -- convert a FlightGear cockpit (AC3D models assembled by PropertyList XML) into the runtime's cockpit
file (.cpit) for the native VR cockpit overlay.

Usage: python -I ac2cpit.py ROOT_XML EYE_X EYE_Y EYE_Z OUT.cpit [--skip REGEX] [--subdiv METRES] [--tex-dir DIR]
                              [--extra MODEL.ac OBJECT_REGEX]...
  ROOT_XML     the aircraft's interior XML (e.g. Models/Interior/interior.xml)
  EYE_*        pilot eye in FlightGear model coordinates (metres; x aft, y right, z up), e.g. from the
               "Cockpit View" in Systems/views.xml: z-offset-m -> x, x-offset-m -> y, y-offset-m -> z
  --subdiv M   split triangles until no edge is longer than M metres (finer per-vertex ambient occlusion; 0 = off)
  --tex-dir D  prefer D/<texture file name> (e.g. upscaled copies) over the model's own texture; repeatable: the
               first folder that has the file wins (a repainted texture in front of the upscaled set)
  --max-tex N  halve textures until no side exceeds N texels (2048; the runtime uploads up to 32 MB at once)
  --max-tex-for R N   the same limit for textures whose model path matches R (e.g. 'Instruments/' 1024)
  --max-tex-strip R N  textures whose model path matches R (long thin tapes) may keep up to N texels along their long
               side while the short side stays within --max-tex: halving a 124 x 48912 altitude tape to 2048 left it 3
               texels wide (ck_polish 2026-10-28, a cockpit's PFD pages); files built without it
               do not change
  --hide-obj R drop the objects whose names match R (e.g. an inner copy of the canopy glass)
  --flatten-obj R  the objects whose names match R are pressed flat onto their own plane (through their vertices'
               mean, normal = their faces' area-weighted normal), so their normals agree: a rear-view mirror glass
               cut out of a curved canopy arch (the Su-37's pads bend with the arch: up to 10.7 mm off a plane,
               face normals 15 deg off it on average) reflects as a flat mirror instead of a warped one
  --drop-pieces R@X0,X1,Y0,Y1,Z0,Z1  in the objects whose names match R, leave out every connected piece (faces sharing
               vertices) that lies wholly inside that box (eye frame metres: X right, Y up, Z back): e.g. a harness
               modelled round a pilot figure the cockpit view leaves out (the F-35A's straps in SeatandStraps).
               Repeatable; files built without it do not change (ck_fit 2026-10-28)
  --glass-obj R[=A]  objects whose names match R are glass (flag 1) with alpha A (default 0: no tint, only the
               runtime's reflection, canopy water, mist and ice on it; at most 0.15) and no texture: canopies
               FlightGear draws through an effect over a fully transparent texture (the F-14's tranbg.png) or with a
               translucent texture the runtime's alpha cut drops (the Su-27's canopy.glass). Without such glass the
               cockpit has no canopy for the in-cloud water (bug #71). Files built without it do not change
  --mat-obj R=KIND[@TILE][:R,G,B]  untextured objects whose names (or, with --group-names, groups) match R get a
               generated tileable surface texture (mat_texture: KIND paint = matte paint, fabric = woven cloth, rubber =
               pebbled coating, riveted = paint on sheet metal with a seam and a rivet row along two tile edges (ck_polish
               2026-10-28); a few per cent of grain and mottling, deterministic) on texture coordinates projected
               from each face's dominant axis in the eye frame, TILE metres a repeat (0.25); the material colour is
               divided by the texture's mean so the average stays the material's; with :R,G,B a material darker than
               that colour takes it (FlightGear black paint with ambient 0 is a void here). Opaque materials only (glass
               stays clear); first match wins; repeatable. Untextured cockpit parts looked flat and plastic: the seats
               and tubs of the F/A-18C, Typhoon, YF-23A, MiG-21bis, A-10A (ck_audit 2026-10-28). Files built without it
               do not change
  --mat-box R=KIND[@TILE]:R,G,B@X0,Y0,Z0,X1,Y1,Z1[@NX,NY,NZ,D][#<N|#>N]  the faces of objects matching R (as --mat-obj)
               whose centre lies in this box (eye frame metres: X right, Y up, Z back; corners in any order) and, with
               the last group, whose normal n has n . (NX, NY, NZ) >= D, take the generated KIND texture (box-projected,
               TILE metres) and the colour R,G,B (set, not lifted; the texture's mean divided out); #<N / #>N: only faces of
               connected parts (shared vertices) with fewer / more than N faces (a harness strap on its cushion): the regions of a
               one-material part, e.g. the cushions and harness of a seat modelled as one grey object (the FGUK F/A-18C's
               SJU-5/6; ck_seats 2026-10-28). First matching box wins, before --mat-obj; opaque materials only; textured
               objects too (their texture is replaced on those faces). Repeatable; files built without it do not change
  --tex-obj R=P  objects whose names (or, with --group-names, groups) match R take texture P (relative to
               --aircraft-root): the texture a livery's material animation (texture-prop) gives them in FlightGear
  --spec-ac R=S  specular S for the parts of AC files whose path matches R (display pages, which FlightGear lights by
               emission: a material with specular 1 and shininess 0 put a grey sheen over the whole screen)
  --spec-obj R=S[,SHI]  specular S (and shininess SHI) for the objects whose names match R, after --spec-ac (the
               first match wins): a flocked coaming or a fabric seat cushion is matte where Flight Simulator's material
               gave every part the same sheen (2026-10-30). Files built
               without it do not change
  --ambient-obj R  objects whose names match R take their material's ambient colour where it is brighter than the
               diffuse one (FlightGear lights both; for near-black diffuse instruments with a grey ambient)
  --extra F R  also take the objects of AC3D file F (same FlightGear frame as ROOT_XML, e.g. the exterior model)
               whose names match R: the fuselage closes the cockpit tub, wings and nose show around it
  --const P=V  this aircraft's property P reads the constant V (like CONSTS; e.g. its electrical bus names); a V
               that is no number is a string (for --const-selects, e.g. an MFD's selected page)
  --const-selects  select animations whose conditions read only constants and unmapped properties (0 / "") are
               decided here: a false one hides its objects and named sub-models as a constant <false/> does
               (FlightGear-style string comparisons included: MFD pages chosen by name)
  --sort-tables  order rotate interpolation tables by input as SimGear does (a table written high to low otherwise
               reads its first entry: the Tornado's ILS needle stood at 30 degrees); off by default, older builds
               keep their files
  --ear-clip  concave polygons triangulated by ear clipping in their plane, as OSG tessellates them for FlightGear (a
               fan from the first vertex fills a concave polygon's notches: the F-15C's TEWS bezel step, three ring
               pieces 6 mm in front of the screen, covered 52 % of the scope's opening with stray triangles of atlas
               texels, so the picture showed as a shifted, cut shape; pt_gaze 2026-10-29); convex polygons keep the fan
               (their triangles unchanged); a polygon the clipper cannot finish (self-intersecting) keeps the fan; off
               by default, older builds keep their files. Since ck_earclip 2026-10-29 (bug #208, all the cockpit
               build scripts: CPIT_EARCLIP) only polygons whose fan folds over are clipped (a fan none of whose
               triangles turns against the polygon covers it exactly: kept, e.g. a fan from the reflex corner); a
               vertex on an ear's edge blocks the ear (a degenerate polygon whose edge runs back over another -- the
               Gripen FG floor, the Tornado -- took an ear across the notch: up to three times the polygon's area); a
               clip whose last triangle turns inside out, or with more 3D area than the fan (a bent polygon), keeps
               the fan
  --prop P=Q[*S]  this aircraft's property P reads the mapped property Q times S (another name or unit of a value
               the runtime has, e.g. 'P=instrumentation/vertical-speed-indicator/indicated-speed-fpm*0.016667'
               for a vertical speed P in feet per second)
  --fold-selects  select conditions the plain parser cannot express (nested and/or/not, two properties compared,
               string values) are folded: parts that depend on no mapped property become constants (unmapped
               properties read 0), the rest flattens into one and/or of terms where possible; without it such parts
               stay always visible (radar blips, warning lamps). Opt-in, so earlier builds do not change
  --texrotate  texmultiple / texrotate animations (texture turned by a property, e.g. an attitude display's horizon
               rolled and shifted): the translations become textranslate animations, a rotation becomes a rotation
               of the part about the point showing the rotation centre -- only for disc-shaped parts, where that is
               the same picture (others stay still, as without the option). Opt-in
  --anim-submodels  an XML's animations also reach its sub-models, as in SimGear: objects of .ac sub-models (and
               deeper) by name, and a whole sub-model by its <name>; constant-true selects are not passed on, unused
               ones not written (the F-22's Interior.xml animates the objects of its protocp.ac sub-model)
  --axis-objects  rotation axes as SimGear reads them: <axis><object-name> (a two-vertex line object: centre its
               middle), with no <axis> an object "<object-name>-axis" if the model has one, or <axis><x1-m>..<z2-m>
               (without this the axis of such an animation is 0 and the part is scaled by cos(angle))
  --axis-fg-sense  an axis object's direction runs from its line's second point (in the SURF refs order) to its
               first, as FlightGear turns the parts: the F-14's airspeed, altimeter and VSI pointers then go
               clockwise, its stick forward with push and right with right roll, its throttles forward with power
               (refs 1 0 and 0 1 lines alike; the F-22's ENG page needles too). Without it (as before) first ->
               second: all of these backwards (ck_polish 2026-10-31, bug #258). Opt-in, so earlier builds do not
               change; also turns the controls' axis objects that --controls reads without --axis-objects
  --axis-flip R  axis objects whose names match R point the other way: a line drawn the wrong way round (the F-14's
               stick-roll-axis, whose -20 deg was meant to undo the handle's roll on the linkage plate stick-roll:
               with the line as drawn the plate swung 40 deg out of the stick foot, in FlightGear too)
  --axis-sg-order  an axis object's direction as SimGear's LineCollector orders the line's two ends (animation.cxx,
               addLine): by default (FlightGear <= 2020.3 and still) from the end with the larger x (AC3D file frame,
               x aft) to the smaller, i.e. forward, and from the second vertex to the first where both have the same
               x; with <axis><order-by-xyz/> or a model's <defaults><axis-animation-vertex-order-xyz/> (passed on to
               its sub-models; FlightGear 2024.1) towards the smaller by x, then y, then z (0.01 mm apart);
               <order-by-x/> the default order again; <swap-axis-direction/> reverses. Supersedes --axis-fg-sense,
               which is this rule only for lines whose second vertex lies aft of (or level with) the first: of the
               F-14's 407 lines 119 run the other way -- with --axis-fg-sense its VDI pitch ladder and standby attitude
               drum turned against the bank, its HSD rose and BDHI card against the heading, and the stick's linkage
               plate needed --axis-flip; of the F-22's 45, 34 run against the first -> second order of --axis-objects
               alone (its six ENG page needles turned anticlockwise) (ck_axes 2026-10-31, bug #262). Opt-in, so earlier
               builds do not change; --axis-flip still applies after it
  --range     SimGear's range animations (min-m / max-m): the objects and AC3D groups one names are left out when
               the design eye is outside that range of their centre (FlightGear's LOD; the F-14's pilot and RIO
               figures mm_pilot / mm_rio are drawn only from 2 m away, never from the cockpit). Opt-in
  --group-names  an object also answers to the names of the AC3D groups it is in (SimGear selects / animates a group
               with everything in it; a group's animations wrap its objects' own; constant-false selects hide it)
  --tex-paths  a texture path that does not exist: an absolute path of the modeller's machine that names this aircraft
               (".../Aircraft/F-23C/Models/...", the folder name of --aircraft-root) resolves under --aircraft-root,
               else the file name next to the model is used (OSG looks there too)
  --controls   the pilot's controls move (ps2_video.c ACZ_VR_COCKPIT_CONTROLS): controls/flight/elevator, aileron,
               rudder and controls/engines/engine[n]/throttle are mapped (CONTROL_PROPS; FlightGear conventions, the
               throttle as the runtime's lever 0 idle .. 0.95 MIL .. 1 afterburner) and their "translate" animations
               (sliding levers, pedals) are kept as runtime animation type 3; files built without it do not change
  --gauge-translate  "translate" animations of every mapped property are kept (runtime type 3), not only the
               controls': instrument tapes and pointers that slide (the MiG-31's engine RPM / temperature tapes, ck_mig31
               2026-10-28); files built without it do not change
  --throttle-mil V  this aircraft's throttle animations reach military power at FlightGear throttle V (the F-14's
               table has MIL at 0.91 and afterburner beyond): the runtime's lever 0..0.95..1 is mapped onto 0..V..1
  --add-model XML@X,Y,Z[,H,P,R[,S]]  also assemble this FlightGear model XML (with its animations) at that offset in
               the root model's frame (metres, degrees: as a <model><offsets>), e.g. a stick and throttle a cockpit lacks
               (the T-38 has none: the FlightGear F-5E's stick and power lever); S scales it about its origin (as a
               FlightGear overlay scale; e.g. a display's live face fitted to another model's larger screen)
  --add-glass NAME=X,Y,Z;X,Y,Z;X,Y,Z;X,Y,Z  a flat clear glass quad (two triangles, two-sided, alpha 0, no texture, no
               shine) at these four corners (FlightGear frame metres, the EYE_* frame, in order around it), named NAME:
               a HUD combiner a model has the frame of but no glass for (ps2_video.c ACZ_VR_HUD_GLASS takes it by
               name, e.g. HUD_glass); repeatable; files built without it do not change
  --hands F.ac  gloved hands (tools/cockpit/hands_bake.py: objects hand_<L|R>_<grip|open|point|fist>[_cuff] in the
               OpenXR grip frame, metres; a cuff shares its glove's slot) as runtime matrix slots (animation type 4,
               ps2_video.c cpit_hands, ACZ_VR_HANDS); mesh flag 16 (placed at run time: cpit_bake leaves them out of
               the occlusion)
  --hands-hide R  the model's own hands (a FlightGear pilot's, objects matching R): mesh flag 32, hidden while the gloves
               show (ACZ_VR_HANDS)
  --hand S=R[@X,Y,Z[,YAW,PITCH,ROLL]]  hand S (L or R) holds the control whose object name matches R (an animated one
               first) and follows its animations: the grip frame's origin at X,Y,Z (eye frame metres; default: on the
               object's axis 5 cm below its top), turned by YAW (about up), PITCH (about right: + rakes the handle
               back), ROLL (about back: + leans it left) from upright (grip X = right, -Z = up); without --hand S the
               hand has no pose on a control (only the free Touch poses)
  --screen R=ID[,LIFT]  objects whose names match R are display screens with id ID (1-255; the runtime shows the
               game's HUD radar on screen 1 and its weapon block on screen 2, ps2_video.c mfd_*; 3 a map display, 4 a
               round radar scope, 5 the wingman commands, 6 a radar display beside a map display, 7..255 no block --
               ps2_video.c MFD_SCR_*): kept even with a
               transparent or placeholder texture (FlightGear canvas targets such as the F-16's MFDimage1), opaque, a
               dark "off" texture, texture coordinates stretched to 0..1 over the object's own range and turned so
               that u goes to the pilot's right and v down the screen, and the id stored in the mesh flags (bits
               8-15); LIFT metres moves the screen towards the eye along its face (a bezel face FlightGear culls as a
               back face covers the F-15's MPCD here: every face is drawn). Repeatable; files built without it do not
               change
  --screen-eye R  screens whose names match R take texture coordinates from the direction each point is seen in from
               the design eye (azimuth, elevation), not their own stretched to 0..1: a face that wraps round the pilot
               (a console display) shows the picture as from the seat. Its own atlas island bends with the
               face (the picture's top rows fell on its wings only), and coordinates in the face's plane mix the
               wrap's depth into the height (the picture came out at half the display's height). Repeatable; files
               built without it do not change (ck_look 2026-10-30)
  --opaque-tex R  textures whose path matches R are made opaque (alpha 255): a material that ignores its texture's
               alpha (a capsule model: the shader reads t4.rgb only; 6 % of the BC3 atlas holds alpha < 128,
               which the runtime's alpha cut would punch out). Repeatable; files built without it do not change
               (ck_look 2026-10-30)
Output frame: X right, Y up, Z back (OpenXR view axes), origin at the eye, metres.
Normals are smoothed within each object up to its AC3D crease angle. The ambient occlusion is 1 here;
tools/cockpit/cpit_bake.c bakes it into the file afterwards.

File layout (little endian):
  "ACZCPIT3" u32 ntex u32 nmesh u32 nanim
  per texture: u32 w, u32 h, w*h*4 RGBA bytes (top row first)
  per animation (FlightGear "rotate"): u32 prop, f32 center[3], f32 axis[3] (eye frame), f32 factor, f32 offset_deg,
               u32 ninterp, ninterp * (f32 value, f32 degrees)
               ("textranslate", drum counters: prop | 0x100, center = step, scroll, bias; axis = texture x, y, 0;
               factor, offset; the texture moves by (stepped value * factor + offset) along the axis)
               ("select": prop = 0x200, center[0] = 1 for "or" (else "and"), center[1] = 1 negates the result; the
               table holds the terms as (code, value): code = op | prop << 8 | negate << 16, op 1 <, 2 <=, 3 >, 4 >=,
               5 ==, 6 !=, 7 property true; prop 255 = not mapped (reads 0, as a missing FlightGear property))
               ("translate", --controls: prop | 0x800, axis = unit direction (eye frame); the part moves by
               table(value) or value * factor + offset metres along it)
               (a hand slot, --hands: prop = 0x1000 | hand << 4 | pose (hand 0 L, 1 R; pose 0 grip, 1 open, 2 point,
               3 fist), center = the grip frame's origin, axis = its rotation vector (eye frame), offset = the control
               (1 stick, 2 throttle, 0 none), table = (index, 0) of the animations it follows on the control)
  per mesh:    char name[32], u32 tex (0xFFFFFFFF none), f32 rgba[4], u32 flags (1 = translucent (glass), 2 =
               two-sided, 16 = placed at run time (--hands), 32 = hidden while the gloves show (--hands-hide), bits 8-15
               = screen id of --screen), u32 anim[4] (applied outermost first; 0xFFFFFFFF
               none), f32 specular, f32 shininess, u32 ntri, ntri*3 vertices of f32 pos[3], f32 nrm[3], f32 uv[2],
               f32 ao
Animated properties (ids shared with the runtime, ps2_video.c CPIT_P_*): see PROPS.
Only reads the given files; no code from the downloaded data is executed.
"""
import math, os, re, struct, sys, xml.etree.ElementTree as ET, zlib

# ---------------------------------------------------------------- PNG decoding (8-bit, non-interlaced)

def png_rgba(path):
    d = open(path, 'rb').read()
    if d[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG: ' + path)
    pos, idat, plte, trns = 8, [], None, None
    w = h = depth = ctype = inter = 0
    while pos < len(d):
        n, t = struct.unpack('>I4s', d[pos:pos + 8]); body = d[pos + 8:pos + 8 + n]; pos += 12 + n
        if t == b'IHDR': w, h, depth, ctype, _, _, inter = struct.unpack('>IIBBBBB', body)
        elif t == b'PLTE': plte = body
        elif t == b'tRNS': trns = body
        elif t == b'IDAT': idat.append(body)
        elif t == b'IEND': break
    if depth != 8:
        raise ValueError('unsupported PNG (depth %d): %s' % (depth, path))
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(b''.join(idat))
    if not inter:
        return w, h, png_rows(raw, 0, w, h, ch, ctype, plte, trns)[0]
    # Adam7: seven sub-images, each filtered on its own, scattered into the full image
    out = bytearray(w * h * 4)
    p = 0
    for x0, y0, dx, dy in ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)):
        pw, ph = (w - x0 + dx - 1) // dx, (h - y0 + dy - 1) // dy
        if pw <= 0 or ph <= 0: continue
        sub, p = png_rows(raw, p, pw, ph, ch, ctype, plte, trns)
        for j in range(ph):
            y = y0 + j * dy
            for i in range(pw):
                o, q = (y * w + x0 + i * dx) * 4, (j * pw + i) * 4
                out[o:o + 4] = sub[q:q + 4]
    return w, h, bytes(out)


def png_rows(raw, p, w, h, ch, ctype, plte, trns):
    """unfilter h rows of w pixels from raw[p:] -> (RGBA bytes, position after them)"""
    stride = w * ch
    out = bytearray(w * h * 4)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[p]; line = bytearray(raw[p + 1:p + 1 + stride]); p += 1 + stride
        if f == 1:
            for i in range(ch, stride): line[i] = (line[i] + line[i - ch]) & 255
        elif f == 2:
            line = bytearray((p + q) & 255 for p, q in zip(line, prev))
        elif f == 3:
            for i in range(stride): line[i] = (line[i] + ((line[i - ch] if i >= ch else 0) + prev[i]) // 2) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0; b = prev[i]; c = prev[i - ch] if i >= ch else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        o = y * w * 4
        if ctype == 6: out[o:o + w * 4] = line
        elif ctype == 2:
            for c in range(3): out[o + c:o + w * 4:4] = line[c::3]
            out[o + 3:o + w * 4:4] = bytes((255,)) * w
        else:
            for x in range(w):
                if ctype == 0: r = g = b = line[x]; a = 255
                elif ctype == 4: r = g = b = line[2 * x]; a = line[2 * x + 1]
                else:
                    k = line[x]; r, g, b = plte[3 * k:3 * k + 3]; a = trns[k] if trns and k < len(trns) else 255
                out[o + 4 * x:o + 4 * x + 4] = bytes((r, g, b, a))
        prev = line
    return bytes(out), p

def sgi_rgba(path):
    """SGI .rgb/.rgba/.sgi (8 bit, verbatim or RLE, 1-4 channels) -> (w, h, RGBA bytes, rows top first)"""
    d = open(path, 'rb').read()
    magic, storage, bpc = struct.unpack_from('>HBB', d, 0)
    if magic != 474 or bpc != 1: raise ValueError('not an 8-bit SGI image')
    dim, w, h, zs = struct.unpack_from('>HHHH', d, 4)
    if dim < 3: zs = 1
    if dim < 2: h = 1
    chans = []
    if storage == 0:
        for z in range(zs):
            chans.append(d[512 + z * w * h:512 + (z + 1) * w * h])
    else:
        n = h * zs
        starts = struct.unpack_from('>%dI' % n, d, 512)
        for z in range(zs):
            ch = bytearray()
            for y in range(h):
                pos = starts[z * h + y]
                row = bytearray()
                while len(row) < w:
                    c = d[pos]; pos += 1
                    cnt = c & 0x7F
                    if not cnt: break
                    if c & 0x80: row += d[pos:pos + cnt]; pos += cnt
                    else: row += bytes([d[pos]]) * cnt; pos += 1
                ch += row[:w].ljust(w, b'\0')
            chans.append(bytes(ch))
    if zs == 1: chans = [chans[0], chans[0], chans[0], bytes([255]) * (w * h)]
    elif zs == 2: chans = [chans[0], chans[0], chans[0], chans[1]]
    elif zs == 3: chans = chans + [bytes([255]) * (w * h)]
    out = bytearray(w * h * 4)
    for c in range(4):
        src = chans[c]
        for y in range(h):                                   # SGI rows run bottom to top
            row = src[(h - 1 - y) * w:(h - y) * w]
            out[y * w * 4 + c:(y + 1) * w * 4:4] = row
    return w, h, bytes(out)


def dds_rgba(path):
    """DDS with DXT1 / DXT3 / DXT5 (top level only) or uncompressed 24/32-bit -> (w, h, RGBA bytes, rows top first)"""
    d = open(path, 'rb').read()
    if d[:4] != b'DDS ': raise ValueError('not a DDS file')
    h, w = struct.unpack_from('<II', d, 12)
    pf_flags, = struct.unpack_from('<I', d, 80)
    four = d[84:88]
    pos = 128
    out = bytearray(w * h * 4)
    if not (pf_flags & 4):                                   # uncompressed 32-bit, or 24-bit (opaque; the F-22)
        bits, rm, gm, bm, am = struct.unpack_from('<IIIII', d, 88)
        if bits == 24:
            px = [int.from_bytes(d[pos + 3 * i:pos + 3 * i + 3], 'little') for i in range(w * h)]
            am = 0
        elif bits == 32: px = struct.unpack_from('<%dI' % (w * h), d, pos)
        else: raise ValueError('DDS: %d-bit uncompressed not supported' % bits)
        sh = [(m & -m).bit_length() - 1 for m in (rm, gm, bm, am)]
        for i, v in enumerate(px):
            out[i * 4:i * 4 + 4] = bytes(((v >> sh[0]) & 255, (v >> sh[1]) & 255, (v >> sh[2]) & 255,
                                          ((v >> sh[3]) & 255) if am else 255))
        four = None                                          # rows as stored, flipped below like the DXT ones
    elif four not in (b'DXT1', b'DXT3', b'DXT5'): raise ValueError('DDS: %r not supported' % four)
    bsz = 8 if four == b'DXT1' else 16
    bw, bh = ((w + 3) // 4, (h + 3) // 4) if four else (0, 0)

    def c565(c):
        r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
        return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
    for by in range(bh):
        for bx in range(bw):
            blk = d[pos:pos + bsz]; pos += bsz
            cb = blk[bsz - 8:]
            c0, c1, idx = struct.unpack_from('<HHI', cb, 0)
            p0, p1 = c565(c0), c565(c1)
            if c0 > c1 or four != b'DXT1':
                pal = [p0 + (255,), p1 + (255,),
                       tuple((2 * a + b) // 3 for a, b in zip(p0, p1)) + (255,),
                       tuple((a + 2 * b) // 3 for a, b in zip(p0, p1)) + (255,)]
            else:
                pal = [p0 + (255,), p1 + (255,), tuple((a + b) // 2 for a, b in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
            alpha = None
            if four == b'DXT3':
                a64, = struct.unpack_from('<Q', blk, 0)
                alpha = [((a64 >> (4 * k)) & 15) * 17 for k in range(16)]
            elif four == b'DXT5':
                a0, a1 = blk[0], blk[1]
                ai, = struct.unpack_from('<Q', blk[2:8] + b'\0\0', 0)
                if a0 > a1: ap = [a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)]
                else: ap = [a0, a1] + [((5 - k) * a0 + k * a1) // 5 for k in range(1, 5)] + [0, 255]
                alpha = [ap[(ai >> (3 * k)) & 7] for k in range(16)]
            for k in range(16):
                x, y = bx * 4 + (k & 3), by * 4 + (k >> 2)
                if x >= w or y >= h: continue
                c = pal[(idx >> (2 * k)) & 3]
                o = (y * w + x) * 4
                out[o:o + 4] = bytes(c[:3] + ((alpha[k],) if alpha else (c[3],)))
    # FlightGear (OSG) uses DDS rows as stored, bottom row first in texture space, where PNGs are flipped on load:
    # flip so the model's texture coordinates land as in FlightGear
    row = w * 4
    out = b''.join(bytes(out[(h - 1 - y) * row:(h - y) * row]) for y in range(h))
    # pure magenta marks places FlightGear fills with live displays (canvas): a dark, switched-off screen instead
    out = bytearray(out)
    for i in range(0, len(out), 4):
        if out[i] > 230 and out[i + 1] < 40 and out[i + 2] > 230: out[i:i + 3] = bytes((20, 22, 24))
    return w, h, bytes(out)


# ---------------------------------------------------------------- --mat-obj surface textures (generated, tileable)

MAT_KINDS = ('paint', 'fabric', 'rubber', 'riveted')


def lum(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def mat_texture(kind, n=512):
    """a tileable grey texture (n x n RGBA) for an untextured part: 'paint' = matte paint (faint mottling and grain),
    'fabric' = woven cloth (seat cushions, straps: threads every 4 texels), 'rubber' = fine pebbled rubber / non-slip
    coating, 'riveted' = the matte paint on sheet metal: a panel seam along two edges of the tile (a groove with a lit
    lip) and a row of domed rivet heads beside each (one per 3/16 of the tile, 1/57 of it in radius: about 2 cm and
    4 mm across at TILE 0.11; ck_polish 2026-10-28, a fabricated seat headbox side). Subtle by design (a few per cent
    around the mean; the rivets and seams +-10..20 %); deterministic (an integer hash, no RNG state).
    Returns (n, RGBA bytes, mean texel value 0..1)."""
    def h(i, j, s):                                  # integer hash -> [-1, 1)
        x = (i * 374761393 + j * 668265263 + s * 2147483647) & 0xFFFFFFFF
        x = ((x ^ (x >> 13)) * 1274126177) & 0xFFFFFFFF
        return ((x ^ (x >> 16)) & 0xFFFF) / 32768.0 - 1.0

    def vnoise(f, s):                                # value noise with f x f cells, wrapping at the edges
        lat = [[h(i, j, s) for i in range(f)] for j in range(f)]
        out = [0.0] * (n * n)
        sc = f / n
        for y in range(n):
            fy = y * sc; j0 = int(fy); ty = fy - j0; ty = ty * ty * (3 - 2 * ty)
            r0, r1 = lat[j0 % f], lat[(j0 + 1) % f]
            base = y * n
            for x in range(n):
                fx = x * sc; i0 = int(fx); tx = fx - i0; tx = tx * tx * (3 - 2 * tx)
                i1 = (i0 + 1) % f; i0 %= f
                a_ = r0[i0] + (r0[i1] - r0[i0]) * tx
                b_ = r1[i0] + (r1[i1] - r1[i0]) * tx
                out[base + x] = a_ + (b_ - a_) * ty
        return out

    if kind in ('paint', 'riveted'):
        m, layers, grain = 0.90, ((4, 0.030), (16, 0.020), (64, 0.012)), 0.010
    elif kind == 'fabric':
        m, layers, grain = 0.86, ((8, 0.025), (32, 0.015)), 0.012
    else:
        m, layers, grain = 0.88, ((8, 0.025), (128, 0.045)), 0.012
    v = [m] * (n * n)
    for k, (f, amp) in enumerate(layers):
        nz = vnoise(f, 11 + k)
        for i in range(n * n): v[i] += amp * nz[i]
    for y in range(n):
        for x in range(n):
            i = y * n + x
            v[i] += grain * h(x, y, 97)
            if kind == 'fabric':                         # a plain weave: over / under every 4 texels
                wx, wy = (x % 4) / 3.0, (y % 4) / 3.0
                over = ((x // 4) + (y // 4)) % 2
                v[i] += 0.07 * ((math.sin(math.pi * (wy if over else wx)) * 0.5) - 0.32)
    if kind == 'riveted':                            # seams on the tile's x = 0 and y = 0 edges, rivet rows beside
        sw = max(2, n // 170)                        # groove half width (texels)
        for y in range(n):
            for x in range(n):
                d = min(x, n - x, y, n - y)          # texels from the nearest seam (the tile wraps)
                if d < sw: v[y * n + x] -= 0.26 * (1.0 - d / sw)
                elif d < 2 * sw: v[y * n + x] += 0.05   # the lit lip beside the groove
        rr, off, pitch = n / 57.0, n * 0.055, n * 3 / 16.0
        cen = []
        k = 0
        while (k + 0.5) * pitch < n:
            t = (k + 0.5) * pitch
            for c_ in ((off, t), (n - off, t), (t, off), (t, n - off)):   # (not two at a corner)
                if all((c_[0] - e_[0]) ** 2 + (c_[1] - e_[1]) ** 2 > (3 * rr) ** 2 for e_ in cen): cen.append(c_)
            k += 1
        for cx, cy in cen:
            for y in range(int(cy - rr - 2), int(cy + rr + 3)):
                for x in range(int(cx - rr - 2), int(cx + rr + 3)):
                    dx, dy = (x + 0.5 - cx) / rr, (y + 0.5 - cy) / rr
                    q = dx * dx + dy * dy
                    i = (y % n) * n + (x % n)
                    if q <= 1.0:                     # a dome lit from the upper left, a dark rim
                        nz = math.sqrt(1.0 - q)
                        v[i] += 0.14 * (-0.6 * dx - 0.6 * dy) * (1.0 - nz) + 0.08 * nz - (0.12 if q > 0.72 else 0.0)
                    elif q <= 1.6:                   # its shadow to the lower right
                        if dx + dy > 0: v[i] -= 0.05 * (1.6 - q) / 0.6
    mean = sum(v) / len(v)
    px = bytearray(n * n * 4)
    for i, val in enumerate(v):
        c = max(0, min(255, int(val * 255 + 0.5)))
        px[4 * i:4 * i + 4] = bytes((c, c, c, 255))
    return n, bytes(px), mean


def load_texture(path):
    """PNG, SGI (.rgb/.rgba/.sgi/.bw) or DDS; a .jpg falls back to a same-named .rgb/.png next to it"""
    low = path.lower()
    if low.endswith('.png'): return png_rgba(path)
    if low.endswith(('.rgb', '.rgba', '.sgi', '.bw')): return sgi_rgba(path)
    if low.endswith('.dds'): return dds_rgba(path)
    if low.endswith(('.jpg', '.jpeg')):
        stem = path.rsplit('.', 1)[0]
        for ext in ('.png', '.rgb', '.rgba'):
            if os.path.exists(stem + ext): return load_texture(stem + ext)
    raise ValueError('unsupported texture format: ' + os.path.basename(path))


def half_rgba(w, h, px):
    """2x2 box filter (sides of 1 stay)"""
    w2, h2 = max(1, w // 2), max(1, h // 2)
    sx, sy = (2 if w > 1 else 1), (2 if h > 1 else 1)
    out = bytearray(w2 * h2 * 4)
    row = w * 4
    for y in range(h2):
        r0 = px[(y * sy) * row:(y * sy) * row + row]
        r1 = px[(y * sy + sy - 1) * row:(y * sy + sy - 1) * row + row]
        o = y * w2 * 4
        if sx == 2:
            # sum the 4 texels channel by channel with slices (byte strides of 8)
            for c in range(4):
                a = r0[c::8]; b = r0[c + 4::8]; cc = r1[c::8]; d = r1[c + 4::8]
                out[o + c:o + w2 * 4:4] = bytes((p + q + r + t + 2) >> 2 for p, q, r, t in zip(a, b, cc, d))
        else:
            for c in range(4):
                out[o + c:o + w2 * 4:4] = bytes((p + q + 1) >> 1 for p, q in zip(r0[c::4], r1[c::4]))
    return w2, h2, bytes(out)

# ---------------------------------------------------------------- AC3D

def m3mul(a, b): return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
def m3v(a, v): return [a[i][0] * v[0] + a[i][1] * v[1] + a[i][2] * v[2] for i in range(3)]
I3 = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]


def parse_ac(path):
    """returns materials and a flat list of (name, texture, texrep, texoff, verts (local->ac-world), surfaces)"""
    lines = open(path, 'r', encoding='latin-1').read().split('\n')
    i = 1
    mats, objs = [], []

    def obj(R, T, anc=()):
        nonlocal i
        while i < len(lines) and not lines[i].strip(): i += 1
        hdr = lines[i].split(); i += 1
        name, tex, rep, off, loc, rot, verts, surfs, kids = hdr[1], None, (1.0, 1.0), (0.0, 0.0), [0, 0, 0], I3, [], [], 0
        crease = 30.0
        while i < len(lines):
            ln = lines[i].strip(); t = ln.split()
            if not t: i += 1; continue
            k = t[0]
            if k == 'name': name = ln[5:].strip().strip('"'); i += 1
            elif k == 'data':                                # n bytes of data, possibly over several lines
                n = int(t[1]); i += 1; got = 0
                while i < len(lines) and got < n: got += len(lines[i].rstrip(chr(13))) + 1; i += 1
                continue
            elif k == 'texture': tex = ln[8:].strip().strip('"'); i += 1
            elif k == 'texrep': rep = (float(t[1]), float(t[2])); i += 1
            elif k == 'texoff': off = (float(t[1]), float(t[2])); i += 1
            elif k == 'crease': crease = float(t[1]); i += 1
            elif k == 'loc': loc = [float(x) for x in t[1:4]]; i += 1
            elif k == 'rot': v = [float(x) for x in t[1:10]]; rot = [v[0:3], v[3:6], v[6:9]]; i += 1
            elif k == 'numvert':
                n = int(t[1]); i += 1
                for _ in range(n): verts.append([float(x) for x in lines[i].split()[:3]]); i += 1
            elif k == 'numsurf':
                n = int(t[1]); i += 1
                for _ in range(n):
                    flags = int(lines[i].split()[1], 0); i += 1
                    mat = 0
                    while not lines[i].strip().startswith('refs'):
                        tt = lines[i].split()
                        if tt and tt[0] == 'mat': mat = int(tt[1])
                        i += 1
                    nr = int(lines[i].split()[1]); i += 1
                    refs = []
                    for _ in range(nr):
                        a = lines[i].split(); refs.append((int(a[0]), float(a[1]), float(a[2]))); i += 1
                    surfs.append((flags, mat, refs))
            elif k == 'kids':
                kids = int(t[1]); i += 1; break
            else: i += 1
        # AC3D: vertices are in object space; world = parent(R, T) applied to (rot * v + loc)
        R2 = m3mul(R, rot); T2 = [T[j] + m3v(R, loc)[j] for j in range(3)]
        wv = [[a + b for a, b in zip(m3v(R2, v), T2)] for v in verts]
        if surfs: objs.append((name, tex, rep, off, wv, surfs, crease, anc))     # anc: the AC3D group names above
        for _ in range(kids):                          # some files claim more kids than they hold
            while i < len(lines) and not lines[i].strip(): i += 1
            if i >= len(lines) or not lines[i].startswith('OBJECT'): break
            obj(R2, T2, anc + (name,))

    while i < len(lines):
        t = lines[i].split()
        if t and t[0] == 'MATERIAL':
            m = re.search(r'rgb ([\d.e-]+) ([\d.e-]+) ([\d.e-]+).*trans ([\d.e-]+)', lines[i])
            sp = re.search(r'spec ([\d.e-]+) ([\d.e-]+) ([\d.e-]+)', lines[i])
            sh = re.search(r'shi ([\d.e-]+)', lines[i])
            rgba = (float(m.group(1)), float(m.group(2)), float(m.group(3)), 1.0 - float(m.group(4))) if m else (1, 1, 1, 1)
            spec = (float(sp.group(1)) + float(sp.group(2)) + float(sp.group(3))) / 3.0 if sp else 0.0
            am = re.search(r'amb ([\d.e-]+) ([\d.e-]+) ([\d.e-]+)', lines[i])
            mats.append((rgba, spec, float(sh.group(1)) if sh else 16.0,
                         tuple(float(am.group(k)) for k in (1, 2, 3)) if am else rgba[:3]))
            i += 1
        elif t and t[0] == 'OBJECT':
            obj(I3, [0, 0, 0])
        else: i += 1
    return mats, objs

# ---------------------------------------------------------------- FlightGear PropertyList assembly


def rot_hpr(h, p, r):
    """FlightGear offsets: heading about z, pitch about y, roll about x (degrees)"""
    h, p, r = math.radians(h), math.radians(p), math.radians(r)
    Rz = [[math.cos(h), -math.sin(h), 0], [math.sin(h), math.cos(h), 0], [0, 0, 1]]
    Ry = [[math.cos(p), 0, math.sin(p)], [0, 1, 0], [-math.sin(p), 0, math.cos(p)]]
    Rx = [[1, 0, 0], [0, math.cos(r), -math.sin(r)], [0, math.sin(r), math.cos(r)]]
    return m3mul(Rz, m3mul(Ry, Rx))


AC_TO_FG = [[1, 0, 0], [0, 0, -1], [0, 1, 0]]          # AC3D (x, y up, z) -> FG (x aft, y right, z up)

# FlightGear property -> runtime value id (FlightGear conventions: knots, feet, fpm, degrees; roll + = right wing down)
PROPS = {
    'velocities/airspeed-kt': 0, 'instrumentation/airspeed-indicator/indicated-speed-kt': 0,
    'velocities/mach': 1,
    'instrumentation/altimeter/indicated-altitude-ft': 2, 'position/altitude-ft': 2,
    'instrumentation/vertical-speed-indicator/indicated-speed-fpm': 3,
    'accelerations/pilot-g': 4, 'accelerations/g-min': 5, 'accelerations/g-max': 6,
    'orientation/alpha-deg': 7,
    'orientation/pitch-deg': 8, 'orientation/model/pitch-deg': 8,
    'orientation/roll-deg': 9,
    'orientation/heading-deg': 10, 'orientation/heading-magnetic-deg': 10,
    'engines/engine[0]/n1': 11, 'engines/engine[0]/n2': 11, 'engines/engine[1]/n1': 11, 'engines/engine[1]/n2': 11,
    'engines/engine/n1': 11, 'engines/engine/n2': 11,
    # derived in the runtime (cpit_values): from the throttle, the clock
    'engines/engine[0]/egt-degf': 12, 'engines/engine[1]/egt-degf': 12,
    'engines/engine[0]/fuel-flow-gph': 13, 'engines/engine[1]/fuel-flow-gph': 13,
    'engines/engine[0]/oil-pressure-psi': 14, 'engines/engine[1]/oil-pressure-psi': 14,
    'sim/time/utc/day-seconds': 15, 'instrumentation/clock/indicated-sec': 15,
    'engines/engine[0]/egt-degC': 16, 'engines/engine[1]/egt-degC': 16,
    'engines/engine[0]/fuel-flow_pph': 17, 'engines/engine[1]/fuel-flow_pph': 17,
    # other aircraft's names for the same values (F-15)
    'accelerations/pilot-gdamped': 4, 'orientation/alpha-indicated-deg': 7,
    'instrumentation/attitude-indicator/indicated-pitch-deg': 8,
    'instrumentation/attitude-indicator/indicated-roll-deg': 9,
    'instrumentation/heading-indicator-fg/indicated-heading-deg': 10,
    'instrumentation/heading-indicator/indicated-heading-deg': 10,
    'instrumentation/gps/indicated-vertical-speed': 3,
    # MiG-21bis, J35J
    'fdm/jsbsim/systems/gyro-compass/heading-deg': 10, 'fdm/jsbsim/aero/alpha-corrected-deg': 7,
    'instrumentation/altimeter[1]/indicated-altitude-ft': 2, 'instrumentation/altimeter/pressure-alt-ft': 2,
    'instrumentation/attitude-indicator[1]/indicated-roll-deg': 9,
    'instrumentation/attitude-indicator[1]/indicated-pitch-deg': 8,
    'engines/engine/egt-degf': 12, 'instrumentation/g-max': 6,
    'position/altitude-agl-ft': 2,                       # the game flies over the sea mostly: MSL
    # MiG-29 9.12 (Russian instruments; metric ones in SCALED; the Su-27 and MiG-21bis share some of these names)
    'fdm/jsbsim/systems/instrumentation/UMS/Mach': 1, 'fdm/jsbsim/accelerations/Nz': 4,
    'mig29/instrumentation/UAP/pilot-g-max': 6, 'mig29/instrumentation/KPP/pitch-indicated-deg': 8,
    'mig29/instrumentation/KPP/roll-indicated-deg': 9, 'mig29/instrumentation/PNP-72-12/heading-indicated-deg': 10,
    'instrumentation/magnetic-compass/indicated-heading-deg': 10,
}

# properties in other units: (runtime value id, scale), FlightGear value = runtime value * scale; folded into the
# animation (rotate factor and table, textranslate step/scroll/bias/offset/factor, select values)
SCALED = {
    'mig29/instrumentation/US-1600/airspeed-kmh': (0, 1.852), 'fdm/jsbsim/systems/instrumentation/UMS/TAS-kmh': (0, 1.852),
    'mig29/instrumentation/UV-30-3/indicated-altitude-m': (2, 0.3048),
    'mig29/instrumentation/RV-21/indicated-altitude-m': (2, 0.3048),
    'fdm/jsbsim/systems/SVS/vertical-speed-mps': (3, 0.00508),
    # F-20 altimeter drums (Nasal aau-34a.nas: digit * 36 degrees, stepped): continuously rolling drums here
    'instrumentation/altimeter/alt-barel-100': (2, 0.36), 'instrumentation/altimeter/alt-barel-1k': (2, 0.036),
    'instrumentation/altimeter/alt-barel-10k': (2, 0.0036),
}


def prop_map(prop):
    """runtime value id and unit scale of a FlightGear property, or (None, 1.0) if the game has no counterpart"""
    if prop in PROPS: return PROPS[prop], PSCALE.get(prop, 1.0)     # (--prop aliases may carry a scale)
    return SCALED.get(prop, (None, 1.0))

# properties the game has no counterpart for, as a jet in normal flight would read them; folded into the animations
# here (a constant rotation, texture offset or condition), unlisted ones read 0 as a missing FlightGear property
CONSTS = {
    'instrumentation/altimeter/setting-inhg': 29.92, 'instrumentation/altimeter/setting-kpa': 101.3,
    'instrumentation/altimeter/setting-hpa': 1013.0, 'instrumentation/altimeter[1]/setting-hpa': 1013.0,
    'engines/engine/running': 1.0, 'engines/engine[0]/running': 1.0,
    'systems/electrical/volts': 28.0, 'systems/electrical/outputs/nav': 28.0,
    'controls/electric/battery-switch': 1.0,
    'position/gear-agl-m': 1000.0,                       # no gear warning light in flight
    'consumables/fuel/tank[0]/level-norm': 0.8, 'consumables/fuel/tank[1]/level-norm': 0.8,
    'consumables/fuel/tank[2]/level-norm': 0.8, 'consumables/fuel/tank[3]/level-norm': 0.8,
    'instrumentation/comm[0]/frequencies/selected-mhz': 243.0,
    'instrumentation/transponder/inputs/digit[3]': 4.0,
    'fdm/jsbsim/systems/electrics/ac-essential-bus': 115.0, 'fdm/jsbsim/systems/electrics/ac-left-main-bus': 115.0,
    'fdm/jsbsim/systems/electrics/ac-right-main-bus': 115.0,
    'consumables/fuel/total-fuel-lbs': 9000.0,                # F-15C (internal ~13,400 lb)
    'sim/model/f15/instrumentation/fuel-gauges/total-display': 9000.0,
    'sim/model/f15/instrumentation/fuel-gauges/left-display': 3000.0,
    'sim/model/f15/instrumentation/fuel-gauges/right-display': 3000.0,         # reads 4000 (digit[0] is the last drum)
    # MiG-29 9.12 (and the Su-27's copies of its instruments): 27 V bus, altimeter 760 mmHg, fuel ~0.8, hydraulics
    # and oxygen normal, attitude indicator working (no failure flags)
    'mig29/instrumentation/electrical/v27': 27.0, 'instrumentation/attitude-indicator/serviceable': 1.0,
    'mig29/instrumentation/UV-30-3/setting-mmhg-1': 7.0, 'mig29/instrumentation/UV-30-3/setting-mmhg-2': 6.0,
    'mig29/instrumentation/ISTR4-2/total-fuel-kg-t': 2700.0, 'mig29/instrumentation/ISTR4-2/tank-2-kg': 440.0,
    'fdm/jsbsim/hs/hs1-pressure': 210.0, 'fdm/jsbsim/hs/hs2-pressure': 210.0, 'mig29/instrumentation/IKG/main': 150.0,
    'mig29/instrumentation/IKG/emergency': 150.0, 'mig29/instrumentation/IKZh/o2_reserve': 100.0,
    # F-20: Kollsman drums 29.92 (digit * 36 degrees), fuel ~0.8, bingo 1000 lb, hydraulics 3000 psi
    'instrumentation/altimeter/baro-barel-1': 72.0, 'instrumentation/altimeter/baro-barel-2': 324.0,
    'instrumentation/altimeter/baro-barel-3': 324.0, 'instrumentation/altimeter/baro-barel-4': 72.0,
    'consumables/fuel/tank/display-level-lbs': 1800.0, 'consumables/fuel/tank[1]/display-level-lbs': 1600.0,
    'consumables/fuel/bingo-value': 1000.0, 'systems/hydraulics/pressure-flt-ctl-psi': 3000.0,
    'systems/hydraulics/pressure-utility-psi': 3000.0,
}
P_ZERO = 255                                             # reads 0 in the runtime
# --controls: the pilot's controls (runtime CPIT_P_ELEV .. CPIT_P_THR)
CONTROL_PROPS = {
    'controls/flight/elevator': 18, 'controls/flight/aileron': 19, 'controls/flight/rudder': 20,
    'controls/engines/engine/throttle': 21, 'controls/engines/engine[0]/throttle': 21,
    'controls/engines/engine[1]/throttle': 21, 'controls/engines/engine[2]/throttle': 21,
    'controls/engines/engine[3]/throttle': 21,
}
CONTROLS = False
GAUGE_TRANSLATE = False                                  # --gauge-translate: translate animations of every mapped property
P_THR = 21
PSCALE = {}                                              # --prop aliases: FlightGear value = runtime value * scale


def stepped(v, step, scroll):
    """SimGear's SGStepExpression (odometer drums)"""
    if step <= 0.0: return v
    sc = 0.0
    if scroll > 0.0:
        rem = step - math.fmod(abs(v), step)
        if rem < scroll: sc = (scroll - rem) / scroll * step
    q = v / step                                         # + epsilon: 101.3 / 0.1 is 1012.999... in binary
    return (math.floor(q + 1e-9) if v > 0 else math.ceil(q - 1e-9)) * step + sc


def interp(table, v):
    if not table: return None
    if v <= table[0][0]: return table[0][1]
    for (x0, y0), (x1, y1) in zip(table, table[1:]):
        if v <= x1: return y0 + (y1 - y0) * (v - x0) / (x1 - x0) if x1 > x0 else y1
    return table[-1][1]


SEL_OPS = {'less-than': 1, 'less-than-equals': 2, 'greater-than': 3, 'greater-than-equals': 4, 'equals': 5,
           'not-equals': 6}


def sel_term(e, neg=0):
    """one condition term (comparison / property / not): (code, value) or None if unsupported"""
    tag = e.tag
    if tag == 'not':
        kids = list(e)
        return sel_term(kids[0], neg ^ 1) if len(kids) == 1 else None
    if tag in ('true', 'false'):                         # constant (Mirage 2000D: <false/> hides single-seat parts)
        return (5 | (P_ZERO << 8), 0.0 if (tag == 'true') != bool(neg) else 1.0)
    if tag == 'property':
        prop = (e.text or '').strip().lstrip('/')
        if prop in CONSTS: return (5 | (P_ZERO << 8), 0.0 if (CONSTS[prop] != 0.0) != bool(neg) else 1.0)
        pid, _ = prop_map(prop)
        return (7 | ((255 if pid is None else pid) << 8) | (neg << 16), 0.0)
    if tag in SEL_OPS:
        pr, va = e.findtext('property'), e.findtext('value')
        if pr is None or va is None or len(e.findall('property')) != 1: return None
        prop = pr.strip().lstrip('/')
        try: val = float(va.strip())
        except ValueError: return None
        if prop in CONSTS:
            x, op = CONSTS[prop], SEL_OPS[tag]
            r = [None, x < val, x <= val, x > val, x >= val, x == val, x != val][op]
            return (5 | (P_ZERO << 8), 0.0 if r != bool(neg) else 1.0)
        op = SEL_OPS[tag]
        pid, s = prop_map(prop)
        if s != 1.0:                                     # s * runtime value op val
            val /= s
            if s < 0: op = {1: 3, 2: 4, 3: 1, 4: 2}.get(op, op)
        return (op | ((255 if pid is None else pid) << 8) | (neg << 16), val)
    return None


FOLD_SELECTS = False         # --fold-selects: fold the conditions parse_select cannot express (fold_cond)
SEL_SWAP = {1: 3, 2: 4, 3: 1, 4: 2, 5: 5, 6: 6}         # a < b  <=>  b > a


def fold_cond(e):
    """--fold-selects: one condition element as ('c', bool) when it does not depend on a mapped property (unmapped
    properties read 0, CONSTS their value), ('t', mode, terms) for a flat and (mode 0) / or (1) of runtime terms, or
    None (not expressible: the part stays visible, as without the option). Nested and/or/not fold and flatten where
    the result stays one level deep; comparisons may hold two properties if at most one is mapped."""
    tag = e.tag
    if tag in ('true', 'false'): return ('c', tag == 'true')
    if tag == 'property':
        prop = (e.text or '').strip().lstrip('/')
        if prop in PROPS: return ('t', 0, [(7 | (PROPS[prop] << 8), 0.0)])
        return ('c', CONSTS.get(prop, 0.0) != 0.0)
    if tag == 'not':
        kids = list(e)
        if len(kids) != 1: return None
        r = fold_cond(kids[0])
        if r is None: return None
        if r[0] == 'c': return ('c', not r[1])
        return ('t', 1 - r[1], [(code ^ (1 << 16), val) for code, val in r[2]])      # De Morgan
    if tag in ('and', 'or'):
        return fold_group(1 if tag == 'or' else 0, [fold_cond(k) for k in e])
    if tag in SEL_OPS:
        op = SEL_OPS[tag]
        props = [(p.text or '').strip().lstrip('/') for p in e.findall('property')]
        vals = e.findall('value')
        cmp_ = lambda x, y: [None, x < y, x <= y, x > y, x >= y, x == y, x != y][op]
        if len(props) == 2 and not vals:
            m = [p in PROPS for p in props]
            if all(m): return None
            if not any(m): return ('c', cmp_(CONSTS.get(props[0], 0.0), CONSTS.get(props[1], 0.0)))
            if m[0]: return ('t', 0, [(op | (PROPS[props[0]] << 8), CONSTS.get(props[1], 0.0))])
            return ('t', 0, [(SEL_SWAP[op] | (PROPS[props[1]] << 8), CONSTS.get(props[0], 0.0))])
        if len(props) != 1 or len(vals) != 1: return None
        try: val = float((vals[0].text or '').strip())
        except ValueError:                               # a string value: an unset property never equals it
            if props[0] in PROPS or op not in (5, 6): return None
            return ('c', op == 6)
        if props[0] in PROPS: return ('t', 0, [(op | (PROPS[props[0]] << 8), val)])
        return ('c', cmp_(CONSTS.get(props[0], 0.0), val))
    return None


def fold_group(mode, parts):
    """and (mode 0) / or (1) of folded parts: constants decide or drop out, same-mode groups flatten"""
    if any(p is None for p in parts): return None
    terms = []
    for p in parts:
        if p[0] == 'c':
            if p[1] == bool(mode): return ('c', bool(mode))         # and: a false part; or: a true part
            continue
        if len(p[2]) > 1 and p[1] != mode: return None              # a nested group of the other kind
        terms.extend(p[2])
    if not terms: return ('c', not mode)                            # and of nothing: true; or of nothing: false
    if len(terms) > 16: return None
    return ('t', mode, terms)


def parse_select(a):
    """a select animation's condition as (or?, negate?, terms), or None (unsupported: the part stays visible)"""
    c = a.find('condition')
    if c is None: return None
    kids = list(c)
    mode, negate = 0, 0
    if len(kids) == 1 and kids[0].tag == 'not' and len(list(kids[0])) == 1 and list(kids[0])[0].tag in ('and', 'or'):
        negate, kids = 1, [list(kids[0])[0]]
    if len(kids) == 1 and kids[0].tag in ('and', 'or'):
        mode = 1 if kids[0].tag == 'or' else 0
        kids = list(kids[0])
    terms = [sel_term(k) for k in kids]
    if not terms or any(t is None for t in terms) or len(terms) > 16:
        if not FOLD_SELECTS: return None
        r = fold_group(0, [fold_cond(k) for k in c])
        if r is None or r == ('c', True): return None              # always shown: no animation needed
        if r[0] == 'c': return (0, 0, [(5 | (P_ZERO << 8), 1.0)])  # never shown: 0 == 1
        return (r[1], 0, r[2])
    return (mode, negate, terms)


TEXROTATE = False            # --texrotate: texmultiple / texrotate animations on disc-shaped parts


def textranslate_entry(a, objs):
    """a FlightGear textranslate (an animation, or a texmultiple transform) as a parse_anims entry, or None"""
    prop = (a.findtext('property') or '').strip().lstrip('/')
    if not prop: return None
    def f(k, d):
        t = a.findtext(k)
        return num(t, d)
    ax = a.find('axis')
    axx = num(ax.findtext('x')) if ax is not None else 0.0
    axy = num(ax.findtext('y')) if ax is not None else 1.0
    pid, s = prop_map(prop)
    if pid is None or s <= 0:                          # constant, or unmapped (reads 0)
        d = (stepped(CONSTS.get(prop, 0.0) + f('bias', 0.0), f('step', 0.0), f('scroll', 0.0))
             + f('offset', 0.0)) * f('factor', 1.0)          # SimGear: (stepped + offset) * factor
        return (objs, P_ZERO | 0x100, [0.0, 0.0, 0.0], [axx, axy, 0.0], 1.0, d, [])
    # in other units (s != 1): stepped(s v + b, step, scroll) = s stepped(v + b/s, step/s, scroll/s)
    return (objs, pid | 0x100, [f('step', 0.0) / s, f('scroll', 0.0) / s, f('bias', 0.0) / s], [axx, axy, 0.0],
            f('factor', 1.0) * s, f('offset', 0.0) / s, [])


def disc_pivot(ev, surfs, rep, off, cu, cv):
    """--texrotate: (eye-frame point showing texture point (cu, cv), unit normal about which a rotation turns the
    texture from u towards v) of a part, or None unless the part is a disc about that point (then rotating the part
    equals rotating its texture)"""
    nsum = [0.0, 0.0, 0.0]; hit = None; best = None
    for flags, mat, refs in surfs:
        if (flags & 15) != 0 or len(refs) < 3: continue
        for k in range(1, len(refs) - 1):
            tri = [refs[0], refs[k], refs[k + 1]]
            P = [ev[r[0]] for r in tri]
            U = [(r[1] * rep[0] + off[0], r[2] * rep[1] + off[1]) for r in tri]
            e1 = [P[1][j] - P[0][j] for j in range(3)]; e2 = [P[2][j] - P[0][j] for j in range(3)]
            du1, dv1, du2, dv2 = U[1][0] - U[0][0], U[1][1] - U[0][1], U[2][0] - U[0][0], U[2][1] - U[0][1]
            det = du1 * dv2 - du2 * dv1
            if abs(det) < 1e-12: continue
            Tg = [(e1[j] * dv2 - e2[j] * dv1) / det for j in range(3)]
            Bg = [(e2[j] * du1 - e1[j] * du2) / det for j in range(3)]
            n = [Tg[1] * Bg[2] - Tg[2] * Bg[1], Tg[2] * Bg[0] - Tg[0] * Bg[2], Tg[0] * Bg[1] - Tg[1] * Bg[0]]
            ln = math.sqrt(sum(x * x for x in n)) or 1.0
            area = 0.5 * math.sqrt(sum(x * x for x in (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                                        e1[0] * e2[1] - e1[1] * e2[0])))
            nsum = [nsum[j] + n[j] / ln * area for j in range(3)]
            # barycentric position of (cu, cv) in this triangle's texture coordinates
            l1 = ((cu - U[0][0]) * dv2 - du2 * (cv - U[0][1])) / det
            l2 = (du1 * (cv - U[0][1]) - (cu - U[0][0]) * dv1) / det
            q = [P[0][j] + l1 * e1[j] + l2 * e2[j] for j in range(3)]
            miss = max(0.0, -l1) + max(0.0, -l2) + max(0.0, l1 + l2 - 1.0)
            if best is None or miss < best[0]: best = (miss, q)
    ln = math.sqrt(sum(x * x for x in nsum))
    if best is None or ln == 0.0 or best[0] > 1e-3: return None
    hit = best[1]
    d = [math.sqrt(sum((v[j] - hit[j]) ** 2 for j in range(3))) for v in ev]
    rmax = max(d)
    if rmax <= 0.0 or any(0.2 * rmax < x < 0.9 * rmax for x in d): return None     # not a disc about the pivot
    return hit, [x / ln for x in nsum]


AC_CACHE = {}


def ac_cached(path):
    if path not in AC_CACHE: AC_CACHE[path] = parse_ac(path)
    return AC_CACHE[path]


def f32(x): return struct.unpack('f', struct.pack('f', x))[0]


def sg_line_order(va, vb, xyz):
    """SimGear's LineCollector::addLine (simgear/scene/model/animation.cxx): the two ends va, vb of an axis line (AC3D
    file frame, in the line's vertex order) as the segment's start -> end (axis = end - start); True = va -> vb. By X
    (FlightGear <= 2020.3, and the default since): the end with the smaller x is the end (x points aft in the AC3D and
    the FlightGear frame alike: the axis points forward), equal x: vb -> va. By X, Y, Z (<axis><order-by-xyz/>, a model's
    <defaults><axis-animation-vertex-order-xyz/>, FlightGear 2024.1): the smaller of the two by x, then y, then z
    (0.01 mm apart) is the end. Compared in single precision, as OSG holds the vertices"""
    a, b = [f32(c) for c in va], [f32(c) for c in vb]
    if not xyz: return a[0] > b[0]
    for k in range(2):
        if abs(b[k] - a[k]) > 0.00001: return b[k] < a[k]
    return b[2] < a[2]


def axis_object(name, srcs, order=None):
    """SimGear's rotation axis given by a line object (an "-axis" object of two vertices): centre = its middle, axis =
    first -> second vertex (--axis-fg-sense: second -> first; order = (xyz, swap), --axis-sg-order: the ends in
    SimGear's order, sg_line_order, reversed by <swap-axis-direction/>), both in the root FG frame; srcs = (AC file, R,
    T) of the XML's models; None if not found"""
    for acp, R, T in srcs:
        if not os.path.exists(acp): continue
        for oname, _, _, _, wv, surfs, _, _ in ac_cached(acp)[1]:
            if oname != name or len(wv) < 2: continue
            ln = next((r for fl, _, r in surfs if (fl & 15) in (1, 2) and len(r) >= 2), None)
            ks = (ln[0][0], ln[1][0]) if ln else (0, 1)
            p = [[x + y for x, y in zip(m3v(R, m3v(AC_TO_FG, wv[k])), T)] for k in ks]
            if order is not None:                        # --axis-sg-order (ck_axes 2026-10-31, bug #262)
                fwd = sg_line_order(wv[ks[0]], wv[ks[1]], order[0]) != order[1]
                if not fwd: p.reverse()
                AXIS_SG_STATS[0] += 1; AXIS_SG_STATS[1] += order[0]; AXIS_SG_STATS[2] += order[1]
                AXIS_SG_STATS[3] += fwd                  # (first -> second: against --axis-fg-sense)
            elif AXIS_FG_SENSE: p.reverse()              # (bug #258: the F-14's pointers and controls ran backwards)
            return [(p[0][j] + p[1][j]) * 0.5 for j in range(3)], [p[1][j] - p[0][j] for j in range(3)]
    return None


def parse_anims(root, R, T, axis_src=(), xyz=False):
    """rotate animations of this XML's model: (objects, prop id, center (root FG), axis (root FG), factor, offset,
    table); textranslate ones: (objects, prop id | 0x100, (step, scroll, bias), (axis x, y, 0), factor, offset, []);
    axis_src: the XML's AC models (file, R, T) for axes given by objects (--axis-objects); xyz: the XML orders its axis
    lines by X, Y, Z (--axis-sg-order: its own <defaults> or an including XML's)"""
    res = []
    for a in root.findall('animation'):
        kind = (a.findtext('type') or '').strip()
        if kind == 'select':
            sel = parse_select(a)
            if sel is None: continue
            objs = [o.text.strip() for o in a.findall('object-name') if o.text]
            mode, negate, terms = sel
            res.append((objs, 0x200, [float(mode), float(negate), 0.0], [0.0, 0.0, 0.0], 1.0, 0.0,
                        [(float(code), val) for code, val in terms]))
            continue
        if kind == 'textranslate':
            objs = [o.text.strip() for o in a.findall('object-name') if o.text]
            e = textranslate_entry(a, objs)
            if e is not None: res.append(e)
            continue
        if kind in ('texmultiple', 'texrotate') and TEXROTATE:
            # --texrotate: the transforms of a texmultiple (or one texrotate) in order; a texture rotation becomes a
            # rotation of the part about the point showing the rotation centre (main(): only for disc-shaped parts,
            # where that equals rotating the texture: attitude displays)
            objs = [o.text.strip() for o in a.findall('object-name') if o.text]
            for t in (a.findall('transform') if kind == 'texmultiple' else [a]):
                sub = (t.findtext('subtype') or kind).strip()
                if sub == 'textranslate':
                    e = textranslate_entry(t, objs)
                    if e is not None: res.append(e)
                elif sub == 'texrotate':
                    prop = (t.findtext('property') or '').strip().lstrip('/')
                    c = t.find('center'); ax = t.find('axis')
                    cu = num(c.findtext('x')) if c is not None else 0.5
                    cv = num(c.findtext('y')) if c is not None else 0.5
                    az = num(ax.findtext('z')) if ax is not None else 1.0
                    table = [(num(en.findtext('ind')), num(en.findtext('dep')))
                             for en in (t.find('interpolation').findall('entry') if t.find('interpolation') is not None else [])]
                    fac, off = num(t.findtext('factor'), 1.0), num(t.findtext('offset-deg'))
                    if prop not in PROPS:                      # constant, or unmapped (reads 0)
                        cval = CONSTS.get(prop, 0.0)
                        ang = interp(table, cval)
                        fac, off, table = 0.0, (cval * fac + off if ang is None else ang), []
                    res.append((objs, (PROPS.get(prop, P_ZERO) if fac or table else P_ZERO) | 0x400, [cu, cv, az],
                                [0.0, 0.0, 0.0], fac, off, table))
            continue
        if kind == 'translate' and (CONTROLS or GAUGE_TRANSLATE):
            # --controls: sliding levers and pedals (SimGear translate: value * factor + offset-m, or the table, metres
            # along the normalised axis); only for the controls, other translate animations stay ignored as before
            prop = (a.findtext('property') or '').strip().lstrip('/')
            pid, s = prop_map(prop)
            if pid not in CONTROL_PROPS.values() and not (GAUGE_TRANSLATE and pid is not None): continue
            objs = [o.text.strip() for o in a.findall('object-name') if o.text]
            ax = a.find('axis')
            gx = lambda k: num(ax.findtext(k)) if ax is not None else 0.0
            if ax is not None and ax.find('x1-m') is not None:
                axis = [gx('x2-m') - gx('x1-m'), gx('y2-m') - gx('y1-m'), gx('z2-m') - gx('z1-m')]
            else: axis = [gx('x'), gx('y'), gx('z')] if ax is not None else [1.0, 0.0, 0.0]
            aw = m3v(R, axis); ln = math.sqrt(sum(x * x for x in aw)) or 1.0
            aw = [x / ln for x in aw]
            table = []
            it = a.find('interpolation')
            if it is not None:
                table = sorted(((num(e.findtext('ind')) / s, num(e.findtext('dep'))) for e in it.findall('entry')),
                               key=lambda e: e[0])
            res.append((objs, pid | 0x800, list(T), aw, num(a.findtext('factor'), 1.0) * s, num(a.findtext('offset-m')),
                        table))
            continue
        if kind != 'rotate': continue
        prop = (a.findtext('property') or '').strip().lstrip('/')
        if not prop: continue
        objs = [o.text.strip() for o in a.findall('object-name') if o.text]
        c = a.find('center'); ax = a.find('axis')
        def g(e, k):
            return num(e.findtext(k)) if e is not None else 0.0
        center = [g(c, 'x-m'), g(c, 'y-m'), g(c, 'z-m')]
        axis = [g(ax, 'x'), g(ax, 'y'), g(ax, 'z')] if ax is not None else [0.0, 0.0, 1.0]
        fac = num(a.findtext('factor'), 1.0)
        off = num(a.findtext('offset-deg'))
        table = []
        it = a.find('interpolation')
        if it is not None:
            for e in it.findall('entry'):
                table.append((num(e.findtext('ind')), num(e.findtext('dep'))))
        if SORT_TABLES: table.sort(key=lambda e: e[0])
        cw = [a_ + b_ for a_, b_ in zip(m3v(R, center), T)]
        aw = m3v(R, axis)
        # (--controls: the controls' rotations read their axes so too, whatever --axis-objects says: the F-14's stick
        # and throttles turn about line objects)
        if AXIS_OBJECTS or (CONTROLS and prop_map(prop)[0] in CONTROL_PROPS.values()):
            # SimGear (readRotationCenterAndAxis): <axis><object-name> (a line object), or with no <axis> at all an
            # object named "<first object-name>-axis" if there is one; else the two points axis/x1-m .. z2-m (centre
            # their middle unless <center> says otherwise), else axis/x, y, z
            an_ = (ax.findtext('object-name') or '').strip() if ax is not None else (objs[0] + '-axis' if objs else '')
            order = None
            if AXIS_SG_ORDER:                            # SimGear: the animation's own tags over the XML's default
                xyz_ = xyz
                if ax is not None and ax.find('order-by-xyz') is not None: xyz_ = True
                elif ax is not None and ax.find('order-by-x') is not None: xyz_ = False
                order = (xyz_, ax is not None and ax.find('swap-axis-direction') is not None)
            got = axis_object(an_, axis_src, order) if an_ else None
            if got:
                cw, aw = got
                if AXIS_FLIP and AXIS_FLIP.search(an_):     # --axis-flip: a line drawn the wrong way round
                    aw = [-x for x in aw]
                    print('--axis-flip: %s turned the other way (%s)' % (an_, ', '.join(objs)))
            elif ax is not None and ax.find('x1-m') is not None:
                p1 = [g(ax, 'x1-m'), g(ax, 'y1-m'), g(ax, 'z1-m')]; p2 = [g(ax, 'x2-m'), g(ax, 'y2-m'), g(ax, 'z2-m')]
                mid = [(p1[j] + p2[j]) * 0.5 for j in range(3)]
                center = [num(c.findtext(k), mid[j]) if c is not None else mid[j] for j, k in enumerate(('x-m', 'y-m', 'z-m'))]
                cw = [a_ + b_ for a_, b_ in zip(m3v(R, center), T)]
                aw = m3v(R, [p2[j] - p1[j] for j in range(3)])
        pid, s = prop_map(prop)
        if pid is None:                                        # constant, or unmapped (reads 0)
            cv = CONSTS.get(prop, 0.0)
            ang = interp(table, cv)
            res.append((objs, P_ZERO, cw, aw, 0.0, cv * fac + off if ang is None else ang, []))
            continue
        if s != 1.0:                                           # in other units: s * runtime value; SimGear
            table = sorted(((ind / s, dep) for ind, dep in table), key=lambda e: e[0])   # orders a table by input
        res.append((objs, pid, cw, aw, fac * s, off, table))
    return res


NUM_RX = re.compile(r'[-+]?(?:[0-9]+[.]?[0-9]*|[.][0-9]+)(?:[eE][-+]?[0-9]+)?')


def num(text, default=0.0):
    """leading number of an XML value; FlightGear files sometimes add units ("0.000 m") or leave it empty"""
    m = NUM_RX.search(text or '')
    return float(m.group(0)) if m else default


AIRCRAFT_ROOT = None        # --aircraft-root: where FlightGear's "Aircraft/<name>/..." paths point


def fg_path(base, text):
    """a model path from an XML: relative to that XML, or "Aircraft/<name>/..." relative to --aircraft-root"""
    t = text.lstrip('/')
    if AIRCRAFT_ROOT and t.startswith('Aircraft/') and t.count('/') >= 2:
        if t.startswith(('Aircraft/Instruments-3d/', 'Aircraft/Instruments/', 'Aircraft/Generic/')):
            # FlightGear's shared models (fgdata): ../fgdata next to the aircraft folder
            return os.path.normpath(os.path.join(os.path.dirname(os.path.normpath(AIRCRAFT_ROOT)), 'fgdata', t))
        return os.path.normpath(os.path.join(AIRCRAFT_ROOT, t.split('/', 2)[2].lstrip('/')))   # ('F-23C//Models')
    rel = os.path.normpath(os.path.join(base, text))
    if AIRCRAFT_ROOT and not os.path.exists(rel):          # FlightGear falls back to the aircraft folder
        alt = os.path.normpath(os.path.join(AIRCRAFT_ROOT, t))
        if os.path.exists(alt): return alt
    return rel


def load_xml(path, depth=0):
    """a PropertyList XML with FlightGear's include="FILE" attributes resolved (path relative to the including file,
    else the aircraft folder): the included file's children go into the element, or with omit-node="y" in its place
    (the F-20 cockpit takes its altimeter, Mach meter, caution lights ... this way)"""
    root = ET.parse(path).getroot()
    if depth < 8:
        inc_children(root, os.path.dirname(path), depth)
    return root


def inc_children(el, base, depth):
    new, changed = [], False
    for k in list(el):
        inc = k.get('include')
        inc_children(k, base, depth)
        if inc:
            p = fg_path(base, inc)
            if os.path.exists(p):
                sub = list(load_xml(p, depth + 1))
                if k.get('omit-node', '').strip().lower() in ('y', 'yes', 'true', '1'):
                    new.extend(sub + list(k)); changed = True
                    continue
                for c in reversed(sub): k.insert(0, c)
        new.append(k)
    if changed: el[:] = new


def hidden_names(root):
    """object / sub-model names that a select animation with a constant false condition hides; FlightGear applies an
    XML's animations to its whole subtree, sub-models included (the Mirage 2000D hides the single-seat canopy of the
    shared exterior and the 2000-5 panel this way)"""
    names = set()
    for a in root.findall('animation'):
        kind = (a.findtext('type') or '').strip()
        # click areas FlightGear never draws (<visible>false</visible>) and Rembrandt light volumes (the MiG-29's
        # floodlight cones): not geometry in FlightGear's default renderer
        if (kind == 'pick' and (a.findtext('visible') or '').strip().lower() == 'false') or kind == 'light':
            names.update(o.text.strip() for o in a.findall('object-name') if o.text)
            continue
        if kind != 'select': continue
        c = a.find('condition')
        kids = list(c) if c is not None else []
        if len(kids) != 1: continue
        k = kids[0]
        if k.tag == 'false' or (k.tag == 'not' and len(list(k)) == 1 and list(k)[0].tag == 'true'):
            names.update(o.text.strip() for o in a.findall('object-name') if o.text)
    if CONST_SELECTS:
        for a in root.findall('animation'):
            if (a.findtext('type') or '').strip() == 'select' and a.find('condition') is not None \
                    and cond_eval(list(a.find('condition'))) is False:
                hid = [o.text.strip() for o in a.findall('object-name') if o.text]
                names.update(hid)
                print('hidden by a constant select:', ', '.join(hid)[:150])
    return names


CONST_SELECTS = False       # --const-selects: evaluate selects that read only constants / unmapped properties here
ANIM_SUBMODELS = False      # --anim-submodels: an XML's animations also move / select its sub-models' objects
GROUP_NAMES = False         # --group-names: AC3D group names count for their objects (selects, animations, hiding)
AXIS_OBJECTS = False        # --axis-objects: rotation axes given by line objects / two points (SimGear)
AXIS_FG_SENSE = False       # --axis-fg-sense: an axis object points from its line's second point to its first
AXIS_FLIP = None            # --axis-flip REGEX: these axis objects point the other way
AXIS_SG_ORDER = False       # --axis-sg-order: an axis object's ends ordered as SimGear's LineCollector orders them
AXIS_SG_STATS = [0, 0, 0, 0]    # (--axis-sg-order) lines read, by X Y Z, swapped, pointing first -> second
RANGE = False               # --range: range animations hide what the design eye is outside of (SimGear's LOD)
RANGE_ANIMS = []            # (--range) (object names, min-m, max-m) of the XMLs' range animations
TEX_PATHS = False           # --tex-paths: missing texture paths resolved as the modeller's machine / OSG did


def tex_fallback(acp, tex, tp):
    """a texture path that does not exist here: an absolute one of the modeller's machine ("C:/Users/.../Aircraft/
    <this aircraft>/Models/x.png") under --aircraft-root, else its file name next to the model (OSG's findDataFile)"""
    m = re.search(r'(?:^|[/\\])Aircraft[/\\]([^/\\]+)[/\\](.+)$', tex)
    if m and AIRCRAFT_ROOT and m.group(1) == os.path.basename(os.path.normpath(AIRCRAFT_ROOT)):
        q = os.path.normpath(os.path.join(AIRCRAFT_ROOT, m.group(2)))
        if os.path.exists(q): return q
    q = os.path.normpath(os.path.join(os.path.dirname(acp), re.split(r'[/\\]', tex)[-1]))
    return q if os.path.exists(q) else tp


SORT_TABLES = False         # --sort-tables: interpolation tables ordered by input, as SimGear keeps them
EAR_CLIP = False            # --ear-clip: concave polygons by ear clipping (poly_tris), convex ones as a fan
EAR_STATS = [0, 0, 0, 0]    # polygons ear-clipped, kept as a fan after a failed clip, convex (fan), concave with a
                            # valid fan (from a reflex corner: kept)


def poly_tris(P):
    """triangles (index triples, the polygon's winding kept) of a polygon with 3D vertices P: the fan from vertex 0,
    or with --ear-clip, for a concave polygon whose fan folds over (stray triangles outside it), ear clipping in the
    plane of its largest normal component"""
    n = len(P)
    fan = [(0, k, k + 1) for k in range(1, n - 1)]
    if not EAR_CLIP or n <= 3:
        return fan
    nr = [0.0, 0.0, 0.0]                                 # Newell normal: twice the signed areas in the three planes
    for a in range(n):
        p, q = P[a], P[(a + 1) % n]
        nr[0] += (p[1] - q[1]) * (p[2] + q[2]); nr[1] += (p[2] - q[2]) * (p[0] + q[0]); nr[2] += (p[0] - q[0]) * (p[1] + q[1])
    ax = max(range(3), key=lambda j: abs(nr[j]))
    if abs(nr[ax]) < 1e-15:
        return fan
    i1, i2 = ((1, 2), (2, 0), (0, 1))[ax]               # the plane whose area sign is nr[ax]'s
    sg = 1.0 if nr[ax] > 0 else -1.0
    Q = [(v[i1], v[i2]) for v in P]
    def cross(o, a_, b_):
        return ((a_[0] - o[0]) * (b_[1] - o[1]) - (a_[1] - o[1]) * (b_[0] - o[0])) * sg
    eps = 1e-12 * abs(nr[ax])
    if all(cross(Q[k - 1], Q[k], Q[(k + 1) % n]) >= -eps for k in range(n)):
        EAR_STATS[2] += 1
        return fan
    # (ck_earclip 2026-10-29) a fan none of whose triangles turns against the polygon tiles exactly the polygon (the
    # signed fan areas sum to the polygon's: no overlap, nothing outside it): a fan from a reflex corner stays
    if all(cross(Q[0], Q[k], Q[k + 1]) >= -eps for k in range(1, n - 1)):
        EAR_STATS[3] += 1
        return fan
    def inside(t, a_, b_, c_):                           # inside the (counter-clockwise) triangle a b c or on its edges
        return cross(a_, b_, t) >= -eps and cross(b_, c_, t) >= -eps and cross(c_, a_, t) >= -eps
    idx, tris = list(range(n)), []
    while len(idx) > 3:
        m = len(idx)
        for a in range(m):
            i0, j, i2_ = idx[a - 1], idx[a], idx[(a + 1) % m]
            if cross(Q[i0], Q[j], Q[i2_]) <= eps:
                continue                                 # a reflex or flat corner: no ear
            # (ck_earclip) a vertex on the ear's edges blocks it too: in a degenerate polygon (an edge running back
            # over another: the Gripen FG floor's spike) an ear across the notch turned the rest inside out
            if any(inside(Q[r], Q[i0], Q[j], Q[i2_]) for r in idx if r not in (i0, j, i2_)
                   and Q[r] != Q[i0] and Q[r] != Q[j] and Q[r] != Q[i2_]):
                continue
            tris.append((i0, j, i2_)); del idx[a]
            break
        else:
            if abs(sum(cross(Q[idx[0]], Q[idx[k]], Q[idx[k + 1]]) for k in range(1, len(idx) - 1))) <= eps * len(idx):
                idx = []                                 # what is left has no area (collinear corners): nothing to draw
                break
            EAR_STATS[1] += 1
            return fan
    if idx:
        if cross(Q[idx[0]], Q[idx[1]], Q[idx[2]]) < -eps:
            EAR_STATS[1] += 1                            # the last triangle turned inside out: the clip went wrong
            return fan
        tris.append((idx[0], idx[1], idx[2]))
    # a fan over a notch has more area than the polygon; a clip with more area than the fan (a bent polygon whose
    # corners meet in its plane: the F-20A's HUD frame) removes no stray fill: the fan stays
    def area(t):
        a_, b_, c_ = P[t[0]], P[t[1]], P[t[2]]
        e1 = [b_[k] - a_[k] for k in range(3)]; e2 = [c_[k] - a_[k] for k in range(3)]
        return math.sqrt((e1[1] * e2[2] - e1[2] * e2[1]) ** 2 + (e1[2] * e2[0] - e1[0] * e2[2]) ** 2
                         + (e1[0] * e2[1] - e1[1] * e2[0]) ** 2)
    if sum(area(t) for t in tris) > sum(area(t) for t in fan) * (1.0 + 1e-6):
        EAR_STATS[1] += 1
        return fan
    EAR_STATS[0] += 1
    return tris
SCONSTS = {}                # --const P=TEXT: string constants (an MFD's selected page)


def cond_eval(terms):
    """FlightGear condition terms (implicit and) over constants: True / False, or None when a live property (one the
    runtime animates) is read; unmapped properties read 0 / "" (a missing property)"""
    def val(prop):
        prop = prop.strip().lstrip('/')
        if prop in SCONSTS: return SCONSTS[prop]
        if prop in CONSTS: return CONSTS[prop]
        if prop in PROPS: raise LookupError(prop)
        return None                                      # missing: 0 against a number, "" against a string

    def lit(t):
        t = (t or '').strip()
        if t.lower() in ('true', 'false'): return 1.0 if t.lower() == 'true' else 0.0
        try: return float(t)
        except ValueError: return t

    def one(e):
        if e.tag in ('true', 'false'): return e.tag == 'true'
        if e.tag == 'not': return not all(one(k) for k in e)
        if e.tag == 'and': return all(one(k) for k in e)
        if e.tag == 'or': return any(one(k) for k in e)
        if e.tag == 'property':
            x = val(e.text or ''); x = lit(x) if isinstance(x, str) else x
            return bool(x) if not isinstance(x, str) else x != ''
        if e.tag in SEL_OPS:
            pr, va = e.findall('property'), e.findall('value')
            if len(pr) != 1 or len(va) != 1: raise LookupError(e.tag)
            x, v = val(pr[0].text or ''), lit(va[0].text)
            x = ('' if isinstance(v, str) else 0.0) if x is None else lit(x) if isinstance(x, str) else x
            if isinstance(x, str) or isinstance(v, str):
                x, v = str(x), str(v)
                if e.tag in ('equals', 'not-equals'): return (x == v) == (e.tag == 'equals')
                raise LookupError(e.tag)
            return [None, x < v, x <= v, x > v, x >= v, x == v, x != v][SEL_OPS[e.tag]]
        raise LookupError(e.tag)
    try: return all(one(t) for t in terms) if terms else None
    except LookupError: return None


def const_true_select(an):
    """a select animation (parse_anims tuple) that reads only constants / unmapped properties and is always true"""
    if an[1] != 0x200 or any(((int(code) >> 8) & 255) != P_ZERO for code, _ in an[6]): return False
    rs = []
    for code, val in an[6]:
        code = int(code); op = code & 255
        r = [None, 0.0 < val, 0.0 <= val, 0.0 > val, 0.0 >= val, 0.0 == val, 0.0 != val, False][op]
        rs.append(bool(r) != bool((code >> 16) & 1))
    on = any(rs) if an[2][0] else all(rs)
    return on != bool(an[2][1])


def assemble(xml_path, R, T, skip, out, depth=0, hidden=frozenset(), outer=(), named=(), xyz=False):
    """the models of a FlightGear XML into out as (AC file, R, T, animations, None, hidden names); with
    --anim-submodels an XML's animations also reach the objects of its sub-models as in SimGear: outer = inherited
    animations that move a whole named sub-model (objects None; outermost first), named = inherited ones by object
    name (SimGear installs them after the sub-model's own: innermost, the nearest XML first); xyz: the including
    XML's axis-line order (--axis-sg-order; SimGear passes a model's <defaults> on to its sub-models)"""
    if depth > 8: return
    base = os.path.dirname(xml_path)
    root = load_xml(xml_path)
    hidden = frozenset(hidden | hidden_names(root))
    dfl = root.find('defaults')
    if dfl is not None and dfl.find('axis-animation-vertex-order-xyz') is not None: xyz = True
    elif dfl is not None and dfl.find('axis-animation-vertex-order-x') is not None: xyz = False
    o = root.find('offsets')
    if o is not None and list(o):                        # a root <offsets> moves the model and its sub-models
        g = lambda tag: num(o.find(tag).text if o.find(tag) is not None else None)
        oT = [g('x-m'), g('y-m'), g('z-m')]
        T = [T[j] + m3v(R, oT)[j] for j in range(3)]
        R = m3mul(R, rot_hpr(g('heading-deg'), g('pitch-deg'), g('roll-deg')))
    subs = []
    for m in root.findall('model'):
        mp = m.find('path')
        if mp is None or not mp.text: continue
        if (m.findtext('name') or '').strip() in hidden: continue
        sub = fg_path(base, mp.text.strip())
        if skip and re.search(skip, sub.replace('\\', '/'), re.I): continue
        o = m.find('offsets'); oT = [0.0, 0.0, 0.0]; oR = I3
        if o is not None:
            g = lambda tag: num(o.find(tag).text if o.find(tag) is not None else None)
            oT = [g('x-m'), g('y-m'), g('z-m')]
            oR = rot_hpr(g('heading-deg'), g('pitch-deg'), g('roll-deg'))
        # <overlay><animation><type>scale</type>: FlightGear sizes shared instrument models to their panel slots with
        # a constant scale (property constants/zero, so scale = x/y/z-offset) about the model origin
        S = I3
        ov = m.find('overlay')
        if ov is not None:
            for an in ov.findall('animation'):
                if (an.findtext('type') or '').strip() != 'scale': continue
                if an.findall('object-name'): continue
                sv = [num(an.findtext(k), 1.0) for k in ('x-offset', 'y-offset', 'z-offset')]
                S = m3mul(S, [[sv[0], 0, 0], [0, sv[1], 0], [0, 0, sv[2]]])
        R2 = m3mul(m3mul(R, oR), S); T2 = [T[j] + m3v(R, oT)[j] for j in range(3)]
        subs.append(((m.findtext('name') or '').strip(), sub, R2, T2))
    p = root.find('path')
    acp = fg_path(base, p.text.strip()) if p is not None and p.text and p.text.strip().endswith('.ac') else None
    axis_src = ([(acp, R, T)] if acp else []) + [(sub, R2, T2) for _, sub, R2, T2 in subs if sub.endswith('.ac')]
    own = parse_anims(root, R, T, axis_src, xyz)
    if RANGE:                                            # --range: SimGear's range (LOD) animations on named objects
        for a_ in root.findall('animation'):
            if (a_.findtext('type') or '').strip() != 'range' or a_.find('min-m') is None: continue
            objs_ = frozenset(o.text.strip() for o in a_.findall('object-name') if o.text)
            if objs_: RANGE_ANIMS.append((objs_, num(a_.findtext('min-m')), num(a_.findtext('max-m'), 1e9)))
    if acp: out.append((acp, R, T, list(outer) + own + list(named), None, hidden))
    for name, sub, R2, T2 in subs:
        if ANIM_SUBMODELS:
            keep = [an for an in own if not const_true_select(an)]
            c_outer = list(outer) + [(None,) + tuple(an[1:]) for an in keep if name and name in an[0]]
            c_named = [an for an in keep if not (name and name in an[0])] + list(named)
        elif CONTROLS:
            # --controls without --anim-submodels: the controls' animations still move a whole sub-model named in them
            # (the Su-27's panel.xml turns its "Stick" model so); nothing else changes
            c_outer = list(outer) + [(None,) + tuple(an[1:]) for an in own if name and name in an[0]
                                     and an[1] & ~0x800 in CONTROL_PROPS.values() and not an[1] & 0x700]
            c_named = list(named)
        else: c_outer, c_named = [], []
        if sub.endswith('.ac'): out.append((sub, R2, T2, c_outer + c_named, None, hidden))
        elif sub.lower().endswith('.xml') and os.path.exists(sub):     # (glTF canvas screens: not supported)
            assemble(sub, R2, T2, skip, out, depth + 1, hidden, c_outer, c_named, xyz)


def split_tri(a, b, c, lim, depth=0):
    """vertices (pos, uv, nrm); halves the longest edge until every edge is at most lim metres (times the distance
    from the eye beyond 1 m: far parts need no fine ambient occlusion)"""
    if lim <= 0 or depth > 12: return [(a, b, c)]
    def d2(p, q): return sum((p[0][j] - q[0][j]) ** 2 for j in range(3))
    e = [(d2(a, b), 0), (d2(b, c), 1), (d2(c, a), 2)]
    l, k = max(e)
    near = min(sum(x * x for x in v[0]) for v in (a, b, c))
    lim_here = lim * max(1.0, math.sqrt(near))
    if l <= lim_here * lim_here: return [(a, b, c)]
    if k == 1: a, b, c = b, c, a
    elif k == 2: a, b, c = c, a, b
    # the edge a-b is the longest: split at its middle m
    n = [(a[2][j] + b[2][j]) * 0.5 for j in range(3)]
    ln = math.sqrt(sum(x * x for x in n)) or 1.0
    m = ([(a[0][j] + b[0][j]) * 0.5 for j in range(3)], ((a[1][0] + b[1][0]) * 0.5, (a[1][1] + b[1][1]) * 0.5),
         [x / ln for x in n])
    return split_tri(a, m, c, lim, depth + 1) + split_tri(m, b, c, lim, depth + 1)


def thr_table(fac, off, table, mil):
    """a throttle animation as a table over the runtime's lever (0 idle, 0.95 MIL, 1 afterburner) for an aircraft whose
    FlightGear throttle reaches MIL at mil"""
    fg = lambda t: t / 0.95 * mil if t <= 0.95 else mil + (t - 0.95) / 0.05 * (1.0 - mil)
    inv = lambda x: x / mil * 0.95 if x <= mil else 0.95 + (x - mil) / (1.0 - mil) * 0.05
    knots = {0.0, 0.95, 1.0} | {inv(x) for x, _ in table if 0.0 <= x <= 1.0}
    val = lambda x: interp(table, x) if table else x * fac + off
    return [(t, val(fg(t))) for t in sorted(knots)]


def smooth_tris(ev, surfs, rep, off, crease):
    """an AC3D object's polygons (vertices ev, eye frame) as triangles (pos, uv, normal) with normals smoothed within
    its crease angle (as main() does for the model's parts)"""
    polys, vfaces = [], {}
    for flags, mat, refs in surfs:
        if (flags & 15) != 0 or len(refs) < 3: continue
        nx = ny = nz = 0.0
        for k in range(len(refs)):
            a_, b_ = ev[refs[k][0]], ev[refs[(k + 1) % len(refs)][0]]
            nx += (a_[1] - b_[1]) * (a_[2] + b_[2]); ny += (a_[2] - b_[2]) * (a_[0] + b_[0]); nz += (a_[0] - b_[0]) * (a_[1] + b_[1])
        area = math.sqrt(nx * nx + ny * ny + nz * nz)
        fn = [nx / area, ny / area, nz / area] if area > 0 else [0.0, 1.0, 0.0]
        for vi, _, _ in refs: vfaces.setdefault(vi, []).append(len(polys))
        polys.append((refs, fn, area))
    cosc = math.cos(math.radians(min(max(crease, 0.0), 179.0)))
    tris = []
    for fi, (refs, fn, area) in enumerate(polys):
        pts = []
        for vi, u, v in refs:
            sx = sy = sz = 0.0
            for fj in vfaces[vi]:
                gn, ga = polys[fj][1], polys[fj][2]
                if fj == fi or fn[0] * gn[0] + fn[1] * gn[1] + fn[2] * gn[2] >= cosc:
                    sx += gn[0] * ga; sy += gn[1] * ga; sz += gn[2] * ga
            ln = math.sqrt(sx * sx + sy * sy + sz * sz)
            pts.append((ev[vi], (u * rep[0] + off[0], 1.0 - (v * rep[1] + off[1])), [sx / ln, sy / ln, sz / ln] if ln > 0 else fn))
        for k in range(1, len(pts) - 1): tris.append((pts[0], pts[k], pts[k + 1]))
    return tris


def rotvec(Rm):
    """rotation vector (axis * angle, radians) of a 3x3 rotation"""
    tr = Rm[0][0] + Rm[1][1] + Rm[2][2]
    ang = math.acos(max(-1.0, min(1.0, (tr - 1.0) * 0.5)))
    if ang < 1e-6: return [0.0, 0.0, 0.0]
    if ang > math.pi - 1e-4:                             # 180 degrees: the axis from the diagonal
        k = max(range(3), key=lambda i: Rm[i][i])
        ax = [0.0, 0.0, 0.0]; ax[k] = math.sqrt(max((Rm[k][k] + 1.0) * 0.5, 0.0))
        for j in range(3):
            if j != k: ax[j] = Rm[k][j] / (2.0 * ax[k])
        return [x * ang for x in ax]
    ax = [Rm[2][1] - Rm[1][2], Rm[0][2] - Rm[2][0], Rm[1][0] - Rm[0][1]]
    ln = math.sqrt(sum(x * x for x in ax))
    return [x / ln * ang for x in ax]


HAND_POSES = {'grip': 0, 'open': 1, 'point': 2, 'fist': 3}


def add_hands(path, spec, textures, meshes, anims):
    """--hands / --hand: the hand objects as meshes moved by matrix slots (see the file layout)"""
    mats, objs = parse_ac(path)
    texs, slot = {}, {}
    for name, tex, rep, off, wv, surfs, crease, anc in objs:
        m = re.match(r'hand_([LR])_(grip|open|point|fist)(_cuff)?$', name)
        if not m: continue
        side, pose = m.group(1), m.group(2)
        h = 1 if side == 'R' else 0
        if (side, pose) in slot:                         # (its cuff: the same slot)
            add_hand_mesh(path, name, tex, rep, off, wv, surfs, crease, mats, texs, textures, meshes, slot[side, pose])
            continue
        if m.group(3): continue
        chain, org, R = [], [0.0, 0.0, 0.0], I3
        ctrl = 0
        if pose == 'grip':
            if side not in spec: continue                # no control for this hand: only its free poses
            rx, pl = spec[side]
            hits = [mm for mm in meshes if rx.search(mm[0])]       # (an animated one first: names repeat)
            hit = next((mm for mm in hits if mm[4]), hits[0] if hits else None)
            if hit is None:
                print('--hand %s: no object matches %s: no grip pose' % (side, rx.pattern)); continue
            chain = list(hit[4])
            ctrl = 1 if h == 1 else 2
            pts = [v[0] for mm in hits if mm[4] == hit[4] for t in mm[5] for v in t]   # (all its materials' meshes)
            lo = [min(p[j] for p in pts) for j in range(3)]; hi = [max(p[j] for p in pts) for j in range(3)]
            org = list(pl[:3]) if pl and len(pl) >= 3 else [(lo[0] + hi[0]) * 0.5, hi[1] - 0.05, (lo[2] + hi[2]) * 0.5]
            yaw, pitch, roll = (list(pl[3:6]) + [0.0] * 3)[:3] if pl and len(pl) > 3 else (0.0, 0.0, 0.0)
            R0 = [[1.0, 0.0, 0.0], [0.0, 0.0, -1.0], [0.0, 1.0, 0.0]]     # columns X right, Y back, Z down
            cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
            cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
            cr, sr = math.cos(math.radians(roll)), math.sin(math.radians(roll))
            Ry = [[cy, 0.0, sy], [0.0, 1.0, 0.0], [-sy, 0.0, cy]]
            Rx = [[1.0, 0.0, 0.0], [0.0, cp, -sp], [0.0, sp, cp]]
            Rz = [[cr, -sr, 0.0], [sr, cr, 0.0], [0.0, 0.0, 1.0]]
            R = m3mul(Ry, m3mul(Rx, m3mul(Rz, R0)))
            print('hand %s on %s (%d animations): grip origin %.3f %.3f %.3f m, yaw %.0f pitch %.0f roll %.0f; the '
                  'object spans X %.3f..%.3f Y %.3f..%.3f Z %.3f..%.3f' % (side, hit[0], len(chain), org[0], org[1], org[2],
                                                                          yaw, pitch, roll, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))
        anims.append((0x1000 | h << 4 | HAND_POSES[pose], org, rotvec(R), 1.0, float(ctrl), [(float(k), 0.0) for k in chain]))
        slot[side, pose] = len(anims) - 1
        add_hand_mesh(path, name, tex, rep, off, wv, surfs, crease, mats, texs, textures, meshes, len(anims) - 1)


def add_hand_mesh(path, name, tex, rep, off, wv, surfs, crease, mats, texs, textures, meshes, k):
    ti = 0xFFFFFFFF
    if tex:
        tp = os.path.normpath(os.path.join(os.path.dirname(path), tex))
        if tp not in texs and os.path.exists(tp):
            tw, th, px = load_texture(tp); textures.append((tw, th, px)); texs[tp] = len(textures) - 1
        ti = texs.get(tp, 0xFFFFFFFF)
    mat = surfs[0][1] if surfs else 0
    rgba, spec_, shi = mats[mat][:3] if mat < len(mats) else ((1, 1, 1, 1), 0.0, 16.0)
    meshes.append((name[:31], ti, rgba, 16, [k], smooth_tris(wv, surfs, rep, off, crease), spec_, shi))


def main():
    a = sys.argv[1:]
    if '--controls' in a:                                # (first: --prop aliases may name the control properties)
        global CONTROLS
        CONTROLS = True; a.remove('--controls'); PROPS.update(CONTROL_PROPS)
    if '--gauge-translate' in a:
        global GAUGE_TRANSLATE
        GAUGE_TRANSLATE = True; a.remove('--gauge-translate')
    skip, subdiv, tex_dir = None, 0.0, None
    if '--skip' in a:
        k = a.index('--skip'); skip = a[k + 1]; del a[k:k + 2]
    if '--subdiv' in a:
        k = a.index('--subdiv'); subdiv = float(a[k + 1]); del a[k:k + 2]
    subdiv_extra = None                                  # --subdiv-extra: for the --extra (exterior) parts
    subdiv_ac = []                                       # --subdiv-ac REGEX=VALUE: for AC files matching REGEX
    while '--subdiv-ac' in a:
        k = a.index('--subdiv-ac'); rx_, val_ = a[k + 1].rsplit('=', 1); subdiv_ac.append((re.compile(rx_, re.I), float(val_)))
        del a[k:k + 2]
    tex_obj = []                                         # --tex-obj REGEX=PATH: texture of the objects matching REGEX
    while '--tex-obj' in a:
        k = a.index('--tex-obj'); rx_, val_ = a[k + 1].split('=', 1)
        tex_obj.append((re.compile(rx_, re.I), val_))
        del a[k:k + 2]
    screens = []                                         # --screen REGEX=ID[,LIFT]: display screens (runtime screen id)
    while '--screen' in a:
        k = a.index('--screen'); rx_, val_ = a[k + 1].rsplit('=', 1); id_, _, lift_ = val_.partition(',')
        if not 1 <= int(id_) <= 255: raise SystemExit('--screen %s: the id must be 1..255' % a[k + 1])
        screens.append((re.compile(rx_, re.I), int(id_), float(lift_) if lift_ else 0.0))
        del a[k:k + 2]
    scr_tex = None                                       # the screens' shared dark "off" texture
    spec_ac = []                                         # --spec-ac REGEX=VALUE: specular of AC files matching REGEX
    while '--spec-ac' in a:
        k = a.index('--spec-ac'); rx_, val_ = a[k + 1].rsplit('=', 1); spec_ac.append((re.compile(rx_, re.I), float(val_)))
        del a[k:k + 2]
    spec_obj = []                                        # --spec-obj REGEX=VALUE[,SHININESS] on object names
    while '--spec-obj' in a:
        k = a.index('--spec-obj'); rx_, val_ = a[k + 1].rsplit('=', 1); sv_, _, sh_ = val_.partition(',')
        spec_obj.append((re.compile(rx_, re.I), float(sv_), float(sh_) if sh_ else None)); del a[k:k + 2]
    if '--subdiv-extra' in a:
        k = a.index('--subdiv-extra'); subdiv_extra = float(a[k + 1]); del a[k:k + 2]
    hide_obj_rx = None                                   # --hide-obj REGEX on object names (a duplicate glass layer)
    if '--hide-obj' in a:
        k = a.index('--hide-obj'); hide_obj_rx = re.compile(a[k + 1], re.I); del a[k:k + 2]
    flat_obj_rx = None                                   # --flatten-obj REGEX (ck_polish 2026-10-31, bug #259)
    if '--flatten-obj' in a:
        k = a.index('--flatten-obj'); flat_obj_rx = re.compile(a[k + 1]); del a[k:k + 2]
    if '--axis-flip' in a:                               # (ck_polish 2026-10-31, bug #258)
        k = a.index('--axis-flip'); globals()['AXIS_FLIP'] = re.compile(a[k + 1]); del a[k:k + 2]
    drop_pieces = []                                     # --drop-pieces REGEX@X0,X1,Y0,Y1,Z0,Z1 (ck_fit 2026-10-28)
    while '--drop-pieces' in a:
        k = a.index('--drop-pieces'); r_, _, b_ = a[k + 1].rpartition('@')
        drop_pieces.append((re.compile(r_, re.I), [float(t_) for t_ in b_.split(',')])); del a[k:k + 2]
    glass_obj = []                                       # --glass-obj REGEX[=ALPHA]: these objects are glass
    while '--glass-obj' in a:
        k = a.index('--glass-obj'); rx_, _, val_ = a[k + 1].partition('=')
        glass_obj.append((re.compile(rx_, re.I), min(max(float(val_), 0.0), 0.15) if val_ else 0.0))
        del a[k:k + 2]
    amb_obj_rx = None                                   # --ambient-obj REGEX: material ambient where brighter
    if '--ambient-obj' in a:
        k = a.index('--ambient-obj'); amb_obj_rx = re.compile(a[k + 1], re.I); del a[k:k + 2]
    mat_obj = []                                         # --mat-obj REGEX=KIND[@TILE][:R,G,B]: surface texture
    while '--mat-obj' in a:
        k = a.index('--mat-obj'); rx_, val_ = a[k + 1].rsplit('=', 1)
        val_, _, col_ = val_.partition(':'); kind_, _, tile_ = val_.partition('@')
        if kind_ not in MAT_KINDS:
            raise SystemExit('--mat-obj %s: the kind must be one of %s' % (a[k + 1], ', '.join(MAT_KINDS)))
        mat_obj.append((re.compile(rx_, re.I), kind_, float(tile_) if tile_ else 0.25,
                        tuple(float(c_) for c_ in col_.split(',')) if col_ else None))
        del a[k:k + 2]
    mat_box = []                                         # --mat-box R=KIND[@TILE]:R,G,B@BOX[@N,D]: regions
    while '--mat-box' in a:
        k = a.index('--mat-box'); rx_, val_ = a[k + 1].split('=', 1)
        val_, _, pc_ = val_.partition('#')
        pc_ = (pc_[0], int(pc_[1:])) if pc_ else None    # ('<' or '>', faces of the connected part)
        fs_ = val_.split('@')
        kind_, _, col_ = fs_[0].partition(':')
        tile_ = 0.25
        if ':' not in fs_[0]:                            # KIND@TILE:R,G,B -> fields KIND, TILE:R,G,B
            tile_, _, col_ = fs_[1].partition(':'); tile_ = float(tile_); fs_ = fs_[1:]
        if kind_ not in MAT_KINDS or not col_ or len(fs_) < 2:
            raise SystemExit('--mat-box %s: KIND[@TILE]:R,G,B@X0,Y0,Z0,X1,Y1,Z1[@NX,NY,NZ,D]' % a[k + 1])
        bx_ = [float(c_) for c_ in fs_[1].split(',')]
        nd_ = [float(c_) for c_ in fs_[2].split(',')] if len(fs_) > 2 else None
        mat_box.append((re.compile(rx_, re.I), kind_, tile_, tuple(float(c_) for c_ in col_.split(',')),
                        [min(bx_[0], bx_[3]), min(bx_[1], bx_[4]), min(bx_[2], bx_[5])],
                        [max(bx_[0], bx_[3]), max(bx_[1], bx_[4]), max(bx_[2], bx_[5])], nd_, pc_))
        del a[k:k + 2]
    mat_tex = {}                                         # kind -> (texture index, mean texel value)
    blank_rx = hide_rx = None                            # --blank / --hide-tex REGEX on texture file names
    if '--blank' in a:
        k = a.index('--blank'); blank_rx = re.compile(a[k + 1], re.I); del a[k:k + 2]
    if '--hide-tex' in a:
        k = a.index('--hide-tex'); hide_rx = re.compile(a[k + 1], re.I); del a[k:k + 2]
    if '--fold-selects' in a:
        global FOLD_SELECTS
        FOLD_SELECTS = True; a.remove('--fold-selects')
    if '--texrotate' in a:
        global TEXROTATE
        TEXROTATE = True; a.remove('--texrotate')
    if '--aircraft-root' in a:
        global AIRCRAFT_ROOT
        k = a.index('--aircraft-root'); AIRCRAFT_ROOT = a[k + 1]; del a[k:k + 2]
    screen_eye = []                                      # ck_look 2026-10-30: --screen-eye
    while '--screen-eye' in a:
        k = a.index('--screen-eye'); screen_eye.append(re.compile(a[k + 1], re.I)); del a[k:k + 2]
    opaque_tex = []                                      # ck_look 2026-10-30: --opaque-tex
    while '--opaque-tex' in a:
        k = a.index('--opaque-tex'); opaque_tex.append(re.compile(a[k + 1], re.I)); del a[k:k + 2]
    tex_dirs = []
    while '--tex-dir' in a:
        k = a.index('--tex-dir'); tex_dirs.append(a[k + 1]); del a[k:k + 2]
    max_tex, max_tex_for = 2048, []
    if '--max-tex' in a:
        k = a.index('--max-tex'); max_tex = int(a[k + 1]); del a[k:k + 2]
    while '--max-tex-for' in a:
        k = a.index('--max-tex-for'); max_tex_for.append((re.compile(a[k + 1], re.I), int(a[k + 2]))); del a[k:k + 3]
    max_tex_strip = []
    while '--max-tex-strip' in a:
        k = a.index('--max-tex-strip'); max_tex_strip.append((re.compile(a[k + 1], re.I), int(a[k + 2]))); del a[k:k + 3]
    while '--const' in a:                                # per-aircraft constants and aliases (before the XML is read)
        k = a.index('--const'); n_, v_ = a[k + 1].rsplit('=', 1); n_ = n_.strip().lstrip('/')
        try: CONSTS[n_] = float(v_)
        except ValueError: SCONSTS[n_] = v_
        del a[k:k + 2]
    for flag_, var_ in (('--anim-submodels', 'ANIM_SUBMODELS'), ('--axis-objects', 'AXIS_OBJECTS'),
                        ('--group-names', 'GROUP_NAMES'), ('--tex-paths', 'TEX_PATHS'),
                        ('--axis-fg-sense', 'AXIS_FG_SENSE'), ('--range', 'RANGE'),
                        ('--axis-sg-order', 'AXIS_SG_ORDER')):
        if flag_ in a: globals()[var_] = True; a.remove(flag_)
    if '--const-selects' in a:
        global CONST_SELECTS
        CONST_SELECTS = True; a.remove('--const-selects')
    if '--ear-clip' in a:
        global EAR_CLIP
        EAR_CLIP = True; a.remove('--ear-clip')
    if '--sort-tables' in a:
        global SORT_TABLES
        SORT_TABLES = True; a.remove('--sort-tables')
    while '--prop' in a:
        k = a.index('--prop'); n_, t_ = a[k + 1].split('=', 1); t_, _, s_ = t_.partition('*')
        n_ = n_.strip().lstrip('/'); PROPS[n_] = PROPS[t_.strip().lstrip('/')]
        if s_: PSCALE[n_] = float(s_)
        del a[k:k + 2]
    thr_mil = None
    if '--throttle-mil' in a:
        k = a.index('--throttle-mil'); thr_mil = float(a[k + 1]); del a[k:k + 2]
    add_models = []                                      # --add-model XML@X,Y,Z[,H,P,R[,S]]
    while '--add-model' in a:
        k = a.index('--add-model'); x_, _, o_ = a[k + 1].rpartition('@')
        v_ = [float(t_) for t_ in o_.split(',')] + [0.0] * 3
        add_models.append((x_, v_[:3], v_[3:6], v_[6] if len(v_) > 9 else 1.0)); del a[k:k + 2]
    add_glass = []                                       # --add-glass NAME=X,Y,Z;X,Y,Z;X,Y,Z;X,Y,Z (FlightGear frame)
    while '--add-glass' in a:
        k = a.index('--add-glass'); n_, _, c_ = a[k + 1].partition('=')
        add_glass.append((n_, [[float(t_) for t_ in q_.split(',')] for q_ in c_.split(';')])); del a[k:k + 2]
    hands_ac, hand_spec, hands_hide = None, {}, None
    if '--hands-hide' in a:                              # the model's own (pilot) hands: hidden while the gloves show
        k = a.index('--hands-hide'); hands_hide = re.compile(a[k + 1], re.I); del a[k:k + 2]
    if '--hands' in a:
        k = a.index('--hands'); hands_ac = a[k + 1]; del a[k:k + 2]
    while '--hand' in a:
        k = a.index('--hand'); side_, _, rest_ = a[k + 1].partition('=')
        rx_, _, pl_ = rest_.partition('@')
        hand_spec[side_.strip().upper()] = (re.compile(rx_, re.I), [float(t_) for t_ in pl_.split(',')] if pl_ else None)
        del a[k:k + 2]
    extras = []
    while '--extra' in a:
        k = a.index('--extra'); extras.append((a[k + 1], a[k + 2])); del a[k:k + 3]
    xml_path, ex, ey, ez, outp = a[0], float(a[1]), float(a[2]), float(a[3]), a[4]
    parts = []
    assemble(xml_path, I3, [0, 0, 0], skip, parts)
    for x_, t_, r_, s_ in add_models:                    # --add-model: as a <model> with <offsets> (and a scale)
        assemble(os.path.normpath(x_), m3mul(rot_hpr(*r_), [[s_, 0, 0], [0, s_, 0], [0, 0, s_]]), t_, skip, parts)
    if AXIS_SG_ORDER:
        print('--axis-sg-order: %d axis lines (%d ordered by X, Y, Z, %d swapped); %d point from their first vertex to '
              'their second' % tuple(AXIS_SG_STATS))
    for acp, rx in extras:
        parts.append((os.path.normpath(acp), I3, [0, 0, 0], [], re.compile(rx, re.I)))
    tex_ids, textures, meshes, anims = {}, [], [], []
    invisible = set()                                    # textures with no visible texel
    # glass: translucent materials (FlightGear draws alpha 0.95-0.99 as opaque: F-15 knobs at 0.988)
    obj_skip = re.compile(r'^HDR|reflect|shadow', re.I)    # effect duplicates; glass is kept (translucent material)

    def to_eye_p(pf):
        return [pf[1] - ey, pf[2] - ez, pf[0] - ex]

    def to_eye_d(d):
        return [d[1], d[2], d[0]]

    range_hide = set()                                   # --range: names (objects, AC3D groups) left out
    if RANGE_ANIMS:
        # SimGear's range animation is an LOD node: its objects are drawn while the eye's distance from their centre
        # lies in [min-m, max-m); the centre here = the mean of all their vertices in every part (the design eye at 0)
        acc = [[0.0, 0.0, 0.0, 0] for _ in RANGE_ANIMS]
        for part in parts:
            acp, R, T = part[:3]
            only = part[4] if len(part) > 4 else None
            for name, _, _, _, wv, _, _, anc in ac_cached(acp)[1]:
                if only is not None and not only.search(name): continue
                nm_ = set(anc) | {name}
                for k_, (objs_, _, _) in enumerate(RANGE_ANIMS):
                    if not objs_ & nm_: continue
                    for p0 in wv:
                        pf = [x + y for x, y in zip(m3v(R, m3v(AC_TO_FG, p0)), T)]
                        e_ = to_eye_p(pf)
                        for j in range(3): acc[k_][j] += e_[j]
                        acc[k_][3] += 1
        for (objs_, lo_, hi_), a_ in zip(RANGE_ANIMS, acc):
            if not a_[3]: continue
            d_ = math.sqrt(sum((a_[j] / a_[3]) ** 2 for j in range(3)))
            if lo_ <= d_ < hi_: continue
            range_hide |= objs_
            print('range: %s left out: %.2f m from the eye, drawn %.1f..%.0f m (%d vertices)'
                  % (', '.join(sorted(objs_)), d_, lo_, hi_, a_[3]))

    for part in parts:
        acp, R, T, part_anims = part[:4]
        only = part[4] if len(part) > 4 else None
        hide = part[5] if len(part) > 5 else frozenset()  # names hidden by a constant select up the XML tree
        # this part's rotate animations, in declaration order (the first one is the outermost transform)
        aidx = []
        texrot = []                                      # --texrotate: rotations resolved from their part below
        mats, objs_ac = ac_cached(acp)
        names_ac = {ob[0] for ob in objs_ac} | ({g for ob in objs_ac for g in ob[7]} if GROUP_NAMES else set())
        for objs, prop, cw, aw, fac, off, table in part_anims:
            if ANIM_SUBMODELS and objs is not None and not names_ac.intersection(objs): continue  # (inherited, unused)
            if prop & 0x400:                             # until resolved: no rotation
                anims.append((P_ZERO, [0.0, 0.0, 0.0], [0.0, 0.0, 1.0], 0.0, 0.0, []))
                texrot.append([len(anims) - 1, set(objs or ()), cw, (prop & 0xFF, fac, off, table), False])
            elif prop & 0x300: anims.append((prop, cw, aw, fac, off, table))     # texture space / select: as is
            else: anims.append((prop, to_eye_p(cw), to_eye_d(aw), fac, off, table))
            aidx.append((None if objs is None else set(objs), len(anims) - 1))     # None: every object
        objs = objs_ac
        for name, tex, rep, off, wv, surfs, crease, anc in objs:
            # --group-names: an object also answers to the names of its AC3D groups (SimGear animates and hides a
            # group with everything in it); names from the outermost group in
            names = (anc + (name,)) if GROUP_NAMES else (name,)
            if obj_skip.search(name): continue
            if only is not None and not only.search(name): continue
            if hide.intersection(names): continue
            if hide_obj_rx and hide_obj_rx.search(name): continue
            if range_hide and range_hide.intersection(anc + (name,)): continue     # --range
            scr, scr_lift = next(((i_, l_) for rx_, i_, l_ in screens if any(rx_.search(n_) for n_ in names)), (0, 0.0))
            ti = 0xFFFFFFFF
            tov = next((t_ for rx_, t_ in tex_obj if any(rx_.search(n_) for n_ in names)), None)
            gla = next((a_ for rx_, a_ in glass_obj if rx_.search(name)), None)   # --glass-obj: no texture
            if gla is not None: pass
            elif scr:                                    # --screen: drawn by the runtime; this texture shows it off
                if scr_tex is None:
                    textures.append((4, 4, bytes((20, 22, 24, 255)) * 16)); scr_tex = len(textures) - 1
                ti = scr_tex
            elif tex or tov:
                tp = os.path.normpath(os.path.join(os.path.dirname(acp), tex)) if tex else ''
                if tov: tp = os.path.normpath(os.path.join(AIRCRAFT_ROOT or '', tov))   # --tex-obj (a livery's)
                elif TEX_PATHS and not os.path.exists(tp): tp = tex_fallback(acp, tex, tp)
                tq = next((q_ for q_ in (os.path.join(d_, os.path.basename(tp)) for d_ in tex_dirs)
                           if os.path.exists(q_)), None)
                src = tq if tq and os.path.exists(tq) else tp
                if tp not in tex_ids and os.path.exists(src):
                    try:
                        tw, th, px = load_texture(src)
                        if any(rx.search(tp.replace(os.sep, '/')) for rx in opaque_tex):     # --opaque-tex
                            px = bytearray(px); px[3::4] = b'\xff' * (tw * th); px = bytes(px)
                        lim = min([n for rx, n in max_tex_for if rx.search(tp.replace(os.sep, '/'))] + [max_tex])
                        lim_s = next((n for rx, n in max_tex_strip if rx.search(tp.replace(os.sep, '/'))), None)
                        if lim_s:                                 # a tape: the long side up to lim_s
                            while max(tw, th) > lim_s or min(tw, th) > lim:
                                tw, th, px = half_rgba(tw, th, px)
                        while not lim_s and (tw > lim or th > lim):
                            tw, th, px = half_rgba(tw, th, px)
                        if not any(px[3::4]):                     # fully transparent: FlightGear pick areas
                            invisible.add(tp)
                            continue
                        low = os.path.basename(tp).lower()
                        if hide_rx and hide_rx.search(low):       # e.g. a reflector glass with a placeholder scribble
                            invisible.add(tp)
                            continue
                        if blank_rx and blank_rx.search(low):    # a display placeholder under another name
                            low = 'canvas-' + low
                        if 'canvas' in low:                       # placeholder of a FlightGear canvas display
                            if 'hud' in low:                      # the game draws its own HUD: no panel on the glass
                                invisible.add(tp)
                                continue
                            px = bytes((20, 22, 24, 255)) * (tw * th)        # a dark, switched-off screen
                        textures.append((tw, th, px)); tex_ids[tp] = len(textures) - 1
                    except Exception as e:
                        print('texture skipped:', e)
                if tp in invisible: continue                       # invisible in FlightGear too
                ti = tex_ids.get(tp, 0xFFFFFFFF)
            # --mat-obj: an untextured part (not glass, not a screen) gets a generated surface texture on texture
            # coordinates projected from its own faces (box mapping in the eye frame, TILE metres a repeat)
            mob = None
            if mat_obj and ti == 0xFFFFFFFF and gla is None and not scr:
                mob = next((m_ for m_ in mat_obj if any(m_[0].search(n_) for n_ in names)), None)
                if mob is not None:
                    if mob[1] not in mat_tex:
                        tw_, px_, mean_ = mat_texture(mob[1])
                        textures.append((tw_, tw_, px_)); mat_tex[mob[1]] = (len(textures) - 1, mean_)
                    ti = mat_tex[mob[1]][0]
            # vertices in the eye frame: X right = fg y, Y up = fg z, Z back = fg x (aft)
            ev = []
            for p0 in wv:
                pf = m3v(AC_TO_FG, p0)                                  # FG model coordinates
                pf = [x + y for x, y in zip(m3v(R, pf), T)]
                ev.append([pf[1] - ey, pf[2] - ez, pf[0] - ex])
            if flat_obj_rx and flat_obj_rx.search(name) and len(ev) >= 3:    # --flatten-obj (ck_polish, bug #259)
                # the plane: through the vertices' mean, normal = the sum of the faces' Newell normals (area-weighted)
                sn = [0.0, 0.0, 0.0]
                for flags, mat, refs in surfs:
                    if (flags & 15) != 0 or len(refs) < 3: continue
                    for k in range(len(refs)):
                        a_, b_ = ev[refs[k][0]], ev[refs[(k + 1) % len(refs)][0]]
                        sn[0] += (a_[1] - b_[1]) * (a_[2] + b_[2]); sn[1] += (a_[2] - b_[2]) * (a_[0] + b_[0])
                        sn[2] += (a_[0] - b_[0]) * (a_[1] + b_[1])
                ln = math.sqrt(sum(c_ * c_ for c_ in sn))
                if ln > 0:
                    sn = [c_ / ln for c_ in sn]
                    cen = [sum(p_[j] for p_ in ev) / len(ev) for j in range(3)]
                    dev = [sum((p_[j] - cen[j]) * sn[j] for j in range(3)) for p_ in ev]
                    ev = [[p_[j] - d_ * sn[j] for j in range(3)] for p_, d_ in zip(ev, dev)]
                    print('--flatten-obj: %s onto its plane (centre %.3f %.3f %.3f, normal %.3f %.3f %.3f): vertices '
                          'moved up to %.1f mm (rms %.1f)' % (name, cen[0], cen[1], cen[2], sn[0], sn[1], sn[2],
                                                              1000 * max(abs(d_) for d_ in dev),
                                                              1000 * math.sqrt(sum(d_ * d_ for d_ in dev) / len(dev))))
            for rx_, bx_ in drop_pieces:                 # --drop-pieces: this object's pieces inside the box
                if not rx_.search(name): continue
                par_ = list(range(len(ev))); seen_ = {}
                def fd_(q_):
                    while par_[q_] != q_: par_[q_] = par_[par_[q_]]; q_ = par_[q_]
                    return q_
                for q_, p_ in enumerate(ev):             # coincident vertices (UV seams) join their pieces
                    kk_ = tuple(round(c_, 4) for c_ in p_)
                    if kk_ in seen_: par_[fd_(q_)] = fd_(seen_[kk_])
                    else: seen_[kk_] = q_
                for _f, _m, refs_ in surfs:
                    for r_ in refs_[1:]:
                        x_, y_ = fd_(refs_[0][0]), fd_(r_[0])
                        if x_ != y_: par_[x_] = y_
                inb_ = {}
                for q_, p_ in enumerate(ev):
                    g_ = fd_(q_)
                    inb_[g_] = inb_.get(g_, True) and all(bx_[2 * j_] <= p_[j_] <= bx_[2 * j_ + 1] for j_ in range(3))
                n0_ = len(surfs)
                surfs = [s_ for s_ in surfs if not (s_[2] and inb_.get(fd_(s_[2][0][0]), False))]
                print('--drop-pieces: %s: %d of %d faces in %d pieces inside the box left out' % (
                    name, n0_ - len(surfs), n0_, len({fd_(q_) for q_ in range(len(ev)) if inb_[fd_(q_)]})))
            if scr and scr_lift and ev:                  # --screen ...,LIFT: towards the eye along the screen's face
                nx = ny = nz = 0.0
                for flags, mat, refs in surfs:
                    if (flags & 15) != 0 or len(refs) < 3: continue
                    for k in range(len(refs)):
                        a_, b_ = ev[refs[k][0]], ev[refs[(k + 1) % len(refs)][0]]
                        nx += (a_[1] - b_[1]) * (a_[2] + b_[2]); ny += (a_[2] - b_[2]) * (a_[0] + b_[0])
                        nz += (a_[0] - b_[0]) * (a_[1] + b_[1])
                ln = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
                cen = [sum(p_[j] for p_ in ev) / len(ev) for j in range(3)]
                sg = -1.0 if nx * cen[0] + ny * cen[1] + nz * cen[2] > 0 else 1.0    # the side facing the eye
                ev = [[p_[j] + sg * scr_lift * (nx, ny, nz)[j] / ln for j in range(3)] for p_ in ev]
            for tr in texrot:                            # --texrotate: pivot and axis from the part's first object
                if tr[4] or name not in tr[1]: continue
                pv = disc_pivot(ev, surfs, rep, off, tr[2][0], tr[2][1])
                tr[4] = True
                if pv is None:
                    print('texrotate: %s is not a disc about the rotation centre: left still' % name)
                    continue
                pid, fac_, off_, table_ = tr[3]
                sgn = -1.0 if tr[2][2] >= 0 else 1.0      # the picture turns opposite to the texture coordinates
                anims[tr[0]] = (pid, pv[0], [sgn * x for x in pv[1]], fac_, off_, table_)
            # face normals (Newell) and, per vertex, the faces using it: smooth normals within the crease angle
            polys_all = []
            vfaces = {}
            for flags, mat, refs in surfs:
                if (flags & 15) != 0 or len(refs) < 3: continue      # lines
                nx = ny = nz = 0.0
                for k in range(len(refs)):
                    a_, b_ = ev[refs[k][0]], ev[refs[(k + 1) % len(refs)][0]]
                    nx += (a_[1] - b_[1]) * (a_[2] + b_[2]); ny += (a_[2] - b_[2]) * (a_[0] + b_[0])
                    nz += (a_[0] - b_[0]) * (a_[1] + b_[1])
                area = math.sqrt(nx * nx + ny * ny + nz * nz)
                fn = [nx / area, ny / area, nz / area] if area > 0 else [0.0, 1.0, 0.0]
                fi = len(polys_all)
                polys_all.append((flags, mat, refs, fn, area))
                for vi, _, _ in refs: vfaces.setdefault(vi, []).append(fi)
            cosc = math.cos(math.radians(min(max(crease, 0.0), 179.0)))
            by_mat = {}
            boxes = [b_ for b_ in mat_box if any(b_[0].search(n_) for n_ in names)] if gla is None and not scr else []
            psize = None
            if any(b_[7] for b_ in boxes):               # faces per connected part (vertex indices shared)
                par_ = {}
                def fnd_(x_):
                    while par_.setdefault(x_, x_) != x_:
                        par_[x_] = par_.setdefault(par_[x_], par_[x_]); x_ = par_[x_]
                    return x_
                for _, _, refs_, _, _ in polys_all:
                    for vi_, _, _ in refs_[1:]: par_[fnd_(vi_)] = fnd_(refs_[0][0])
                cnt_ = {}
                for _, _, refs_, _, _ in polys_all: r_ = fnd_(refs_[0][0]); cnt_[r_] = cnt_.get(r_, 0) + 1
                psize = [cnt_[fnd_(refs_[0][0])] for _, _, refs_, _, _ in polys_all]
            for fi, (flags, mat, refs, fn, area) in enumerate(polys_all):
                pts = []
                reg = -1                                 # --mat-box: the first box holding the face's centre
                if boxes:
                    cf = [sum(ev[vi][j] for vi, _, _ in refs) / len(refs) for j in range(3)]
                    for bi_, b_ in enumerate(boxes):
                        if all(b_[4][j] <= cf[j] <= b_[5][j] for j in range(3)) and (
                                b_[6] is None or fn[0] * b_[6][0] + fn[1] * b_[6][1] + fn[2] * b_[6][2] >= b_[6][3]) and (
                                b_[7] is None or (psize[fi] < b_[7][1] if b_[7][0] == '<' else psize[fi] > b_[7][1])):
                            reg = mat_box.index(b_)
                            break
                for vi, u, v in refs:
                    sx = sy = sz = 0.0
                    for fj in vfaces[vi]:
                        gn, ga = polys_all[fj][3], polys_all[fj][4]
                        if fj == fi or fn[0] * gn[0] + fn[1] * gn[1] + fn[2] * gn[2] >= cosc:
                            sx += gn[0] * ga; sy += gn[1] * ga; sz += gn[2] * ga
                    ln = math.sqrt(sx * sx + sy * sy + sz * sz)
                    vn = [sx / ln, sy / ln, sz / ln] if ln > 0 else fn
                    if reg >= 0:                         # --mat-box: as --mat-obj, its own tile
                        ax = max(range(3), key=lambda j_: abs(fn[j_]))
                        i0, i1 = ((1, 2), (0, 2), (0, 1))[ax]
                        pts.append((ev[vi], (ev[vi][i0] / mat_box[reg][2], ev[vi][i1] / mat_box[reg][2]), vn))
                        continue
                    if mob is not None:                  # --mat-obj: the face's dominant axis picks the projection
                        ax = max(range(3), key=lambda j_: abs(fn[j_]))
                        i0, i1 = ((1, 2), (0, 2), (0, 1))[ax]
                        pts.append((ev[vi], (ev[vi][i0] / mob[2], ev[vi][i1] / mob[2]), vn))
                        continue
                    pts.append((ev[vi], (u * rep[0] + off[0], 1.0 - (v * rep[1] + off[1])), vn))
                by_mat.setdefault((mat, flags & 0x20, reg), []).append(pts)
            if scr and by_mat:                           # --screen: texture coordinates 0..1 over the object's range,
                uvs = [p_[1] for polys in by_mat.values() for pts in polys for p_ in pts]
                if any(rx_.search(name) for rx_ in screen_eye):            # --screen-eye (ck_look): the direction
                    by_mat = {k_: [[(p_[0], (math.atan2(p_[0][0], -p_[0][2]),     # from the eye: u azimuth to the
                                             -math.atan2(p_[0][1], math.hypot(p_[0][0], p_[0][2]))), p_[2])   # right,
                                    for p_ in pts] for pts in polys] for k_, polys in by_mat.items()}  # v elevation down
                    uvs = [p_[1] for polys in by_mat.values() for pts in polys for p_ in pts]
                    print('screen %d: %s: texture coordinates from the design eye (--screen-eye)' % (scr, name))
                elif (max(t_[0] for t_ in uvs) - min(t_[0] for t_ in uvs) < 1e-6
                        or max(t_[1] for t_ in uvs) - min(t_[1] for t_ in uvs) < 1e-6):
                    # no texture coordinates on the screen (the F-22's MFD faces: all 0): planar ones from the face --
                    # u along the pilot's right, v down, both in the face's plane (the orientation is checked below)
                    nn = [0.0, 0.0, 0.0]
                    for polys in by_mat.values():
                        for pts in polys:
                            for k in range(1, len(pts) - 1):
                                q0, q1, q2 = pts[0][0], pts[k][0], pts[k + 1][0]
                                e1 = [q1[j] - q0[j] for j in range(3)]; e2 = [q2[j] - q0[j] for j in range(3)]
                                cr = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
                                sg = 1.0 if sum(cr[j] * (-q0[j]) for j in range(3)) >= 0 else -1.0   # towards the eye
                                nn = [nn[j] + sg * cr[j] for j in range(3)]
                    ln_ = math.sqrt(sum(c_ * c_ for c_ in nn)) or 1.0
                    nn = [c_ / ln_ for c_ in nn]
                    ua = [1.0 - nn[0] * nn[0], -nn[0] * nn[1], -nn[0] * nn[2]]          # x (right) in the plane
                    la = math.sqrt(sum(c_ * c_ for c_ in ua)) or 1.0
                    ua = [c_ / la for c_ in ua]
                    va = [nn[1] * ua[2] - nn[2] * ua[1], nn[2] * ua[0] - nn[0] * ua[2], nn[0] * ua[1] - nn[1] * ua[0]]
                    if va[1] > 0: va = [-c_ for c_ in va]                               # v points down the screen
                    by_mat = {k_: [[(p_[0], (sum(p_[0][j] * ua[j] for j in range(3)), sum(p_[0][j] * va[j] for j in range(3))),
                                     p_[2]) for p_ in pts] for pts in polys] for k_, polys in by_mat.items()}
                    uvs = [p_[1] for polys in by_mat.values() for pts in polys for p_ in pts]
                    print('screen %d: %s: no texture coordinates: planar ones from the face' % (scr, name))
                u0, u1 = min(t_[0] for t_ in uvs), max(t_[0] for t_ in uvs)
                v0, v1 = min(t_[1] for t_ in uvs), max(t_[1] for t_ in uvs)
                du, dv = (u1 - u0) or 1.0, (v1 - v0) or 1.0
                # ... u to the pilot's right and v down the screen (down = -y, and towards the pilot for a screen
                # lying back): a mirrored or turned mapping (the F-14's HSD) is undone. gu, gv: dP/du, dP/dv
                gu, gv = [0.0] * 3, [0.0] * 3
                for polys in by_mat.values():
                    for pts in polys:
                        for k in range(1, len(pts) - 1):
                            (q0, t0, _), (q1, t1, _), (q2, t2, _) = pts[0], pts[k], pts[k + 1]
                            e1 = [q1[j] - q0[j] for j in range(3)]; e2 = [q2[j] - q0[j] for j in range(3)]
                            du1, dv1, du2, dv2 = t1[0] - t0[0], t1[1] - t0[1], t2[0] - t0[0], t2[1] - t0[1]
                            det = du1 * dv2 - du2 * dv1
                            cr = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
                            ar = 0.5 * math.sqrt(sum(c_ * c_ for c_ in cr))
                            if abs(det) < 1e-12 or ar <= 0: continue
                            for j in range(3):
                                gu[j] += ar * (e1[j] * dv2 - e2[j] * dv1) / det
                                gv[j] += ar * (e2[j] * du1 - e1[j] * du2) / det
                ru, rv, du_, dv_ = gu[0], gv[0], gu[2] - gu[1], gv[2] - gv[1]
                swap = abs(rv) + abs(du_) > abs(ru) + abs(dv_)
                fu, fv = ((rv if swap else ru) < 0), ((du_ if swap else dv_) < 0)
                if swap or fu or fv: print('screen %d: %s: texture mapping%s%s%s' % (scr, name, ' turned' if swap else '',
                                                                           ' mirrored left-right' if fu else '',
                                                                           ' upside down' if fv else ''))
                def scr_uv(t_):
                    a_, b_ = (t_[0] - u0) / du, (t_[1] - v0) / dv
                    if swap: a_, b_ = b_, a_
                    return (1.0 - a_ if fu else a_, 1.0 - b_ if fv else b_)
                by_mat = {k_: [[(p_[0], scr_uv(p_[1]), p_[2]) for p_ in pts] for pts in polys]
                          for k_, polys in by_mat.items()}
            for (mat, two, reg), polys in by_mat.items():
                rgba, spec, shi = mats[mat][:3] if mat < len(mats) else ((1, 1, 1, 1), 0.0, 16.0)
                if scr: rgba = (1.0, 1.0, 1.0, 1.0)      # a screen is opaque (FlightGear's are often fully transparent)
                for rx_, val_ in spec_ac:
                    if rx_.search(acp.replace(os.sep, '/')): spec = val_
                so_ = next(((v_, h_) for rx_, v_, h_ in spec_obj if rx_.search(name)), None)
                if so_: spec, shi = so_[0], (so_[1] if so_[1] is not None else shi)
                if amb_obj_rx and mat < len(mats) and amb_obj_rx.search(name):
                    # FlightGear lights a material by its ambient colour too: a near-black diffuse with a 0.5 ambient
                    # (the F-20A's attitude indicator) shows mid grey there
                    rgba = tuple(max(c, am) for c, am in zip(rgba[:3], mats[mat][3])) + (rgba[3],)
                if gla is not None: rgba = (rgba[0], rgba[1], rgba[2], gla)     # --glass-obj
                elif rgba[3] < 0.95:
                    # fully transparent material: FlightGear's invisible pick areas, unless it is glass drawn by an
                    # effect there (canopy, windscreen, instrument glass); glass at most 15 % opaque (the F-16's gold
                    # outer canopy at 45 % tinted the whole view)
                    if rgba[3] <= 0.001 and not re.search(r'glass|canopy|windscreen|vitre|hud', name, re.I): continue
                    rgba = (rgba[0], rgba[1], rgba[2], min(rgba[3], 0.15))
                tim = ti
                if reg >= 0 and rgba[3] >= 0.95:         # --mat-box: this region's texture and colour
                    kb_ = mat_box[reg][1]
                    if kb_ not in mat_tex:
                        tw_, px_, mean_ = mat_texture(kb_)
                        textures.append((tw_, tw_, px_)); mat_tex[kb_] = (len(textures) - 1, mean_)
                    tim, m_ = mat_tex[kb_]
                    col_ = mat_box[reg][3]
                    rgba = (col_[0] / m_, col_[1] / m_, col_[2] / m_, rgba[3])
                elif mob is not None:                    # --mat-obj: opaque materials only (glass stays clear)
                    if rgba[3] < 0.95: tim = 0xFFFFFFFF
                    else:
                        col_ = mob[3]                    # a darker material takes the given colour (a black tub)
                        if col_ and lum(rgba) < lum(col_): rgba = (col_[0], col_[1], col_[2], rgba[3])
                        m_ = mat_tex[mob[1]][1]          # the texture's mean texel: the mean colour stays the material's
                        rgba = (rgba[0] / m_, rgba[1] / m_, rgba[2] / m_, rgba[3])
                tris = []
                for pts in polys:
                    for t0_, t1_, t2_ in poly_tris([p_[0] for p_ in pts]):   # fan (--ear-clip: concave ones clipped)
                        lim = subdiv_extra if only is not None and subdiv_extra is not None else subdiv
                        for rx_, val_ in subdiv_ac:
                            if rx_.search(acp.replace(os.sep, '/')): lim = val_
                        tris.extend(split_tri(pts[t0_], pts[t1_], pts[t2_], lim))
                if tris:
                    if GROUP_NAMES:      # a group's animations wrap its objects' own ones (SimGear), outermost first
                        lv = [(-1 if objs is None else min(names.index(x) for x in objs.intersection(names)), k)
                              for objs, k in aidx if objs is None or objs.intersection(names)]
                        chain = [k for _, k in sorted(lv, key=lambda e: e[0])][:4]
                    else:
                        # (--controls: a control's animation also moves the AC3D groups it names with what is in them,
                        # as SimGear does: the F-16's THROTTLE group holds its GRIP)
                        chain = [k for objs, k in aidx if objs is None or name in objs
                                 or (CONTROLS and anims[k][0] & ~0x800 in CONTROL_PROPS.values()
                                     and not anims[k][0] & 0x700 and objs.intersection(anc))][:4]
                    meshes.append((name[:31], tim, rgba, (1 if rgba[3] < 0.95 else 0) | (2 if two else 0) | scr << 8,
                                   chain, tris, spec, shi))
    if ANIM_SUBMODELS:                                   # only the animations some mesh uses (hidden parts' ones go)
        keep = sorted({k for m in meshes for k in m[4]})
        remap = {k: i for i, k in enumerate(keep)}
        anims = [anims[k] for k in keep]
        meshes = [m[:4] + ([remap[k] for k in m[4]],) + m[5:] for m in meshes]
    if thr_mil is not None:                              # --throttle-mil: lever 0..0.95..1 -> FlightGear 0..V..1
        for k_, (prop, c, a_, fac, off, table) in enumerate(anims):
            if prop & 0xFF != P_THR or prop & 0x700: continue
            anims[k_] = (prop, c, a_, 1.0, 0.0, thr_table(fac, off, table, thr_mil))
    for n_, q_ in add_glass:                             # --add-glass: a clear quad (eye frame), both triangles
        e_ = [to_eye_p(c_) for c_ in q_]
        u_ = [e_[1][j] - e_[0][j] for j in range(3)]; v_ = [e_[2][j] - e_[0][j] for j in range(3)]
        nn = [u_[1] * v_[2] - u_[2] * v_[1], u_[2] * v_[0] - u_[0] * v_[2], u_[0] * v_[1] - u_[1] * v_[0]]
        ln = math.sqrt(sum(c_ * c_ for c_ in nn)) or 1.0; nn = [c_ / ln for c_ in nn]
        if sum(nn[j] * e_[0][j] for j in range(3)) > 0: nn = [-c_ for c_ in nn]          # facing the eye
        uv_ = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
        tris_ = [[(e_[i_], uv_[i_], nn) for i_ in (0, 1, 2)], [(e_[i_], uv_[i_], nn) for i_ in (0, 2, 3)]]
        meshes.append((n_[:31], 0xFFFFFFFF, (1.0, 1.0, 1.0, 0.0), 1 | 2, [], tris_, 0.0, 1.0))
        print('--add-glass %s: corners (eye frame) %s' % (n_, '; '.join('%.3f %.3f %.3f' % tuple(c_) for c_ in e_)))
    if hands_ac: add_hands(hands_ac, hand_spec, textures, meshes, anims)
    if hands_hide:
        hh = [i for i, m in enumerate(meshes) if hands_hide.search(m[0]) and not m[3] & 16]
        meshes = [m[:3] + (m[3] | 32,) + m[4:] if i in hh else m for i, m in enumerate(meshes)]
        print('--hands-hide: %d meshes (%s)' % (len(hh), ', '.join(sorted({meshes[i][0] for i in hh}))[:120]))
    if EAR_CLIP:
        print('--ear-clip: %d concave polygons ear-clipped, %d kept as a fan (not clippable), %d convex, %d concave with '
              'a valid fan (kept)' % tuple(EAR_STATS))
    with open(outp, 'wb') as f:
        f.write(b'ACZCPIT3' + struct.pack('<III', len(textures), len(meshes), len(anims)))
        for w, h, px in textures: f.write(struct.pack('<II', w, h) + px)
        for prop, c, a, fac, off, table in anims:
            f.write(struct.pack('<I3f3fffI', prop, c[0], c[1], c[2], a[0], a[1], a[2], fac, off, len(table)))
            for ind, dep in table: f.write(struct.pack('<ff', ind, dep))
        for name, ti, rgba, flags, chain, tris, spec, shi in meshes:
            f.write(name.encode('latin-1', 'replace').ljust(32, b'\0'))
            ch = (chain + [0xFFFFFFFF] * 4)[:4]
            f.write(struct.pack('<I4fI4IffI', ti, rgba[0], rgba[1], rgba[2], rgba[3], flags, ch[0], ch[1], ch[2], ch[3],
                                spec, shi, len(tris)))
            buf = bytearray()
            for tri in tris:
                for p_, uv, n in tri:
                    buf += struct.pack('<9f', p_[0], p_[1], p_[2], n[0], n[1], n[2], uv[0], uv[1], 1.0)
            f.write(buf)
    for m in meshes:
        if m[3] >> 8:
            ps_ = [v[0] for t in m[5] for v in t]
            print('screen %d: %s, %d triangles, centre %.3f %.3f %.3f m' % (
                m[3] >> 8, m[0], len(m[5]), *[(min(p[j] for p in ps_) + max(p[j] for p in ps_)) / 2 for j in range(3)]))
    for rx_, i_, l_ in screens:
        if not any(m[3] >> 8 == i_ for m in meshes): print('--screen %s=%d: no object matched' % (rx_.pattern, i_))
    ntri = sum(len(m[5]) for m in meshes)
    allp = [v[0] for m in meshes for t in m[5] for v in t]
    lo = [min(p[j] for p in allp) for j in range(3)]; hi = [max(p[j] for p in allp) for j in range(3)]
    print('%s: %d parts, %d textures, %d meshes (%d animated, %d glass), %d animations, %d triangles; bounds '
          'X %.2f..%.2f Y %.2f..%.2f Z %.2f..%.2f m' % (outp, len(parts), len(textures), len(meshes),
                                                       sum(1 for m in meshes if m[4]), sum(1 for m in meshes if m[3] & 1),
                                                       len(anims), ntri, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))


if __name__ == '__main__':                 # (prep_gripen.sh imports its PNG reader)
    main()
