// Rainy Window by rocksdanister (CC BY-NC-SA 3.0), after "Heartfelt" by Martijn Steinrucken (BigWings).
// Modified for Screen+: a clearer glass (every drop is a clear lens through the fog), the background in 3D
// with the mouse (parallax-occlusion over a depth map, depth/<picture>.png), and weather inside the scene:
// rain or snow in depth layers that only show where the picture is farther away than the layer (rain falls in
// front of the mountains, not in front of the near ground), drifting veils of rain over the far distance, and
// frost on the glass for snow. Without a depth map the scene counts as far everywhere.
#ifdef GL_ES
precision highp float;
#endif

varying vec2 vUv;
uniform sampler2D u_tex0;
uniform sampler2D u_depth;
uniform vec2 u_tex0_resolution;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_speed;
uniform float u_intensity;
uniform float u_normal;
uniform float u_brightness;
uniform float u_blur_intensity;
uniform float u_zoom;
uniform int u_blur_iterations;
uniform bool u_panning;
uniform bool u_post_processing;
uniform bool u_lightning;
uniform bool u_texture_fill;
uniform bool u_has_depth;
uniform vec2 u_mouse;           // -1..1, smoothed
uniform float u_depth_amt;      // parallax shift at full mouse travel (UV units)
uniform int u_weather;          // 0 rain, 1 snow
uniform float u_scene;          // how much falls in the scene, 0..1
uniform float u_splash;         // how often drops landing on things glint, 0..1
uniform sampler2D u_land;       // the picture as a rough 3D surface: where a drop at each distance lands (script.js)

#define LAND_BINS 32.
vec2 g_fill;                    // screen -> picture: the cover scaling and the depth zoom (set in main)
float g_zoom;

#define S(a, b, t) smoothstep(a, b, t)
// Reads inside loops at mip 0: an ordinary read needs the pixel's neighbours (gradients), which makes Windows'
// shader compiler unroll the loops around it. WebGL2 has textureLod; WebGL1 keeps the plain read.
#if __VERSION__ >= 300
#define TEX0(s, u) textureLod(s, u, 0.)
#else
#define TEX0(s, u) texture2D(s, u)
#endif

vec3 N13(float p) {
    //  from DAVE HOSKINS
    vec3 p3 = fract(vec3(p) * vec3(.1031, .11369, .13787));
    p3 += dot(p3, p3.yzx + 19.19);
    return fract(vec3((p3.x + p3.y) * p3.z, (p3.x + p3.z) * p3.y, (p3.y + p3.z) * p3.x));
}

float N(float t) {
    return fract(sin(t * 12345.564) * 7658.76);
}

float Saw(float b, float t) {
    return S(0., b, t) * S(1., b, t);
}

// ---------------------------------------------------------------- drops on the glass (the original)

vec2 DropLayer2(vec2 uv, float t) {
    vec2 UV = uv;

    uv.y += t * 0.75;
    vec2 a = vec2(6., 1.);
    vec2 grid = a * 2.;
    vec2 id = floor(uv * grid);

    float colShift = N(id.x);
    uv.y += colShift;

    id = floor(uv * grid);
    vec3 n = N13(id.x * 35.2 + id.y * 2376.1);
    vec2 st = fract(uv * grid) - vec2(.5, 0);

    float x = n.x - .5;

    float y = UV.y * 20.;
    float wiggle = sin(y + sin(y));
    x += wiggle * (.5 - abs(x)) * (n.z - .5);
    x *= .7;
    float ti = fract(t + n.z);
    y = (Saw(.85, ti) - .5) * .9 + .5;
    vec2 p = vec2(x, y);

    float d = length((st - p) * a.yx);

    float mainDrop = S(.4, .0, d);

    float r = sqrt(S(1., y, st.y));
    float cd = abs(st.x - x);
    float trail = S(.23 * r, .15 * r * r, cd);
    float trailFront = S(-.02, .02, st.y - y);
    trail *= trailFront * r * r;

    y = UV.y;
    y = fract(y * 10.) + (st.y - .5);
    float dd = length(st - vec2(x, y));
    float droplets = S(.3, 0., dd);
    float m = mainDrop + droplets * r * trailFront;

    return vec2(m, trail);
}

