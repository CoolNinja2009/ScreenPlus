'use strict';
/* NULL//SAINT - settings schema (also generates LivelyProperties.json), palettes, presets, live settings */
(function (NS) {
  const U = NS.U;

  // ---------- palettes ----------
  // bg: sky top, bg2: sky bottom/horizon, a/b/c: neons, text, warn
  NS.PALETTES = [
    { name: 'Neon Tokyo', bg: '#05010f', bg2: '#2a0636', a: '#ff2bd6', b: '#00f0ff', c: '#ffe600', text: '#e8f7ff', warn: '#ff2a3d' },
    { name: 'Toxic Sludge', bg: '#010702', bg2: '#0c2a08', a: '#7dff00', b: '#00ff9c', c: '#e1ff00', text: '#d6ffd0', warn: '#ff4800' },
    { name: 'Blood Moon', bg: '#080000', bg2: '#3a0006', a: '#ff0033', b: '#ff7a00', c: '#ffd6d6', text: '#ffe6e6', warn: '#ffffff' },
    { name: 'Ice Ghost', bg: '#00040b', bg2: '#0b2538', a: '#8ffcff', b: '#4d7cff', c: '#ffffff', text: '#eaffff', warn: '#ff3c7f' },
    { name: 'Vapor Mall', bg: '#120024', bg2: '#ff4fa3', a: '#ff71ce', b: '#01cdfe', c: '#fffb96', text: '#fff2ff', warn: '#b967ff' },
    { name: 'Amber Terminal', bg: '#060300', bg2: '#2b1300', a: '#ffb000', b: '#ff6a00', c: '#fff1c4', text: '#ffd88a', warn: '#ff2200' },
    { name: 'Rain Code', bg: '#000300', bg2: '#001a06', a: '#00ff41', b: '#008f11', c: '#c8ffd4', text: '#b6ffc8', warn: '#ff0040' },
    { name: 'Ultraviolet', bg: '#03000a', bg2: '#1d0047', a: '#a100ff', b: '#ff00a8', c: '#00ffd5', text: '#f2e6ff', warn: '#ffef00' },
    { name: 'Dead Channel', bg: '#030303', bg2: '#1c1c1c', a: '#ffffff', b: '#9a9a9a', c: '#ff0040', text: '#f0f0f0', warn: '#ff0040' },
    { name: 'Acid Candy', bg: '#0a0012', bg2: '#003b3b', a: '#ff3cac', b: '#2bff88', c: '#2b86c5', text: '#fff', warn: '#ffe100' },
    { name: 'Chrome Sunset', bg: '#07000f', bg2: '#ff3d00', a: '#ff9e00', b: '#ff006e', c: '#8338ec', text: '#fff4e0', warn: '#00f5d4' },
    { name: 'Custom', bg: '#05010f', bg2: '#2a0636', a: '#ff2bd6', b: '#00f0ff', c: '#ffe600', text: '#e8f7ff', warn: '#ff2a3d' },
  ];

  // ---------- schema ----------
  // t: s=slider c=checkbox d=dropdown col=color txt=textbox h=heading(label)
  const S = (k, text, v, min, max, step = 1) => ({ k, t: 's', text, v, min, max, step });
  const C = (k, text, v) => ({ k, t: 'c', text, v });
  const D = (k, text, v, items) => ({ k, t: 'd', text, v, items });
  const COL = (k, text, v) => ({ k, t: 'col', text, v });
  const TXT = (k, text, v) => ({ k, t: 'txt', text, v });
  const H = (k, text) => ({ k, t: 'h', text });
  const FD = (k, text, folder, filter) => ({ k, t: 'fd', text, v: '', folder, filter });

  NS.SCHEMA = [
    H('h_core', '▓▓ CORE ▓▓'),
    D('preset', 'Preset (applies on change)', 0, ['Keep my settings', 'Balanced', 'Calm Night', 'Total Meltdown', 'Retro CRT', 'Ghost In The Wires', 'Minimal HUD', 'Seizure-Safe']),
    D('theme', 'Colour theme', 0, NS.PALETTES.map((p) => p.name)),
    COL('colA', 'Custom neon A', '#ff2bd6'),
    COL('colB', 'Custom neon B', '#00f0ff'),
    COL('colC', 'Custom neon C', '#ffe600'),
    COL('colBg', 'Custom sky top', '#05010f'),
    COL('colBg2', 'Custom horizon', '#2a0636'),
    S('hueCycle', 'Hue drift speed', 0, 0, 100),
    S('intensity', 'Glitch intensity (%)', 100, 0, 250, 5),
    S('chaos', 'Chaos (event frequency)', 45, 0, 100),
    S('speed', 'Time speed (%)', 100, 10, 300, 5),
    D('quality', 'Render resolution', 1, ['50% (fast)', '75%', '100% (sharp)']),
    D('fps', 'Frame cap', 1, ['30 fps', '60 fps', 'Unlimited']),
    C('boot', 'Boot sequence on start', true),
    TXT('seed', 'Reality seed (changes the city)', 'NULLSAINT'),
    C('safe', 'Seizure-safe mode (no strobes or flashes)', false),

    H('h_city', '▓▓ MEGACITY ▓▓'),
    C('city', 'Show city', true),
    S('cityDensity', 'Building density', 60, 10, 100),
    S('cityHeight', 'Skyscraper height', 60, 10, 100),
    S('parallax', 'Mouse parallax', 50, 0, 100),
    D('moon', 'Sky object', 1, ['None', 'Broken moon', 'Striped sun', 'Black hole', 'Giant eye', 'Planet rings']),
    S('moonSize', 'Sky object size', 50, 10, 100),
    C('signs', 'Neon kanji signs', true),
    S('cars', 'Flying traffic', 50, 0, 100),
    S('weather', 'Rain', 40, 0, 100),
    S('fog', 'Smog', 45, 0, 100),
    C('searchlights', 'Searchlights', true),
    C('lightning', 'Data lightning', true),
    C('grid', 'Synth grid floor', false),

    H('h_glyph', '▓▓ GLYPH RAIN ▓▓'),
    C('glyphs', 'Glyph rain', true),
    D('glyphSet', 'Glyph set', 0, ['Katakana', 'Hex', 'Binary', 'Runes', 'Box drawing', 'Math', 'Hangul', 'Braille', 'Mixed', 'Custom']),
    TXT('glyphCustom', 'Custom glyphs', 'NULLSAINT01'),
    S('glyphDensity', 'Glyph density', 35, 0, 100),
    S('glyphSpeed', 'Glyph speed', 50, 5, 100),
    S('glyphSize', 'Glyph size', 16, 8, 40),
    S('glyphRepel', 'Cursor repels glyphs', 60, 0, 100),
    D('glyphColor', 'Glyph colour', 1, ['Neon A', 'Neon B', 'Neon C', 'Rainbow', 'Audio']),

    H('h_saint', '▓▓ THE SAINT ▓▓'),
    C('saint', 'Show the Saint', true),
    D('saintStyle', 'Form', 0, ['Scanline face', 'Single eye', 'Particle swarm', 'Random each visit']),
    D('saintPos', 'Position', 0, ['Centre', 'Left', 'Right', 'Haunts the cursor', 'Wanders']),
    S('saintSize', 'Size', 55, 15, 100),
    S('saintTrack', 'Eye tracking', 80, 0, 100),
    S('saintAlpha', 'Opacity', 75, 10, 100),
    C('saintTalk', 'The Saint speaks', true),
    TXT('saintWords', 'Its words (separate with |)', ''),
    C('saintBlink', 'Blinks', true),

    H('h_girl', '▓▓ KEI (CYBER GIRL) ▓▓'),
    C('girl', 'Show Kei, the cyber girl', false),
    D('girlMode', 'Look', 0, ['Drawn (Kei)', 'Your own image']),
    D('girlPos', 'Stands on the', 1, ['Left', 'Right', 'Side away from the cursor']),
    S('girlSize', 'Size', 45, 10, 100),
    D('girlHair', 'Hair colour', 0, ['Neon A', 'Neon B', 'Silver', 'Midnight', 'Sakura', 'Mint']),
    D('girlAcc', 'Accessory', 0, ['Cat-ear headphones', 'Holo visor', 'Both', 'None']),
    C('girlUmbrella', 'Neon umbrella', true),
    C('girlTalk', 'She chats', true),
    C('girlFlip', 'Mirror her', false),
    FD('girlFile', 'Your image (PNG with transparency works best)', 'girls', '*.png|*.webp|*.gif|*.jpg'),
    TXT('girlUrl', 'Or image URL / path', ''),

    H('h_photo', '▓▓ PHOTO SIGNAL ▓▓'),
    D('photo', 'Photo layer', 0, ['None', 'Neon streets (bundled, CC0)', 'Your own photo']),
    D('photoPlace', 'Photo placement', 0, ['Sky backdrop', 'Ghost overlay', 'Replace the city']),
    S('photoMix', 'Photo strength', 55, 5, 100),
    S('photoDrift', 'Photo drift', 30, 0, 100),
    C('photoTint', 'Tint photo with theme', true),
    FD('photoFile', 'Your photo', 'photos', '*.jpg|*.jpeg|*.png|*.webp'),

    H('h_holo', '▓▓ HOLOGRAMS & ADS ▓▓'),
    C('ads', 'Holo billboards', true),
    S('adCount', 'Billboard count', 5, 0, 12),
    C('holo', 'Floating wireframe artifact', true),
    D('holoShape', 'Artifact shape', 5, ['Cube', 'Icosahedron', 'Torus', 'Pyramid', 'Knot', 'Cycle']),
    S('holoSize', 'Artifact size', 40, 10, 100),

    H('h_hud', '▓▓ HUD ▓▓'),
    C('hud', 'Show HUD', true),
    C('hudGlitch', 'HUD goes through the glitch', false),
    C('clock', 'Clock', true),
    C('clock24', '24-hour clock', true),
    C('logs', 'Terminal logs', true),
    C('sys', 'System monitor', true),
    C('reticle', 'Cursor reticle', true),
    C('ticker', 'News ticker', true),
    C('popups', 'Haunted error popups', true),
    S('popupRate', 'Popup frequency', 30, 0, 100),
    TXT('operator', 'Operator name', 'OPERATOR'),
    TXT('messages', 'Your own messages (separate with |)', ''),
    S('hudScale', 'HUD scale (%)', 100, 60, 180, 5),

    H('h_post', '▓▓ SIGNAL DAMAGE ▓▓'),
    S('rgb', 'RGB split', 45, 0, 100),
    S('scan', 'Scanlines', 45, 0, 100),
    S('curve', 'CRT curvature', 20, 0, 100),
    S('grain', 'Static grain', 30, 0, 100),
    S('blocks', 'Block corruption', 35, 0, 100),
    S('tear', 'Line tearing', 30, 0, 100),
    S('vhs', 'VHS tracking', 25, 0, 100),
    S('mosh', 'Datamosh', 30, 0, 100),
    S('sort', 'Pixel sorting', 20, 0, 100),
    S('bloom', 'Neon bloom', 60, 0, 100),
    S('vignette', 'Vignette', 50, 0, 100),
    S('wave', 'Signal wave', 10, 0, 100),
    S('pixelate', 'Pixelate', 0, 0, 100),
    S('posterize', 'Posterize', 0, 0, 100),
    C('dither', 'Ordered dither', false),
    C('mask', 'CRT phosphor mask', false),
    D('ascii', 'ASCII vision', 1, ['Off', 'During events', 'Always']),
    D('colorMode', 'Colour mode', 0, ['Normal', 'Thermal', 'Night vision', 'Inverted', '1-bit', 'Sepia rot', 'Chroma hell']),
    D('mirror', 'Mirror', 0, ['Off', 'Left/right', 'Quad', 'Kaleidoscope']),

    H('h_touch', '▓▓ INTERACTION ▓▓'),
    S('trail', 'Cursor smear', 55, 0, 100),
    S('lens', 'Cursor lens warp', 35, 0, 100),
    D('click', 'Click effect', 4, ['Nothing', 'Shockwave', 'Data burst', 'Reality tear', 'Random', 'Everything']),
    S('clickPower', 'Click power', 60, 0, 100),
    C('idleRot', 'Idle rot (decays while you are away)', true),
    S('idleTime', 'Idle rot after (s)', 60, 10, 600, 10),
    C('keys', 'Keyboard triggers (browser)', true),

    H('h_audio', '▓▓ AUDIO ▓▓'),
    C('audio', 'React to system audio', true),
    S('audioGain', 'Audio gain', 100, 10, 400, 10),
    C('audioRing', 'Spectrum ring', true),
    C('beatGlitch', 'Glitch on bass hits', true),
    C('fakeBeat', 'Fake pulse when silent', false),

    H('h_sound', '▓▓ SOUND (OFF BY DEFAULT) ▓▓'),
    C('sound', 'Synth glitch sounds', false),
    S('soundVol', 'Sound volume', 40, 0, 100, 5),
    C('drone', 'Low city drone', true),

    H('h_ghost', '▓▓ PHANTOMS ▓▓'),
    C('ghosts', 'Phantom cursors replay your past moves', true),
    S('ghostCount', 'Max phantoms at once', 2, 1, 6),

    H('h_events', '▓▓ EVENTS ▓▓'),
    C('events', 'Random glitch events', true),
    C('evBreach', 'Security breach', true),
    C('evMelt', 'Meltdown', true),
    C('evShuffle', 'Tile shuffle', true),
    C('evInvert', 'Negative strobe', true),
    C('evPanic', 'Kernel panic screen', true),
    C('evWorm', 'Corruption worm', true),
    C('evAscii', 'ASCII collapse', true),
    C('evBlackout', 'Blackout (it sees you)', true),
    C('evTakeover', 'Saint takeover', true),
    C('evSignal', 'Signal lost', true),
    C('evMirror', 'Kaleidoscope slip', true),
    C('evFreeze', 'Time freeze', true),
  ];

  // ---------- presets ----------
  NS.PRESETS = [
    null,
    {}, // balanced = defaults
    { intensity: 45, chaos: 12, rgb: 25, blocks: 10, tear: 10, mosh: 10, sort: 5, grain: 20, weather: 70, glyphDensity: 20, popups: false, theme: 3, speed: 70, evPanic: false, evInvert: false, evBreach: false },
    { intensity: 230, chaos: 100, rgb: 100, blocks: 90, tear: 85, vhs: 70, mosh: 75, sort: 70, wave: 40, grain: 70, popupRate: 90, glyphDensity: 70, theme: 2, speed: 140 },
    { theme: 5, curve: 75, scan: 90, mask: true, vhs: 55, grain: 55, bloom: 80, rgb: 55, colorMode: 0, glyphSet: 1, dither: false },
    { theme: 6, glyphDensity: 80, glyphSet: 0, saintStyle: 2, saintSize: 85, ascii: 1, mosh: 50, colorMode: 0 },
    { intensity: 35, chaos: 5, city: true, glyphs: false, ads: false, holo: false, popups: false, rgb: 15, blocks: 5, tear: 5, mosh: 5, sort: 0, grain: 15, events: false, theme: 8 },
    { safe: true, intensity: 40, chaos: 10, evInvert: false, evPanic: false, evBlackout: false, evSignal: false, beatGlitch: false, blocks: 15, tear: 10 },
  ];

  // ---------- live settings ----------
  const defaults = {};
  for (const e of NS.SCHEMA) if (e.t !== 'h') defaults[e.k] = e.v;
  NS.DEFAULTS = defaults;
  const cfg = NS.cfg = Object.assign({}, defaults);
  const listeners = [];
  NS.onConfig = (fn) => listeners.push(fn);

  const coerce = (e, v) => {
    switch (e.t) {
      case 's': return U.clamp(Number(v) || 0, e.min, e.max);
      case 'c': return v === true || v === 'true' || v === 1 || v === '1';
      case 'd': return U.clamp(parseInt(v, 10) || 0, 0, e.items.length - 1);
      case 'fd': return String(v == null ? '' : v).replace(/^.*[\\/]/, '');
      case 'col': return typeof v === 'string' ? v : U.rgb2hex(U.hex2rgb(v));
      default: return String(v == null ? '' : v);
    }
  };
  const byKey = {};
  for (const e of NS.SCHEMA) byKey[e.k] = e;
  NS.schemaOf = (k) => byKey[k];

  NS.set = (k, v, quiet) => {
    const e = byKey[k]; if (!e || e.t === 'h') return;
    const val = coerce(e, v);
    if (cfg[k] === val && k !== 'preset') return;
    cfg[k] = val;
    if (k === 'preset' && val > 0) { NS.applyPreset(val); return; }
    if (!quiet) for (const fn of listeners) fn(k, val);
    NS.save();
  };
  NS.applyPreset = (i) => {
    const p = NS.PRESETS[i]; if (!p) return;
    for (const k in defaults) if (k !== 'preset' && k !== 'seed' && k !== 'operator' && k !== 'messages' && k !== 'saintWords') cfg[k] = defaults[k];
    for (const k in p) cfg[k] = coerce(byKey[k], p[k]);
    for (const fn of listeners) fn('*', null);
    NS.save();
  };
  NS.notify = () => { for (const fn of listeners) fn('*', null); NS.save(); };
  NS.reset = () => { Object.assign(cfg, defaults); for (const fn of listeners) fn('*', null); NS.save(); };

  // persistence only for the in-page panel (browser use); the wallpaper host owns its own values
  const LS = 'nullsaint.cfg.v1';
  NS.hosted = false;
  NS.save = () => { if (NS.hosted) return; try { localStorage.setItem(LS, JSON.stringify(cfg)); } catch (e) { /* no storage */ } };
  NS.load = () => {
    try {
      const raw = localStorage.getItem(LS);
      if (raw) { const o = JSON.parse(raw); for (const k in o) if (byKey[k] && byKey[k].t !== 'h') cfg[k] = coerce(byKey[k], o[k]); }
    } catch (e) { /* ignore */ }
    // URL overrides: ?theme=3&intensity=200
    try {
      const q = new URLSearchParams(location.search);
      q.forEach((v, k) => { if (byKey[k]) cfg[k] = coerce(byKey[k], v); });
    } catch (e) { /* ignore */ }
  };

  // ---------- derived palette ----------
  NS.palette = () => {
    const i = cfg.theme, base = NS.PALETTES[i] || NS.PALETTES[0];
    const custom = base.name === 'Custom';
    const P = {
      bg: U.hex2rgb(custom ? cfg.colBg : base.bg), bg2: U.hex2rgb(custom ? cfg.colBg2 : base.bg2),
      a: U.hex2rgb(custom ? cfg.colA : base.a), b: U.hex2rgb(custom ? cfg.colB : base.b), c: U.hex2rgb(custom ? cfg.colC : base.c),
      text: U.hex2rgb(base.text), warn: U.hex2rgb(base.warn),
    };
    return P;
  };

  // ---------- Lively / Screen+ host hooks ----------
  window.livelyPropertyListener = function (name, val) {
    NS.hosted = true;
    // Lively colour pickers send "#RRGGBB"; sliders numbers; dropdowns indices; checkboxes booleans
    NS.set(name, val);
  };

  // ---------- LivelyProperties.json generator (run NS.livelyJSON() in a console or node) ----------
  NS.livelyJSON = () => {
    const out = {};
    for (const e of NS.SCHEMA) {
      if (e.t === 'h') out[e.k] = { type: 'label', value: e.text };
      else if (e.t === 's') out[e.k] = { type: 'slider', value: e.v, min: e.min, max: e.max, step: e.step, text: e.text };
      else if (e.t === 'c') out[e.k] = { type: 'checkbox', value: e.v, text: e.text };
      else if (e.t === 'd') out[e.k] = { type: 'dropdown', value: e.v, text: e.text, items: e.items };
      else if (e.t === 'fd') out[e.k] = { type: 'folderDropdown', value: e.v, text: e.text, filter: e.filter, folder: e.folder };
      else if (e.t === 'col') out[e.k] = { type: 'color', value: e.v, text: e.text };
      else out[e.k] = { type: 'textbox', value: e.v, text: e.text };
    }
    return JSON.stringify(out, null, 2);
  };
})(window.NS);
