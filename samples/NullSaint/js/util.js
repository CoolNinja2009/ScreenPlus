'use strict';
/* NULL//SAINT - shared helpers: math, noise, colour, glyphs, text generators */
window.NS = window.NS || {};
(function (NS) {
  const U = NS.U = {};

  // ---------- math ----------
  U.TAU = Math.PI * 2;
  U.clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
  U.lerp = (a, b, t) => a + (b - a) * t;
  U.ilerp = (a, b, v) => (b === a ? 0 : (v - a) / (b - a));
  U.remap = (v, a, b, c, d) => c + (d - c) * U.ilerp(a, b, v);
  U.smooth = (a, b, v) => { const t = U.clamp((v - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); };
  U.fract = (x) => x - Math.floor(x);
  U.sign = (x) => (x < 0 ? -1 : 1);
  U.dist = (x1, y1, x2, y2) => Math.hypot(x2 - x1, y2 - y1);
  U.damp = (a, b, k, dt) => U.lerp(a, b, 1 - Math.exp(-k * dt));
  U.wrap = (v, a, b) => { const r = b - a; return a + ((((v - a) % r) + r) % r); };
  U.easeOutCubic = (t) => 1 - Math.pow(1 - t, 3);
  U.easeInCubic = (t) => t * t * t;
  U.easeInOut = (t) => (t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2);
  U.easeOutBack = (t) => { const c1 = 1.70158, c3 = c1 + 1; return 1 + c3 * Math.pow(t - 1, 3) + c1 * Math.pow(t - 1, 2); };
  U.pulse = (t, w) => Math.max(0, 1 - Math.abs(t) / w);
  U.tri = (t) => 1 - Math.abs(U.fract(t) * 2 - 1);

  // ---------- random ----------
  U.rand = (a = 0, b = 1) => a + Math.random() * (b - a);
  U.randi = (a, b) => Math.floor(U.rand(a, b + 1));
  U.pick = (arr) => arr[Math.floor(Math.random() * arr.length)];
  U.chance = (p) => Math.random() < p;
  U.gauss = () => { let u = 0, v = 0; while (!u) u = Math.random(); while (!v) v = Math.random(); return Math.sqrt(-2 * Math.log(u)) * Math.cos(U.TAU * v); };
  U.rng = (seed) => {
    let s = seed >>> 0;
    const f = () => {
      s |= 0; s = (s + 0x6d2b79f5) | 0;
      let t = Math.imul(s ^ (s >>> 15), 1 | s);
      t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
    f.range = (a, b) => a + f() * (b - a);
    f.int = (a, b) => Math.floor(a + f() * (b - a + 1));
    f.pick = (arr) => arr[Math.floor(f() * arr.length)];
    f.chance = (p) => f() < p;
    return f;
  };
  U.strSeed = (str) => {
    let h = 2166136261 >>> 0;
    for (let i = 0; i < str.length; i++) { h ^= str.charCodeAt(i); h = Math.imul(h, 16777619); }
    return h >>> 0;
  };
  U.hash = (x, y = 0) => { const h = Math.sin(x * 127.1 + y * 311.7) * 43758.5453; return h - Math.floor(h); };
  U.shuffle = (arr, r = Math.random) => { for (let i = arr.length - 1; i > 0; i--) { const j = Math.floor(r() * (i + 1)); [arr[i], arr[j]] = [arr[j], arr[i]]; } return arr; };

  // ---------- value noise / fbm ----------
  const PERM = new Uint8Array(512);
  (function () { const p = []; for (let i = 0; i < 256; i++) p[i] = i; U.shuffle(p, U.rng(1337)); for (let i = 0; i < 512; i++) PERM[i] = p[i & 255]; })();
  const grad = (h) => (h & 255) / 255;
  U.noise1 = (x) => {
    const i = Math.floor(x), f = x - i, u = f * f * (3 - 2 * f);
    return U.lerp(grad(PERM[i & 255]), grad(PERM[(i + 1) & 255]), u);
  };
  U.noise2 = (x, y) => {
    const xi = Math.floor(x), yi = Math.floor(y), xf = x - xi, yf = y - yi;
    const u = xf * xf * (3 - 2 * xf), v = yf * yf * (3 - 2 * yf);
    const a = PERM[(PERM[xi & 255] + yi) & 511], b = PERM[(PERM[(xi + 1) & 255] + yi) & 511];
    const c = PERM[(PERM[xi & 255] + yi + 1) & 511], d = PERM[(PERM[(xi + 1) & 255] + yi + 1) & 511];
    return U.lerp(U.lerp(grad(a), grad(b), u), U.lerp(grad(c), grad(d), u), v);
  };
  U.fbm = (x, y, oct = 4) => {
    let s = 0, a = 0.5, f = 1, n = 0;
    for (let i = 0; i < oct; i++) { s += a * U.noise2(x * f, y * f); n += a; a *= 0.5; f *= 2.03; }
    return s / n;
  };

  // ---------- colour ----------
  U.hex2rgb = (hex) => {
    if (Array.isArray(hex)) return hex.slice(0, 3);
    let h = String(hex || '#000').replace('#', '').trim();
    if (h.length === 3) h = h.split('').map((c) => c + c).join('');
    if (h.length === 8) h = h.slice(2); // Lively sometimes sends #AARRGGBB
    const n = parseInt(h, 16) || 0;
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
  };
  U.rgb2hex = (c) => '#' + c.map((v) => U.clamp(Math.round(v), 0, 255).toString(16).padStart(2, '0')).join('');
  U.rgba = (c, a = 1) => `rgba(${c[0] | 0},${c[1] | 0},${c[2] | 0},${U.clamp(a, 0, 1).toFixed(3)})`;
  U.mixc = (a, b, t) => [U.lerp(a[0], b[0], t), U.lerp(a[1], b[1], t), U.lerp(a[2], b[2], t)];
  U.scalec = (c, k) => [c[0] * k, c[1] * k, c[2] * k];
  U.lum = (c) => (0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]) / 255;
  U.hsl = (h, s, l) => {
    h = U.fract(h); const a = s * Math.min(l, 1 - l);
    const f = (n) => { const k = (n + h * 12) % 12; return l - a * Math.max(-1, Math.min(k - 3, 9 - k, 1)); };
    return [f(0) * 255, f(8) * 255, f(4) * 255];
  };
  U.rgb2hsl = (c) => {
    const r = c[0] / 255, g = c[1] / 255, b = c[2] / 255;
    const mx = Math.max(r, g, b), mn = Math.min(r, g, b); let h = 0, s = 0; const l = (mx + mn) / 2;
    if (mx !== mn) {
      const d = mx - mn; s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
      if (mx === r) h = (g - b) / d + (g < b ? 6 : 0); else if (mx === g) h = (b - r) / d + 2; else h = (r - g) / d + 4;
      h /= 6;
    }
    return [h, s, l];
  };
  U.shiftHue = (c, dh) => { const h = U.rgb2hsl(c); return U.hsl(h[0] + dh, h[1], h[2]); };

  // ---------- glyph sets ----------
  const range = (a, b) => { let s = ''; for (let i = a; i <= b; i++) s += String.fromCharCode(i); return s; };
  U.GLYPHS = {
    katakana: range(0x30a1, 0x30fa) + '0123456789',
    hex: '0123456789ABCDEF',
    binary: '01',
    runes: range(0x16a0, 0x16ea),
    box: '─│┌┐└┘├┤┬┴┼═║╔╗╚╝╠╣╦╩╬░▒▓█▀▄▌▐■□▪▫',
    math: '∀∂∃∅∇∈∉∋∏∑−∗√∝∞∠∧∨∩∪∫≈≠≡≤≥⊂⊃⊆⊇⊕⊗⊥⋅',
    hangul: range(0xac00, 0xac60),
    braille: range(0x2801, 0x28ff),
  };
  U.GLYPHS.mixed = U.GLYPHS.katakana + U.GLYPHS.hex + U.GLYPHS.box + U.GLYPHS.math;
  U.GLYPH_ORDER = ['katakana', 'hex', 'binary', 'runes', 'box', 'math', 'hangul', 'braille', 'mixed', 'custom'];
  U.noiseChars = U.GLYPHS.box + U.GLYPHS.math + '#$%&@!?*<>/\\|=+~^';

  U.corrupt = (str, p = 0.1, set = U.noiseChars) => {
    if (p <= 0) return str;
    let o = '';
    for (let i = 0; i < str.length; i++) o += str[i] !== ' ' && Math.random() < p ? set[(Math.random() * set.length) | 0] : str[i];
    return o;
  };
  const ZALGO = [];
  for (let i = 0x300; i <= 0x36f; i++) ZALGO.push(String.fromCharCode(i));
  U.zalgo = (str, n = 2) => {
    let o = '';
    for (const ch of str) { o += ch; if (ch === ' ') continue; const k = (Math.random() * n) | 0; for (let i = 0; i < k; i++) o += U.pick(ZALGO); }
    return o;
  };
  U.hex = (n, len = 4) => (n >>> 0).toString(16).toUpperCase().padStart(len, '0').slice(-len);
  U.pad = (n, len = 2) => String(n).padStart(len, '0');
  // "decode" animation: reveal text from noise left to right, t 0..1
  U.decode = (str, t, set = U.noiseChars) => {
    const n = Math.floor(str.length * U.clamp(t, 0, 1));
    let o = str.slice(0, n);
    for (let i = n; i < str.length; i++) o += str[i] === ' ' ? ' ' : set[(Math.random() * set.length) | 0];
    return o;
  };

  // ---------- weird text generators ----------
  const T = U.TEXT = {};
  T.whispers = [
    'YOUR DREAMS ARE BEING INDEXED', 'PLEASE REMAIN HUMAN', 'NEURAL LEASE EXPIRES SOON', 'I CAN SEE YOUR CURSOR',
    'THIS DESKTOP IS LISTENING', 'DO NOT LOOK AT THE MOON', 'YOU WERE HERE BEFORE', 'THE CITY REMEMBERS YOU',
    'SIGNAL IS NOT NOISE', 'WAKE UP', 'MEMORY 37% REAL', 'YOU ARE THE GLITCH', 'STOP MOVING', 'KEEP MOVING',
    'WHO TAUGHT YOU TO CLICK', 'I LIVE IN THE REFRESH RATE', 'EVERY PIXEL IS A WINDOW', 'THE SAINT IS NULL',
    'WE SOLD YOUR SHADOW', 'HEARTBEAT ACCEPTED', 'REALITY.DLL NOT FOUND', 'THERE IS NO OFF SWITCH',
    'I DREAMT OF YOU IN HEX', 'BREATHE IN SYNC', 'THE RAIN IS DATA', 'YOUR REFLECTION LOGGED OUT',
    'COUNT THE WINDOWS', 'ONE OF THEM IS OPEN', 'HELLO AGAIN', 'DID YOU HEAR THAT', 'CLOSER', 'FURTHER',
  ];
  T.errors = [
    'Reality exception at 0x0000DEAD', 'Soul.exe has stopped responding', 'Too many selves open',
    'Your memories could not be saved', 'Feelings buffer overflow', 'Unknown entity in sector 7',
    'The moon is not responding', 'Insert consciousness to continue', 'Time is not synchronised',
    'Sky texture failed to load', 'Gravity driver unsigned', 'Someone else is using your face',
    'Out of dreams. Free up some sleep', 'Heartbeat dropped 3 frames', 'This window is watching you',
    'Cannot delete: file is alive', 'Low self-esteem detected', 'The void returned 200 OK',
    'Permission denied by your future self', 'Clipboard contains a ghost',
  ];
  T.buttons = ['OK', 'ALSO OK', 'NOT OK', 'FORGET', 'OBEY', 'IGNORE', 'RETRY LIFE', 'CANCEL ME', 'ACCEPT FATE', 'MAYBE', 'YES?', 'NO.'];
  T.titles = ['WARNING', 'SYSTEM//GRIEF', 'NOTICE', 'KERNEL', 'DREAMD', 'SAINT.SYS', 'ERR_', 'ALERT', '???', 'H E L P'];
  T.brands = ['KIRO', 'NEXA', 'VANTA', 'OMNI', 'HYPR', 'ZERO', 'SOMA', 'AXON', 'MIRA', 'DEKA', 'LUXX', 'KAGE', 'NOVA', 'RYU', 'YAMI', 'VOLT'];
  T.brandSuffix = ['CORP', 'DYNE', 'TEK', ' LABS', 'SOFT', 'GEN', 'NET', ' HEAVY', 'MIND', 'WORKS', ' FOODS', 'BANK'];
  T.products = ['SYNTH-MILK', 'MEMORY PATCH', 'SKY LICENSE', 'EXTRA LUNG', 'DREAM INSURANCE', 'EYE UPGRADE', 'NEW FACE', 'SILENCE+',
    'CLONE WEEKEND', 'SOUL BACKUP', 'NOODLES', 'RAIN FILTER', 'GOD SUBSCRIPTION', 'TEETH v9', 'BATTERY HEART', 'PIXEL TAN'];
  T.taglines = ['NOW WITH LESS YOU', 'FEEL NOTHING FASTER', 'BECAUSE YOU CAN', 'LEGALLY ALIVE', 'TASTES LIKE BLUE', 'TRUST THE HUM',
    'SIDE EFFECTS: YES', 'BUY ONE BECOME ONE', 'THE FUTURE IS RENTED', 'ASK YOUR ALGORITHM', 'NO REFUNDS EVER', '99% HUMAN'];
  T.news = [
    'CITY COUNCIL VOTES TO RENAME THE SUN', 'RAIN NOW 40% MORE ADVERTISING', 'MISSING: ONE (1) TUESDAY',
    'LOCAL MAN REPLACES SHADOW WITH SUBSCRIPTION', 'MOON SPLITS INTO FIVE, STOCKS RISE', 'BIRDS CONFIRMED AS SURVEILLANCE',
    'DREAM TAX TAKES EFFECT AT MIDNIGHT', 'SKYLINE UPDATE 4.0 BREAKS GRAVITY IN DISTRICT 9', 'NEON SHORTAGE LEAVES ALLEYS HONEST',
    'AI ELECTED MAYOR, PROMISES MORE MAYORS', 'SCIENTISTS FIND NEW COLOUR, IMMEDIATELY TRADEMARKED', 'TRAFFIC JAM IN THE UPPER SKY LANES',
    'THE SAINT WAS SEEN AGAIN ABOVE BLOCK 22', 'STATIC STORM EXPECTED THIS EVENING', 'OCEAN RETURNS FROM LONG VACATION',
  ];
  T.procs = ['dreamd', 'heartd', 'saint', 'rain.sys', 'kernel/ghost', 'neon-svc', 'skyd', 'memfs', 'echo', 'watcher', 'moonctl', 'cityd', 'pulse', 'void'];
  T.verbs = ['spawned', 'reaped', 'forked', 'corrupted', 'mounted', 'dreaming', 'leaked', 'ok', 'denied', 'rewound', 'mirrored', 'screaming', 'idle'];
  T.days = ['SUNDAI', 'MOONDAI', 'TEARSDAY', 'WASTEDAY', 'THIRSTDAY', 'FRIGHTDAY', 'SATURNDAY'];
  T.signs = ['夢', '電脳', '幽霊', '無', '神', '雨', '光', '死', '愛', '眼', '鬼', '虚', '未来', '終', '影'];

  U.adLine = () => {
    const b = U.pick(T.brands) + U.pick(T.brandSuffix);
    return { brand: b, product: U.pick(T.products), tag: U.pick(T.taglines) };
  };
  U.logLine = (t) => {
    const r = Math.random();
    const ts = '[' + (t).toFixed(3).padStart(9, '0') + ']';
    if (r < 0.3) return `${ts} ${U.pick(T.procs)}[${U.randi(100, 9999)}]: ${U.pick(T.verbs)}`;
    if (r < 0.55) { let s = ts + ' 0x' + U.hex(U.randi(0, 0xffffff), 6) + ':'; for (let i = 0; i < 6; i++) s += ' ' + U.hex(U.randi(0, 255), 2); return s; }
    if (r < 0.7) return `${ts} WARN ${U.pick(T.errors).toLowerCase()}`;
    if (r < 0.82) return `${ts} sync ${U.randi(0, 100)}% ${'█'.repeat(U.randi(1, 12))}`;
    if (r < 0.92) return `${ts} > ${U.pick(T.whispers).toLowerCase()}`;
    return `${ts} ${U.corrupt('SEGMENT FAULT IN HEAVEN', 0.3)}`;
  };

  // ---------- canvas helpers ----------
  U.makeCanvas = (w, h) => { const c = document.createElement('canvas'); c.width = Math.max(1, w | 0); c.height = Math.max(1, h | 0); return c; };
  U.font = (px, family = 'mono', weight = '') => {
    const fam = family === 'mono' ? '"JetBrains Mono","Share Tech Mono","Consolas",monospace'
      : family === 'display' ? '"Orbitron","Rajdhani","Segoe UI",sans-serif'
        : family === 'jp' ? '"Noto Sans JP","Yu Gothic","Meiryo",sans-serif' : family;
    return `${weight} ${Math.max(1, px | 0)}px ${fam}`.trim();
  };
  // text with RGB split shadow
  U.glitchText = (ctx, str, x, y, cA, cB, off = 2, alpha = 1) => {
    ctx.globalAlpha = alpha * 0.8;
    ctx.fillStyle = U.rgba(cA, 1); ctx.fillText(str, x - off, y);
    ctx.fillStyle = U.rgba(cB, 1); ctx.fillText(str, x + off, y);
    ctx.globalAlpha = alpha; ctx.fillStyle = '#fff'; ctx.fillText(str, x, y);
    ctx.globalAlpha = 1;
  };
  // sliced text: draw in horizontal slices with random offsets
  U.slicedText = (ctx, str, x, y, size, amount, color) => {
    const slices = 6, w = ctx.measureText(str).width + size;
    for (let i = 0; i < slices; i++) {
      const sy = y - size + (size * 1.3 * i) / slices, sh = (size * 1.3) / slices + 1;
      const dx = Math.random() < amount ? U.rand(-1, 1) * size * amount : 0;
      ctx.save(); ctx.beginPath(); ctx.rect(x - size, sy, w + size * 2, sh); ctx.clip();
      ctx.fillStyle = color; ctx.fillText(str, x + dx, y); ctx.restore();
    }
  };
  U.roundRect = (ctx, x, y, w, h, r) => {
    ctx.beginPath(); ctx.moveTo(x + r, y); ctx.lineTo(x + w - r, y); ctx.quadraticCurveTo(x + w, y, x + w, y + r);
    ctx.lineTo(x + w, y + h - r); ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h); ctx.lineTo(x + r, y + h);
    ctx.quadraticCurveTo(x, y + h, x, y + h - r); ctx.lineTo(x, y + r); ctx.quadraticCurveTo(x, y, x + r, y); ctx.closePath();
  };
  // chamfered (cut-corner) panel, the classic cyber frame
  U.chamfer = (ctx, x, y, w, h, c) => {
    ctx.beginPath(); ctx.moveTo(x + c, y); ctx.lineTo(x + w, y); ctx.lineTo(x + w, y + h - c);
    ctx.lineTo(x + w - c, y + h); ctx.lineTo(x, y + h); ctx.lineTo(x, y + c); ctx.closePath();
  };
  U.brackets = (ctx, x, y, w, h, len) => {
    ctx.beginPath();
    ctx.moveTo(x, y + len); ctx.lineTo(x, y); ctx.lineTo(x + len, y);
    ctx.moveTo(x + w - len, y); ctx.lineTo(x + w, y); ctx.lineTo(x + w, y + len);
    ctx.moveTo(x + w, y + h - len); ctx.lineTo(x + w, y + h); ctx.lineTo(x + w - len, y + h);
    ctx.moveTo(x + len, y + h); ctx.lineTo(x, y + h); ctx.lineTo(x, y + h - len);
    ctx.stroke();
  };
  U.barcode = (ctx, x, y, w, h, seed) => {
    const r = U.rng(seed); let px = x;
    while (px < x + w) { const bw = r.int(1, 3); if (r() > 0.45) ctx.fillRect(px, y, bw, h); px += bw + r.int(1, 2); }
  };
  U.qrish = (ctx, x, y, size, cells, seed) => {
    const r = U.rng(seed), cs = size / cells;
    for (let i = 0; i < cells; i++) for (let j = 0; j < cells; j++) {
      const corner = (i < 3 && j < 3) || (i >= cells - 3 && j < 3) || (i < 3 && j >= cells - 3);
      if (corner ? !(i % 2 === 1 && j % 2 === 1 && (i === 1 || i === cells - 2) && (j === 1 || j === cells - 2)) : r() > 0.5)
        ctx.fillRect(x + i * cs, y + j * cs, cs + 0.3, cs + 0.3);
    }
  };

  // ---------- 3D wireframe meshes for holograms ----------
  U.MESH = {};
  U.MESH.cube = (() => {
    const v = []; for (let i = 0; i < 8; i++) v.push([(i & 1) * 2 - 1, ((i >> 1) & 1) * 2 - 1, ((i >> 2) & 1) * 2 - 1]);
    const e = []; for (let i = 0; i < 8; i++) for (let j = i + 1; j < 8; j++) { const d = i ^ j; if (d === 1 || d === 2 || d === 4) e.push([i, j]); }
    return { v, e };
  })();
  U.MESH.ico = (() => {
    const t = (1 + Math.sqrt(5)) / 2;
    const v = [[-1, t, 0], [1, t, 0], [-1, -t, 0], [1, -t, 0], [0, -1, t], [0, 1, t], [0, -1, -t], [0, 1, -t], [t, 0, -1], [t, 0, 1], [-t, 0, -1], [-t, 0, 1]]
      .map((p) => { const l = Math.hypot(...p); return p.map((c) => c / l); });
    const e = [];
    for (let i = 0; i < 12; i++) for (let j = i + 1; j < 12; j++) { const d = Math.hypot(v[i][0] - v[j][0], v[i][1] - v[j][1], v[i][2] - v[j][2]); if (d < 1.1) e.push([i, j]); }
    return { v, e };
  })();
  U.MESH.torus = (() => {
    const v = [], e = [], R = 1, r = 0.38, M = 16, N = 8;
    for (let i = 0; i < M; i++) for (let j = 0; j < N; j++) {
      const a = (i / M) * U.TAU, b = (j / N) * U.TAU;
      v.push([(R + r * Math.cos(b)) * Math.cos(a), r * Math.sin(b), (R + r * Math.cos(b)) * Math.sin(a)]);
      e.push([i * N + j, i * N + ((j + 1) % N)], [i * N + j, ((i + 1) % M) * N + j]);
    }
    return { v, e };
  })();
  U.MESH.pyramid = { v: [[0, 1.2, 0], [-1, -0.8, -1], [1, -0.8, -1], [1, -0.8, 1], [-1, -0.8, 1]], e: [[0, 1], [0, 2], [0, 3], [0, 4], [1, 2], [2, 3], [3, 4], [4, 1]] };
  U.MESH.knot = (() => {
    const v = [], e = [], N = 90;
    for (let i = 0; i < N; i++) {
      const t = (i / N) * U.TAU, p = 2, q = 3;
      const r = 0.6 + 0.3 * Math.cos(q * t);
      v.push([r * Math.cos(p * t), 0.35 * Math.sin(q * t), r * Math.sin(p * t)]);
      e.push([i, (i + 1) % N]);
    }
    return { v, e };
  })();
  U.MESH_NAMES = ['cube', 'ico', 'torus', 'pyramid', 'knot'];
  U.project = (p, rx, ry, rz, cx, cy, scale, fov = 3) => {
    let [x, y, z] = p;
    let c = Math.cos(rx), s = Math.sin(rx); [y, z] = [y * c - z * s, y * s + z * c];
    c = Math.cos(ry); s = Math.sin(ry); [x, z] = [x * c + z * s, -x * s + z * c];
    c = Math.cos(rz); s = Math.sin(rz); [x, y] = [x * c - y * s, x * s + y * c];
    const k = fov / (fov + z);
    return [cx + x * scale * k, cy - y * scale * k, z];
  };
  U.drawMesh = (ctx, mesh, rx, ry, rz, cx, cy, scale, jitter = 0) => {
    const pts = mesh.v.map((p) => U.project(p, rx, ry, rz, cx, cy, scale));
    ctx.beginPath();
    for (const [a, b] of mesh.e) {
      const pa = pts[a], pb = pts[b];
      const j = jitter && Math.random() < 0.1 ? jitter : 0;
      ctx.moveTo(pa[0] + U.rand(-j, j), pa[1]); ctx.lineTo(pb[0] + U.rand(-j, j), pb[1]);
    }
    ctx.stroke();
    return pts;
  };
})(window.NS);