float StaticDrops(vec2 uv, float t) {
    uv *= 40.;

    vec2 id = floor(uv);
    uv = fract(uv) - .5;
    vec3 n = N13(id.x * 107.45 + id.y * 3543.654);
    vec2 p = (n.xy - .5) * .7;
    float d = length(uv - p);

    float fade = Saw(.025, fract(t + n.z));
    float c = S(.3, 0., d) * fract(n.z * 10.) * fade;
    return c;
}

vec2 Drops(vec2 uv, float t, float l0, float l1, float l2) {
    float s = StaticDrops(uv, t) * l0;
    vec2 m1 = DropLayer2(uv, t) * l1;
    vec2 m2 = DropLayer2(uv * 1.85, t) * l2;

    float c = s + m1.x + m2.x;
    c = S(.3, 1., c);

    return vec2(c, max(m1.y * l0, m2.y * l1));
}

float N21(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

// ---------------------------------------------------------------- the picture in depth

float Depth(vec2 u) {           // 1 near, 0 far
    return TEX0(u_depth, clamp(u, .001, .999)).r;
}

// Parallax occlusion: near things slide over far ones as the mouse moves (as Screen+'s depth wallpapers do).
vec2 Parallax(vec2 st) {
    vec2 off = -u_mouse * u_depth_amt, d = off / 16., u = st - off * .5;
    float ls = 1. / 16., l = 1., m = Depth(u);
    for(int i = 0; i < 16; i++) {
        if(l <= m) break;
        u += d;
        m = Depth(u);
        l -= ls;
    }
    float a = m - l, b = Depth(u - d) - (l + ls);
    return mix(u, u - d, clamp(a / (a - b + 1e-5), 0., 1.));
}

// ---------------------------------------------------------------- weather in the scene

float VNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3. - 2. * f);
    float a = N21(i), b = N21(i + vec2(1, 0)), c = N21(i + vec2(0, 1)), d = N21(i + vec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float Fbm(vec2 p) {
    float v = 0., a = .5;
    for(int i = 0; i < 4; i++) {
        v += a * VNoise(p);
        p = p * 2.03 + 17.1;
        a *= .5;
    }
    return v;
}

// Where a drop at distance z falling down the picture's column tx first meets the surface, in screen units
// (s.y); far below the screen when nothing in that column is that near.
float LandY(float tx, float z) {
    vec4 v = TEX0(u_land, vec2(tx, (floor(clamp(z, 0., .999) * LAND_BINS) + .5) / LAND_BINS));
    if(v.a < .5) return -10.;
    float ty = (v.r * 65280. + v.g * 255.) / 65535.;
    return (ty - .5) / g_zoom / g_fill.y;
}

// The anime glint of a drop landing: a white dot with a short four-point twinkle that pops and fades.
float Glint(vec2 d, float age, float sizePx) {
    float a = exp(-age * 12.) * S(0., .015, age);
    float r = 1.2 + sizePx * .7 + age * 20.;
    float arm = 3. + sizePx * 1.5 + age * 45.;
    float dot_ = exp(-dot(d, d) / (r * r));
    float star = exp(-abs(d.y) * 1.3) * exp(-abs(d.x) / arm) + exp(-abs(d.x) * 1.3) * exp(-abs(d.y) / arm);
    return (dot_ + star * .7) * a;
}

// One layer of falling rain around distance z. s: screen units (height = 1). Drops are short, tapered streaks
// as long as the drop travels during one exposure (motion blur, not drawn lines), a random few per column, each
// at its own distance near z. With a depth map a drop falls until it meets the picture's surface at its distance
// (the top of a hood, a shoulder, the ground) and is gone below it; a few of those landings glint (glint, added).
// The layer moves with the picture's 3D parallax at its distance, so drops and landings stay on the surfaces.
// sizePx is a drop's width on screen; speed in screen heights per second; wind the slant (sideways per fall).
float RainLayer(vec2 s, float t, float scale, float speed, float sizePx, float density, float seed, float z, float wind,
                inout float glint) {
    const float CH = 4.;                                 // cell height, in column widths
    if(u_has_depth) {                                    // where scenery at distance z shows with the mouse where it is
        vec2 shift = u_mouse * u_depth_amt * (z - .5) / g_zoom / g_fill;
        s -= vec2(shift.x * u_resolution.x / u_resolution.y, shift.y);
    }
    vec2 q = s * scale;
    q.x += q.y * wind;
    float column = floor(q.x);
    float v = speed * scale;                             // in column widths per second
    float Y = q.y + t * v + N(column + seed) * 31.;      // this pixel in the column's falling frame
    float len = clamp(v / 55., .4, CH - .6);             // streak = distance travelled in ~1/55 s
    float w = max(sizePx * scale / u_resolution.y, .02);
    float tx = g_fill.x * s.x * u_resolution.y / u_resolution.x * g_zoom + .5;   // this column in the picture
    float streak = 0.;

    vec2 id = vec2(column, floor(Y / CH));
    vec3 r = N13(id.x * 71.37 + id.y * 113.71 + seed * 17.3);
    if(r.z < density) {
        float zd = z + (N(r.x * 53.1 + seed) - .5) * .12;
        float x = .2 + r.x * .6;
        float y0 = r.y * (CH - len);
        float along = (mod(Y, CH) - y0) / len;
        if(along >= 0. && along <= 1.) {
            float dx = (fract(q.x) - x) / w;
            float taper = S(0., .35, along) * S(1., .55, along);
            streak = exp(-dx * dx * 1.6) * taper * (.55 + .45 * N(r.x * 91.7));
            if(u_has_depth) streak *= S(-.5, .5, q.y - LandY(tx, zd) * scale);   // landed: nothing below
        }
    }

    // drops that landed near here in the last moments (a glint lives ~a quarter second); narrow columns also
    // look at their neighbours, so a glint is never cut off at a column's edge
    if(u_has_depth && u_splash > .001) {
        const float LIFE = .25;
        float share = u_splash * .02;                    // at the default, about 1 landing in 170 glints
        float side = u_resolution.y / scale < 40. ? 1. : 0.;
        for(int n = -1; n <= 1; n++) {
            float dc = float(n);
            if(abs(dc) > side) continue;
            float cn = column + dc;
            float phase = N(cn + seed) * 31.;
            float Yn = q.y + t * v + phase;
            for(int k = 0; k < 14; k++) {
                if(float(k) * CH > LIFE * v + CH) break;
                vec2 cid = vec2(cn, floor(Yn / CH) - float(k));
                vec3 c = N13(cid.x * 71.37 + cid.y * 113.71 + seed * 17.3);
                if(c.z >= density || N(c.y * 77.7 + seed * 3.1) >= share) continue;
                float zd = z + (N(c.x * 53.1 + seed) - .5) * .12;
                float ly = LandY(tx, zd) * scale;        // its landing height
                float head = cid.y * CH + c.y * (CH - len);   // its leading end, in the falling frame
                float age = (ly + t * v + phase - head) / v;
                if(age < 0. || age > LIFE) continue;
                vec2 d = vec2(q.x - (cn + .2 + c.x * .6), q.y - ly) * u_resolution.y / scale;
                glint += Glint(d, age, sizePx);
            }
        }
    }
    return streak;
}

// A flake's place in its cell of the falling frame at time t: its spot, swaying side to side.
vec2 FlakeAt(vec3 r, float t) {
    return vec2(.25 + r.x * .5 + sin(t * (.7 + r.y) + r.x * 6.283) * .22, .25 + r.y * .5);
}

// A snow crystal, six-fold and mirrored, so only half of one arm is drawn: the angle (plus the flake's spin, so
// no rotation matrix) is folded into the 30° beside an arm. Its parts, as the field guides show them: a hexagonal
// core (mostly small, now and then a broad plate), a tapering main arm and up to three pairs of side branches at
// 60°, set further out than in as dendrites grow and short enough to stay in their sector. h: the flake's random
// numbers. p in flake radii; returns the distance to the crystal in flake radii.
// Side branch i of a crystal's half arm (f: x along the arm, y beside it): out at 60°, short enough to stay in
// its sector, tapering.
float Branch(vec2 f, float th, vec4 h, float i) {
    float x0 = mix(.3, .85, fract(h.z * (3.7 + i * 2.3) + i * .37));
    float len = min((1. - x0) * mix(.35, .95, fract(h.w * (5.1 + i * 1.7))), x0 * .95);
    vec2 v = f - vec2(x0, 0.);
    float t = clamp(dot(v, vec2(.5, .8660254)), 0., len);
    return length(v - vec2(.5, .8660254) * t) - th * .75 * (1. - .5 * t / len);
}

float Crystal(vec2 p, float spin, vec4 h) {
    float a = abs(mod(atan(p.y, p.x) + spin, 1.0471976) - .5235988), l = length(p);
    vec2 f = l * vec2(cos(a), sin(a));                   // x along the arm, y beside it
    float th = .03 + .05 * h.x;                          // the arm's half width
    float d = (f.x < 1. ? f.y : length(f - vec2(1., 0.))) - th * (1. - .45 * f.x);
    float core = .1 + .5 * h.y * h.y * h.y;
    float hex = l * cos(.5235988 - a);                   // a hexagon's distance, its corners on the arms
    if(core > .25) d = min(min(d, abs(hex - core * .8660254) - th * .8), hex - core * .35);   // a plate: rim and hub
    else d = min(d, hex - core * .8660254);
    float pairs = 1. + floor(h.w * 3.);
    d = min(d, Branch(f, th, h, 0.));
    if(pairs > 1.) d = min(d, Branch(f, th, h, 1.));
    if(pairs > 2.) d = min(d, Branch(f, th, h, 2.));
    return d;
}

// Where a flake at distance z over screen x (in cells of this scale) meets the surface, in cells.
float SnowLand(float x, float scale, float z) {
    float tx = g_fill.x * x / scale * u_resolution.y / u_resolution.x * g_zoom + .5;
    return LandY(tx, z) * scale;
}

// One layer of snow around distance z: soft round flakes drifting and swaying, sharp when small (far), soft when
// big (near). With a depth map a flake falls until it meets the picture's surface at its distance, as rain does,
// and is gone there; some of those flakes settle where they landed as a soft dot that lingers and melts (settled,
// added; life in this clock's units). The layer moves with the picture's parallax at its distance, like rain.
// The crystal to draw on this pixel, the nearest layer's (main() draws it once: inside each of the four layers
// Windows' shader compiler took ~15 s over it, and the wallpaper stayed black that long the first time):
// xy its point in flake radii, z its spin, w the edge blur; its numbers; its strength and size in pixels.
vec4 g_cry, g_cryH;
float g_cryW, g_cryPx;

float SnowLayer(vec2 s, float t, float scale, float speed, float sizePx, float density, float seed, float z,
                float w, float life, inout float settled) {
    if(u_has_depth) {
        vec2 shift = u_mouse * u_depth_amt * (z - .5) / g_zoom / g_fill;
        s -= vec2(shift.x * u_resolution.x / u_resolution.y, shift.y);
    }
    float v = speed * scale, b = .015 * scale;           // fall and breeze, in cells per unit of t
    vec2 drift = vec2(t * b, t * v);
    vec2 q = s * scale + drift;                          // this pixel in the falling, drifting frame
    vec2 id = floor(q);
    float flake = 0.;
    vec3 r = N13(id.x * 53.91 + id.y * 217.3 + seed * 11.7);
    if(r.z <= density) {
        vec2 c = id + FlakeAt(r, t);
        vec3 g = N13(r.x * 91.3 + r.y * 47.1 + seed * 5.9);   // its shape
        bool clump = g.x < .4;                           // most falling snow is clumps of crystals: round
        float px = sizePx * (.75 + .5 * g.y) * (clump ? 1. : 1.6);   // radius in pixels; crystals are lacy, so wider
        float rad = max(px * scale / u_resolution.y, .015);
        float soft = mix(.35, .9, S(2., 9., sizePx));    // big flakes are out of focus
        vec2 p = (q - c) / rad;                          // in flake radii
        float shade = (.6 + .4 * N(r.y * 47.3)) * w;
        if(u_has_depth) {                                // landed: gone
            vec2 cs = c - drift;
            shade *= S(-.3, .3, cs.y - SnowLand(cs.x, scale, z + (N(r.x * 53.1 + seed) - .5) * .12));
        }
        if(dot(p, p) < 1.2) {                            // only pixels on the flake draw it
            if(clump || px < 3.) {                       // a clump, or a crystal too small to show its shape
                flake = S(1., 1. - soft, length(p)) * shade * (clump ? 1. : .7);
            } else if(px > g_cryPx) {                    // a crystal: spinning, tumbling (seen edge-on, it narrows)
                p.y /= .4 + .6 * abs(sin(t * (.3 + g.z * .5) + g.y * 6.283));
                g_cry = vec4(p, g.z * 6.283 + t * (g.y - .5) * 1.2, 1. / px + soft * .25);   // blur: a pixel, plus the defocus of near flakes
                g_cryH = vec4(g.yz, N(g.x * 31.7 + seed), N(g.z * 17.3 + seed));
                g_cryW = shade * 1.15;
                g_cryPx = px;
            }
        }
    }

    // flakes that landed near here a moment ago; the breeze and the sway carry a flake up to two cells sideways
    if(u_has_depth && u_splash > .001) {
        float share = min(u_splash * .5, 1.);           // at the default, about 1 flake in 7 stays a moment
        vec2 ps = s * scale;                             // this pixel on screen, in cells
        for(int n = -2; n <= 1; n++) {
            for(int k = 0; k < 12; k++) {
                if(float(k) > life * v + 1.) break;
                vec2 cid = vec2(id.x + float(n), id.y - float(k));
                vec3 c = N13(cid.x * 53.91 + cid.y * 217.3 + seed * 11.7);
                if(c.z > density || N(c.y * 77.7 + seed * 3.1) >= share) continue;
                float zd = z + (N(c.x * 53.1 + seed) - .5) * .12;
                float fy = cid.y + .25 + c.y * .5;       // its height in the falling frame
                // when it landed: from where it is now, then again from where it was then
                float ly = SnowLand(cid.x + FlakeAt(c, t).x - t * b, scale, zd);
                float tl = (fy - ly) / v;
                float lx = cid.x + FlakeAt(c, tl).x - tl * b;
                ly = SnowLand(lx, scale, zd);
                tl = (fy - ly) / v;
                float ph = (t - tl) / life;
                if(ph < 0. || ph > 1.) continue;
                vec2 d = (ps - vec2(lx, ly)) * u_resolution.y / scale;   // pixels from where it lies
                float rp = 1.5 + sizePx * .6;
                settled += exp(-dot(d, d) / (rp * rp)) * S(0., .15, ph) * S(1., .4, ph) * .8;   // settles, then melts
            }
        }
    }
    return flake;
}

// How much of a layer at distance z (0 near .. 1 far) shows: only where the picture is farther than it.
float Behind(float z, float sceneFar) {
    return S(z - .1, z + .02, sceneFar);
}

// ---------------------------------------------------------------- main

void main() {
    vec2 uv = (gl_FragCoord.xy - .5 * u_resolution.xy) / u_resolution.y;
    vec2 s = uv;                                         // screen units for the weather (not zoomed)
    vec2 UV = gl_FragCoord.xy / u_resolution.xy;
    vec2 screenUV = UV;
    float T = u_time;
    bool snow = u_weather == 1;

    vec2 fillScale = vec2(1.);
    if(u_texture_fill) {
        float screenAspect = u_resolution.x / u_resolution.y;
        float textureAspect = u_tex0_resolution.x / u_tex0_resolution.y;
        if(textureAspect > screenAspect)
            fillScale.x = screenAspect / textureAspect;
        else
            fillScale.y = textureAspect / screenAspect;
        UV = fillScale * (UV - 0.5) + 0.5;
    }
    float zoomIn = u_has_depth ? 1. - u_depth_amt * 1.2 : 1.;
    g_fill = fillScale;
    g_zoom = zoomIn;
    if(u_has_depth) {                                    // a little closer, so shifted pixels stay inside the picture
        UV = (UV - .5) * zoomIn + .5;
        UV = Parallax(UV);
    }
    float sceneFar = u_has_depth ? 1. - Depth(UV) : 1.;

    float t = T * .2 * u_speed;
    float rainAmount = u_intensity;

    float zoom = u_panning ? -cos(T * .2) : 0.;
    uv *= (.7 + zoom * .3) * u_zoom;

    // the glass: snow leaves a few melted drops, no running ones
    float staticDrops = S(-.5, 1., rainAmount) * (snow ? .7 : 2.);
    float layer1 = snow ? 0. : S(.25, .75, rainAmount);
    float layer2 = snow ? 0. : S(.0, .5, rainAmount);

    vec2 c = Drops(uv, t, staticDrops, layer1, layer2);
    vec2 e = vec2(.001, 0.) * u_normal;
    float cx = Drops(uv + e, t, staticDrops, layer1, layer2).x;
    float cy = Drops(uv + e.yx, t, staticDrops, layer1, layer2).x;
    vec2 n = vec2(cx - c.x, cy - c.x);

    // fogged glass, but clear through every drop and its trail (they wipe the fog away)
    float clear = max(S(.1, .25, c.x), c.y * .8);
    float blur = u_blur_intensity * .01 * (1. - clear * .9);

    vec3 col = texture2D(u_tex0, UV + n).rgb;
    if(u_blur_iterations != 1) {
        float a = N21(gl_FragCoord.xy) * 6.2831;
        for(int m = 0; m < 64; m++) {
            if(m > u_blur_iterations)
                break;
            vec2 offs = vec2(sin(a), cos(a)) * blur;
            float d = fract(sin((float(m) + 1.) * 546.) * 5424.);
            offs *= sqrt(d);
            col += texture2D(u_tex0, UV + n + offs).xyz;
            a++;
        }
        col /= float(u_blur_iterations);
    }

    // weather in the scene, seen through the glass: refracted by the drops, softened by the fog
    float amount = u_scene * (.35 + .65 * S(0., .6, rainAmount));
    vec2 ws = s + n * 1.5;
    float fog = u_blur_intensity * .01 * (1. - clear);   // 0 clear .. 1 fogged
    float lum = dot(col, vec3(.299, .587, .114));
    float wt = T * (.6 + u_speed * 1.6);                  // the weather's own clock, from the speed setting
    float glints = 0.;
    if(amount > .001) {
        if(!snow) {
            float soft = 1. + fog * 3.;
            // far to near: smaller and denser far away, bigger, sparser and softer up close
            // rain is never even: heavier and lighter spells (about a minute), gusts sweeping across the scene,
            // and wind that slowly sways the slant
            float spell = .5 + .5 * sin(wt * .09) * sin(wt * .037 + 1.7);
            float sweep = Fbm(vec2(ws.x * .9 - wt * .09, wt * .021));
            float gust = clamp(.3 + .55 * spell + (sweep - .5) * 1.1, .12, 1.15);
            float wind = .05 + .045 * sin(wt * .043) + .02 * sin(wt * .17 + 2.);
            float a = amount * (.55 + .6 * gust), bright = .75 + .35 * gust;
            float r = 0., gl = 0.;
            r += RainLayer(ws, wt, 160., 1.2, .7 * soft, a * .5, 5., .64, wind, gl) * .6 * Behind(.64, sceneFar);
            r += RainLayer(ws, wt, 120., 1.25, .8 * soft, a * .6, 1., .5, wind, gl) * .7 * Behind(.5, sceneFar);
            r += RainLayer(ws, wt, 70., 1.35, 1.1 * soft, a * .55, 2., .38, wind, gl) * .75 * Behind(.38, sceneFar);
            r += RainLayer(ws, wt, 40., 1.5, 1.6 * soft, a * .45, 3., .27, wind, gl) * .75 * Behind(.27, sceneFar);
            r += RainLayer(ws, wt, 22., 1.7, 2.6 * soft, a * .3, 4., .17, wind, gl) * .6 * Behind(.17, sceneFar);
            r *= bright;
            glints = gl;
            // rain is the sky's light caught in the drops: bright on dark rock, lost against a bright sky
            vec3 lit = mix(vec3(.82, .86, .92), vec3(lum) * 1.1 + .16, .35);
            col = mix(col, lit, clamp(r, 0., 1.) * (1. - fog * .4));

            // heavy rain far away shows as veils drifting down over the distance
            vec2 vp = vec2(ws.x * 1.4 + wt * .06, ws.y * .55 + wt * .55);
            float veil = S(.42, .85, Fbm(vp * vec2(1., .3))) * S(.2, .6, sceneFar);
            col = mix(col, mix(col, vec3(lum) * .95 + .12, .7), veil * a * .5 * (.6 + .6 * gust));
        } else {
            float soft = 1. + fog * 1.5;
            float f = 0.;
            float st = wt * .35;
            float life = 1.4 * (.6 + u_speed * 1.6) * .35;  // a settled flake lasts 1.4 s, on the snow's clock
            float gl = 0.;
            g_cryW = g_cryPx = 0.;
            f += SnowLayer(ws, st, 90., .07, 1.1 * soft, amount * .35, 5., .5, .7 * Behind(.5, sceneFar), life, gl);
            f += SnowLayer(ws, st, 50., .1, 1.8 * soft, amount * .3, 6., .38, .8 * Behind(.38, sceneFar), life, gl);
            f += SnowLayer(ws, st, 26., .14, 3.2 * soft, amount * .25, 7., .26, .85 * Behind(.26, sceneFar), life, gl);
            f += SnowLayer(ws, st, 12., .2, 7. * soft, amount * .18, 8., .14, .6 * Behind(.14, sceneFar), life, gl);
            if(g_cryW > 0.) f += (1. - S(-g_cry.w, g_cry.w, Crystal(g_cry.xy, g_cry.z, g_cryH))) * g_cryW;
            glints = gl;
            col = mix(col, vec3(.95, .97, 1.), clamp(f, 0., 1.));
            // falling snow far away whitens the distance
            vec2 vp = vec2(ws.x * 1.2 + st * .05, ws.y * .9 + st * .12);
            float haze = S(.3, .9, Fbm(vp)) * S(.2, .6, sceneFar);
            col = mix(col, vec3(.9, .93, .97), haze * amount * .35);
        }
    }

    // drops and flakes landing on things, seen through the glass
    if(u_has_depth && u_splash > .001) {
        col = mix(col, vec3(1.), clamp(glints, 0., 1.) * (1. - fog * .5));
    }

    // frost creeping in from the corners of the glass when it snows
    if(snow) {
        vec2 cc = screenUV - .5;
        float frost = S(.42, .75, length(cc * vec2(1.1, 1.3)) + (Fbm(screenUV * 9.) - .5) * .35);
        col = mix(col, col * .6 + vec3(.38, .41, .45), frost * .55 * (1. - clear));
    }

    t = (T + 3.) * .5;			// make time sync with first lightning
    if(u_post_processing) {
        col *= snow ? vec3(.92, .97, 1.12) : vec3(.8, .9, 1.3);	// cool colour shift
    }
    float fade = S(0., 10., T);							// fade in at the start

    if(u_lightning) {
        float lightning = sin(t * sin(t * 10.));				// lighting flicker
        lightning *= pow(max(0., sin(t + sin(t))), 10.);		// lightning flash
        col *= 1. + lightning * fade;
    }
    vec2 v = screenUV - .5;
    col *= 1. - dot(v, v); // vignette

    gl_FragColor = vec4(col * u_brightness, 1);
}
