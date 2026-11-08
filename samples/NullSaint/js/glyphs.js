'use strict';
/* NULL//SAINT - glyph rain: columns that bend around the cursor, corrupt, reverse and spell words */
(function (NS) {
  const U = NS.U;
  const G = NS.glyphs = { cols: [], key: '', words: [] };

  function charset(cfg) {
    const name = U.GLYPH_ORDER[cfg.glyphSet] || 'katakana';
    if (name === 'custom') return (cfg.glyphCustom || '01').replace(/\s/g, '') || '01';
    return U.GLYPHS[name];
  }

  G.build = (F) => {
    const { W, H, cfg } = F;
    const size = Math.max(6, cfg.glyphSize * F.k);
    const density = cfg.glyphDensity / 100;
    const n = Math.floor((W / size) * density);
    G.set = charset(cfg);
    G.size = size;
    G.cols = [];
    for (let i = 0; i < n; i++) G.cols.push(newCol(F, U.rand(0, W), true));
    G.key = [W, H, cfg.glyphSize, cfg.glyphDensity, cfg.glyphSet, cfg.glyphCustom, F.k].join('|');
  };

  function newCol(F, x, scatter) {
    const len = U.randi(6, 28);
    const chars = []; for (let i = 0; i < len; i++) chars.push(U.pick(G.set));
    const depth = U.rand(0.35, 1);
    return {
      x: Math.round(x / G.size) * G.size, y: scatter ? U.rand(-F.H, F.H) : U.rand(-F.H * 0.5, -40),
      sp: U.rand(0.5, 1.6) * depth, len, chars, depth,
      bend: 0, reverse: U.chance(0.03), corrupt: 0, word: null, hue: U.rand(0, 1),
    };
  }

  G.injectWord = (word) => {
    // a column near the middle will spell the word vertically
    if (!G.cols.length) return;
    const c = U.pick(G.cols);
    c.word = word.toUpperCase().slice(0, 24);
    c.len = Math.max(c.len, c.word.length + 2);
  };

  G.draw = (F) => {
    const { ctx, W, H, P, cfg, dt, t, A } = F;
    if (!cfg.glyphs || cfg.glyphDensity <= 0) return;
    const key = [W, H, cfg.glyphSize, cfg.glyphDensity, cfg.glyphSet, cfg.glyphCustom, F.k].join('|');
    if (key !== G.key) G.build(F);
    const size = G.size;
    const speed = (cfg.glyphSpeed / 50) * 160 * F.speed * (1 + A.bass * 0.8);
    const repel = (cfg.glyphRepel / 100) * size * 9;
    const rr = size * 10;
    ctx.font = U.font(size, U.GLYPH_ORDER[cfg.glyphSet] === 'katakana' || U.GLYPH_ORDER[cfg.glyphSet] === 'hangul' ? 'jp' : 'mono');
    ctx.textAlign = 'center'; ctx.textBaseline = 'top';
    const mode = cfg.glyphColor;
    const base = mode === 0 ? P.a : mode === 1 ? P.b : P.c;
    for (let ci = 0; ci < G.cols.length; ci++) {
      const c = G.cols[ci];
      const dir = c.reverse ? -1 : 1;
      c.y += dir * speed * c.sp * dt;
      if ((dir > 0 && c.y - c.len * size > H) || (dir < 0 && c.y < -size * 2)) {
        Object.assign(c, newCol(F, U.rand(0, W), false));
        if (c.reverse) c.y = H + c.len * size;
        continue;
      }
      // random mutation
      if (U.chance(dt * 6)) c.chars[U.randi(0, c.len - 1)] = U.pick(G.set);
      if (U.chance(dt * 0.05 * F.gi)) c.corrupt = 1;
      c.corrupt = Math.max(0, c.corrupt - dt * 0.7);
      const alphaD = 0.25 + c.depth * 0.75;
      for (let i = 0; i < c.len; i++) {
        const gy = c.y - i * size * dir;
        if (gy < -size || gy > H + size) continue;
        // bend around the cursor
        let gx = c.x;
        const dx = gx - F.mx, dy = gy - F.my, d2 = dx * dx + dy * dy;
        if (repel > 0 && d2 < rr * rr) {
          const d = Math.sqrt(d2) + 1, f = (1 - d / rr);
          gx += (dx / d) * repel * f * f;
        }
        const k = i / c.len;
        let a = (1 - k) * alphaD;
        let col;
        if (i === 0) col = [255, 255, 255];
        else if (mode === 3) col = U.hsl(c.hue + k * 0.3 + t * 0.05, 1, 0.6);
        else if (mode === 4) col = U.mixc(base, P.warn, U.clamp(A.spec[(i * 2) % 64] * 2, 0, 1));
        else col = base;
        if (c.corrupt > 0 && U.chance(0.3)) col = P.warn;
        ctx.fillStyle = U.rgba(col, a);
        let ch = c.chars[i % c.chars.length];
        if (c.word && i < c.word.length) { ch = c.word[i]; ctx.fillStyle = U.rgba(P.c, 0.95); }
        if (c.corrupt > 0.5 && U.chance(0.2)) ch = U.pick(U.noiseChars);
        const jx = c.corrupt > 0 ? U.rand(-3, 3) * c.corrupt : 0;
        ctx.fillText(ch, gx + jx, gy);
      }
      if (c.word && c.y - c.len * size > H) c.word = null;
    }
  };

  // glitch burst: every column near x jumps
  G.burst = (x, y, power) => {
    for (const c of G.cols) {
      if (Math.abs(c.x - x) < 200 * power) { c.corrupt = 1; c.y += U.rand(-60, 60) * power; }
    }
  };
})(window.NS);
