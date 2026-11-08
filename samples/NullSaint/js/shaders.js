'use strict';
/* NULL//SAINT - GLSL: feedback/datamosh, bloom and the big signal-damage composite */
(function (NS) {
  const SH = NS.SH = {};

  SH.vert = `#version 300 es
in vec2 aPos; out vec2 vUv;
void main(){ vUv = aPos*0.5+0.5; gl_Position = vec4(aPos,0.0,1.0); }`;

  const common = `#version 300 es
precision highp float;
in vec2 vUv; out vec4 o;
#define PI 3.14159265
#define TAU 6.2831853
float h1(float n){ return fract(sin(n)*43758.5453123); }
float h2(vec2 p){ return fract(sin(dot(p, vec2(127.1,311.7)))*43758.5453123); }
float vn(vec2 p){ vec2 i=floor(p), f=fract(p); f=f*f*(3.0-2.0*f);
  return mix(mix(h2(i),h2(i+vec2(1,0)),f.x), mix(h2(i+vec2(0,1)),h2(i+vec2(1,1)),f.x), f.y); }
float luma(vec3 c){ return dot(c, vec3(0.2126,0.7152,0.0722)); }
vec3 hueRot(vec3 c, float a){ const vec3 k = vec3(0.57735); float ca=cos(a);
  return c*ca + cross(k,c)*sin(a) + k*dot(k,c)*(1.0-ca); }
`;

  // ---------- feedback: datamosh, melt, cursor smear, time freeze ----------
  SH.mosh = common + `
uniform sampler2D uScene, uPrev;
uniform vec2 uRes, uMouse, uMouseVel;
uniform float uTime, uMosh, uMelt, uTrail, uSeed, uFreeze, uSwirl;
void main(){
  vec2 uv = vUv;
  vec4 sc = texture(uScene, uv);
  vec2 g = vec2(32.0, 18.0);
  vec2 b = floor(uv*g);
  float tq = floor(uTime*3.0 + uSeed);
  float r = h2(b + tq);
  vec2 mv = (vec2(h2(b+tq+3.1), h2(b+tq+7.7)) - 0.5) * 0.012;
  float moshMask = step(1.0 - uMosh*0.55, r);
  vec2 flow = mv * moshMask;
  // melt: every 3px column drips at its own speed
  float col = h1(floor(uv.x*uRes.x/3.0) + uSeed);
  flow.y += uMelt * (0.0008 + 0.009*col*col);
  // swirl around the centre (used by the takeover)
  vec2 c = uv - 0.5; float ang = uSwirl * 0.03 * exp(-dot(c,c)*6.0);
  flow += vec2(-c.y, c.x) * ang;
  // cursor smear: drag the previous frame along the cursor's motion
  float asp = uRes.x/uRes.y;
  vec2 dm = (uv - uMouse) * vec2(asp, 1.0);
  float near = exp(-dot(dm,dm)*55.0);
  flow -= uMouseVel * near * 0.9;
  vec4 pv = texture(uPrev, uv + flow);
  float sp = clamp(length(uMouseVel)*60.0, 0.0, 1.0);
  float m = moshMask*uMosh*0.92 + near*uTrail*sp*0.93 + uMelt*(0.35+0.62*col) + abs(uSwirl)*0.6;
  m = clamp(max(m, uFreeze), 0.0, 0.985);
  // moshed blocks keep only bright motion, like a dropped I-frame
  vec3 res = mix(sc.rgb, pv.rgb, m);
  o = vec4(res, 1.0);
}`;

  SH.bright = common + `
uniform sampler2D uSrc; uniform float uThr;
void main(){
  vec3 c = texture(uSrc, vUv).rgb;
  float l = max(c.r, max(c.g, c.b));
  o = vec4(c * smoothstep(uThr, 1.0, l) * 1.4, 1.0);
}`;

  SH.blur = common + `
uniform sampler2D uSrc; uniform vec2 uDir;
void main(){
  vec3 s = texture(uSrc, vUv).rgb * 0.227027;
  s += texture(uSrc, vUv + uDir*1.3846).rgb * 0.3162162;
  s += texture(uSrc, vUv - uDir*1.3846).rgb * 0.3162162;
  s += texture(uSrc, vUv + uDir*3.2307).rgb * 0.0702703;
  s += texture(uSrc, vUv - uDir*3.2307).rgb * 0.0702703;
  o = vec4(s, 1.0);
}`;

  SH.copy = common + `
uniform sampler2D uSrc;
void main(){ o = vec4(texture(uSrc, vUv).rgb, 1.0); }`;

  // ---------- final composite ----------
  SH.final = common + `
uniform sampler2D uSrc, uBloom, uAtlas;
uniform vec2 uRes, uMouse, uMouseVel;
uniform float uTime, uSeed;
uniform float uRGB, uScan, uCurve, uGrain, uBlock, uTear, uVHS, uSort, uBloomK, uVig, uWave, uPix, uPost;
uniform float uDither, uMask, uAscii, uAsciiCell, uLens, uInvert, uShuffle, uHue, uStatic, uBars, uZoom;
uniform int uColorMode, uMirror;
uniform vec4 uShock[6];
uniform vec4 uFlash;
uniform vec3 uTint;

float bayer4(vec2 p){
  ivec2 q = ivec2(mod(p, 4.0));
  int i = q.x + q.y*4;
  int m[16] = int[16](0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5);
  return (float(m[i]) + 0.5) / 16.0;
}
vec3 thermal(float l){
  vec3 c0=vec3(0.0,0.0,0.05), c1=vec3(0.1,0.0,0.6), c2=vec3(0.8,0.0,0.6), c3=vec3(1.0,0.2,0.0), c4=vec3(1.0,0.9,0.1), c5=vec3(1.0);
  if(l<0.2) return mix(c0,c1,l/0.2);
  if(l<0.4) return mix(c1,c2,(l-0.2)/0.2);
  if(l<0.6) return mix(c2,c3,(l-0.4)/0.2);
  if(l<0.8) return mix(c3,c4,(l-0.6)/0.2);
  return mix(c4,c5,(l-0.8)/0.2);
}

void main(){
  vec2 uv = vUv;
  float asp = uRes.x/uRes.y;

  // zoom (takeover punch-in)
  uv = (uv - 0.5) / (1.0 + uZoom) + 0.5;

  // CRT barrel
  vec2 cc = uv*2.0-1.0;
  cc *= 1.0 + uCurve*0.16*dot(cc,cc);
  cc /= 1.0 + uCurve*0.08;
  uv = cc*0.5+0.5;
  float outside = step(0.0,uv.x)*step(uv.x,1.0)*step(0.0,uv.y)*step(uv.y,1.0);

  // mirror modes
  if(uMirror==1){ uv.x = uv.x>0.5 ? 1.0-uv.x : uv.x; }
  else if(uMirror==2){ uv = 0.5 - abs(uv-0.5); }
  else if(uMirror==3){
    vec2 p = (uv-0.5)*vec2(asp,1.0);
    float a = atan(p.y,p.x) + uTime*0.1, r = length(p), seg = TAU/6.0;
    a = mod(a, seg); a = abs(a - seg*0.5);
    p = r*vec2(cos(a), sin(a));
    uv = p/vec2(asp,1.0) + 0.5;
  }

  // tile shuffle
  if(uShuffle>0.0){
    vec2 g = vec2(6.0,4.0);
    vec2 t = floor(uv*g);
    float r = h2(t + uSeed*13.0);
    if(r < uShuffle){
      vec2 nt = floor(vec2(h2(t+uSeed+1.7), h2(t+uSeed+9.1))*g);
      uv = (nt + fract(uv*g))/g;
    }
  }

  // signal wave
  uv.x += sin(uv.y*24.0 + uTime*4.0) * uWave * 0.006 + sin(uv.y*140.0 - uTime*11.0)*uWave*0.0015;

  // cursor lens
  vec2 dm = (uv - uMouse)*vec2(asp,1.0);
  float lf = exp(-dot(dm,dm)*30.0) * uLens;
  uv -= (dm/vec2(asp,1.0)) * lf * 0.45;

  // shockwaves
  for(int i=0;i<6;i++){
    vec4 s = uShock[i];
    if(s.w <= 0.0) continue;
    vec2 d = (uv - s.xy)*vec2(asp,1.0);
    float len = length(d) + 1e-5;
    float rad = s.z*0.85;
    float ring = exp(-pow((len-rad)*18.0, 2.0)) * s.w * max(0.0, 1.0 - s.z/1.6);
    uv -= (d/len)/vec2(asp,1.0) * ring * 0.035;
  }

  // line tearing
  float rowH = 3.0 + 26.0*h1(floor(uTime*7.0)+uSeed);
  float row = floor(uv.y*uRes.y/rowH);
  float tr = h2(vec2(row, floor(uTime*12.0)+uSeed));
  if(tr < uTear*0.22) uv.x += (h2(vec2(row,3.3)) - 0.5) * 0.16 * uTear;

  // block corruption
  float blockHit = 0.0;
  {
    float sz = 1.0 + floor(h1(floor(uTime*4.0)+uSeed)*4.0);
    vec2 g = vec2(10.0,6.0)*sz;
    vec2 b = floor(uv*g);
    float r = h2(b + floor(uTime*10.0) + uSeed);
    if(r < uBlock*0.13){
      uv += (vec2(h2(b+1.0), h2(b+2.0)) - 0.5) * 0.12 * uBlock;
      blockHit = 1.0;
    }
  }

  // VHS tracking
  uv.x += (vn(vec2(uv.y*220.0, uTime*25.0)) - 0.5) * 0.004 * uVHS;
  float tb = fract(uTime*0.071);
  float band = smoothstep(0.06, 0.0, abs(uv.y - tb)) * uVHS;
  uv.x += band * (h2(vec2(floor(uv.y*uRes.y*0.5), floor(uTime*30.0))) - 0.5) * 0.06;

  // pixelate
  if(uPix > 0.001){
    vec2 px = vec2(mix(1.0, 26.0, uPix)) / uRes;
    uv = (floor(uv/px) + 0.5)*px;
  }

  // chromatic split
  vec2 off = (uv-0.5)*uRGB*0.014 + uMouseVel*uRGB*0.6 + vec2(blockHit*0.012*(h1(uSeed)-0.3),0.0);
  vec3 col;
  col.r = texture(uSrc, uv + off).r;
  col.g = texture(uSrc, uv).g;
  col.b = texture(uSrc, uv - off).b;
  if(blockHit>0.5 && h1(uSeed+floor(uTime*10.0))>0.5) col = col.brg;

  // pixel sort streaks (vertical, in random columns)
  if(uSort > 0.001){
    float colId = floor(uv.x*48.0);
    float pick = h2(vec2(colId, floor(uTime*2.0)+uSeed));
    if(pick < uSort*0.6){
      vec3 best = col; float bl = luma(col);
      float thr = 0.35 + 0.3*h1(colId);
      for(int k=1;k<18;k++){
        vec3 s = texture(uSrc, uv + vec2(0.0, float(k)*3.0/uRes.y)).rgb;
        float l = luma(s);
        if(l > thr && l > bl){ best = s; bl = l; }
      }
      col = mix(col, best, 0.85);
    }
  }

  // bloom
  col += texture(uBloom, uv).rgb * uBloomK;

  // ASCII vision
  if(uAscii > 0.001){
    vec2 cs = vec2(uAsciiCell, uAsciiCell*1.6);
    vec2 cell = floor(gl_FragCoord.xy / cs);
    vec2 cuv = (cell + 0.5)*cs/uRes;
    vec3 cc2 = texture(uSrc, cuv).rgb + texture(uBloom, cuv).rgb*0.5;
    float l = clamp(luma(cc2)*1.25, 0.0, 0.999);
    float idx = floor(l*16.0);
    vec2 lc = fract(gl_FragCoord.xy / cs);
    float g = texture(uAtlas, vec2((idx + lc.x)/16.0, lc.y)).r;
    vec3 ac = normalize(cc2 + 0.001)*1.3*g*(0.4+l);
    col = mix(col, ac, uAscii);
  }

  // colour modes
  float L = luma(col);
  if(uColorMode==1) col = thermal(clamp(L*1.2,0.0,1.0));
  else if(uColorMode==2) col = vec3(0.1,1.0,0.25)*pow(L*1.6,0.8) + (h2(gl_FragCoord.xy+uTime)-0.5)*0.08;
  else if(uColorMode==3) col = 1.0 - col;
  else if(uColorMode==4) col = vec3(step(bayer4(gl_FragCoord.xy), L*1.3)) * uTint;
  else if(uColorMode==5) col = vec3(L*1.1, L*0.95, L*0.6) * vec3(1.0,0.95,0.7) + vec3(0.0,0.03,0.0);
  else if(uColorMode==6) col = hueRot(col*1.2, L*TAU*2.0 + uTime*1.5);

  // hue drift
  if(abs(uHue) > 0.0001) col = hueRot(col, uHue);

  // posterize & dither
  float d = uDither > 0.5 ? bayer4(gl_FragCoord.xy) - 0.5 : 0.0;
  if(uPost > 0.001){
    float lv = mix(24.0, 2.0, uPost);
    col = floor(col*lv + d + 0.5)/lv;
  } else if(uDither > 0.5){
    col = floor(col*8.0 + d + 0.5)/8.0;
  }

  // static / colour bars (signal lost)
  if(uBars > 0.001){
    float bi = floor(vUv.x*7.0);
    vec3 bars = vec3(step(0.5, h1(bi*1.3)), step(0.5,h1(bi*2.7+1.0)), step(0.5,h1(bi*4.1+2.0)))*0.85;
    if(vUv.y < 0.25) bars = vec3(vUv.x);
    col = mix(col, bars, uBars);
  }
  if(uStatic > 0.001){
    float n = h2(floor(gl_FragCoord.xy*0.5) + fract(uTime*7.3)*311.0);
    col = mix(col, vec3(n), uStatic);
  }

  // VHS band noise
  col += band * (h2(gl_FragCoord.xy + uTime) - 0.5) * 0.5;

  // scanlines + rolling bar
  col *= 1.0 - uScan*0.38*(0.5+0.5*sin(gl_FragCoord.y*PI*0.5));
  col *= 1.0 - uScan*0.06*(0.5+0.5*sin(vUv.y*5.0 - uTime*1.7));

  // phosphor mask
  if(uMask > 0.5){
    float m = mod(gl_FragCoord.x, 3.0);
    vec3 mk = m<1.0 ? vec3(1.0,0.45,0.45) : m<2.0 ? vec3(0.45,1.0,0.45) : vec3(0.45,0.45,1.0);
    col *= mk*1.35;
  }

  // grain
  col += (h2(gl_FragCoord.xy + fract(uTime)*123.0) - 0.5) * uGrain * 0.22;

  // invert strobe + flash
  col = mix(col, 1.0 - col, uInvert);
  col = mix(col, uFlash.rgb, uFlash.a);

  // vignette & CRT edge
  vec2 vv = vUv - 0.5;
  col *= 1.0 - uVig*pow(length(vv)*1.35, 2.6);
  vec2 e = smoothstep(vec2(0.0), vec2(0.004+uCurve*0.012), uv) * smoothstep(vec2(0.0), vec2(0.004+uCurve*0.012), 1.0-uv);
  col *= outside * (uCurve > 0.01 ? e.x*e.y : 1.0);

  o = vec4(max(col, 0.0), 1.0);
}`;
})(window.NS);
