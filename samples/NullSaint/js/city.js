'use strict';
/* NULL//SAINT - procedural megacity: sky, sky object, parallax skyline, signs, traffic, rain, smog, lightning */
(function (NS) {
  const U = NS.U;
  const City = NS.city = { layers: [], stars: [], cars: [], drops: [], bolts: [], lights: [], key: '' };
  const DEPTHS = [0.18, 0.38, 0.62, 1.0];

  // ---------- build ----------
  City.build = (F) => {
    const { W, H, cfg, P } = F;
    const seed = U.strSeed(cfg.seed || 'NULLSAINT');
    const r = U.rng(seed);
    City.layers = [];
    City.lights = [];
    const margin = Math.round(W * 0.16);
    const dens = cfg.cityDensity / 100, tall = cfg.cityHeight / 100;
    for (let li = 0; li < DEPTHS.length; li++) {
      const depth = DEPTHS[li];
      const cw = W + margin * 2, ch = H;
      const cv = U.makeCanvas(cw, ch), x = cv.getContext('2d');
      const fogK = 1 - depth; // far layers fade into the horizon colour
      const body = U.mixc(U.scalec(P.bg, 0.6 + depth * 0.4), P.bg2, fogK * 0.55);
      const edge = U.mixc(body, P.b, 0.25 + fogK * 0.1);
      const windows = [], signs = [], antennas = [];
      const base = H * (0.98 - (1 - depth) * 0.12);
      let px = -r.range(0, 40);
      while (px < cw) {
        const bw = (r.range(30, 110) + r.range(0, 70) * depth) * (W / 1920) * (1.2 - dens * 0.5);
        const gap = r.range(0, 22) * (1.2 - dens) * (W / 1920);
        const hmax = H * (0.18 + 0.55 * tall) * (0.45 + depth * 0.55);
        let bh = r.range(0.25, 1) * hmax;
        if (r.chance(0.08)) bh *= 1.45; // megatower
        const top = base - bh;
        // body with optional setbacks
        x.fillStyle = U.rgba(body);
        x.fillRect(px, top, bw, base - top + 4);
        let tiers = r.chance(0.45) ? r.int(1, 3) : 0, tw = bw, tx = px, ty = top;
        for (let k = 0; k < tiers; k++) {
          tw *= r.range(0.5, 0.8); tx += (bw - tw) / 2 * r.range(0.6, 1.4) - (bw - tw) / 2 + (bw - tw) / 2; const th = r.range(10, 60) * (0.5 + depth);
          tx = U.clamp(tx, px, px + bw - tw);
          x.fillRect(tx, ty - th, tw, th + 1); ty -= th;
        }
        // spire / antenna
        if (r.chance(0.35)) {
          const ax = tx + tw / 2, ah = r.range(20, 90) * (0.4 + depth);
          x.strokeStyle = U.rgba(edge); x.lineWidth = Math.max(1, depth * 2);
          x.beginPath(); x.moveTo(ax, ty); x.lineTo(ax, ty - ah); x.stroke();
          antennas.push({ x: ax, y: ty - ah, ph: r() * 10, sp: r.range(0.5, 2) });
        }
        // edges
        x.fillStyle = U.rgba(edge, 0.5);
        x.fillRect(px, top, Math.max(1, depth * 2), base - top);
        // neon outline on near buildings
        if (depth > 0.5 && r.chance(0.3)) {
          const nc = r.pick([P.a, P.b, P.c]);
          x.fillStyle = U.rgba(nc, 0.85);
          x.fillRect(px + bw - 2, top, 2, base - top);
          if (r.chance(0.5)) x.fillRect(px, top, bw, 2);
        }
        // windows
        const ws = Math.max(2, (3 + depth * 4) * (W / 1920)), wg = ws * r.range(1.4, 2.4);
        const lit = r.range(0.1, 0.55);
        const tint = r.pick([P.c, P.b, P.a, [255, 230, 190], [190, 230, 255]]);
        const style = r.int(0, 3); // 0 grid, 1 strips, 2 sparse, 3 scanned
        for (let wy = top + wg; wy < base - wg; wy += wg * (style === 1 ? 1.6 : 1)) {
          if (style === 1) { // horizontal light strips
            if (r.chance(lit)) { x.fillStyle = U.rgba(tint, 0.25 + 0.5 * depth); x.fillRect(px + ws, wy, bw - ws * 2, ws * 0.6); }
            continue;
          }
          for (let wx = px + ws; wx < px + bw - ws * 1.5; wx += wg) {
            if (style === 2 && r.chance(0.7)) continue;
            if (r.chance(lit)) {
              const a = (0.35 + r() * 0.55) * (0.4 + depth * 0.6);
              x.fillStyle = U.rgba(r.chance(0.06) ? P.a : tint, a);
              x.fillRect(wx, wy, ws, ws * (style === 3 ? 0.4 : 1));
              if (windows.length < 400 && r.chance(0.08)) windows.push({ x: wx, y: wy, w: ws, h: ws, c: tint, ph: r() * 100 });
            }
          }
        }
        // vertical kanji signs on mid/near buildings
        if (depth > 0.3 && r.chance(0.18 + dens * 0.15) && bh > H * 0.12) {
          const word = r.pick(U.TEXT.signs);
          const sw = Math.max(10, (14 + depth * 22) * (W / 1920));
          const sx = px + (r.chance(0.5) ? -sw * 0.6 : bw - sw * 0.4);
          const sy = top + r.range(10, Math.max(12, bh * 0.4));
          signs.push({ x: sx, y: sy, w: sw, text: word, c: r.pick([P.a, P.b, P.c, P.warn]), ph: r() * 100, broken: r.chance(0.3) });
        }
        px += bw + gap;
      }
      // ground fill
      x.fillStyle = U.rgba(U.scalec(body, 0.8));
      x.fillRect(0, base, cw, H - base);
      City.layers.push({ cv, depth, margin, windows, signs, antennas, base });
    }
    // stars
    City.stars = [];
    for (let i = 0; i < 160; i++) City.stars.push({ x: r() * W, y: r() * H * 0.6, s: r.range(0.5, 2), ph: r() * 10, glitch: r.chance(0.05) });
    // traffic lanes
    City.cars = [];
    City.drops = [];
    City.bolts = [];
    City.key = City.keyOf(F);
  };
  City.keyOf = (F) => [F.W, F.H, F.cfg.seed, F.cfg.cityDensity, F.cfg.cityHeight, F.cfg.theme, F.cfg.colA, F.cfg.colB, F.cfg.colC, F.cfg.colBg, F.cfg.colBg2].join('|');

  // ---------- sky ----------
  function sky(F) {
    const { ctx, W, H, P, t } = F;
    const g = ctx.createLinearGradient(0, 0, 0, H);
    g.addColorStop(0, U.rgba(P.bg));
    g.addColorStop(0.62, U.rgba(U.mixc(P.bg, P.bg2, 0.7)));
    g.addColorStop(0.85, U.rgba(P.bg2));
    g.addColorStop(1, U.rgba(U.scalec(P.bg2, 0.6)));
    ctx.fillStyle = g; ctx.fillRect(0, 0, W, H);
    // aurora-like data bands
    ctx.globalCompositeOperation = 'lighter';
    for (let i = 0; i < 3; i++) {
      const y = H * (0.12 + i * 0.09) + Math.sin(t * 0.2 + i) * 20;
      const gg = ctx.createLinearGradient(0, y - 40, 0, y + 40);
      const c = [P.a, P.b, P.c][i];
      gg.addColorStop(0, U.rgba(c, 0)); gg.addColorStop(0.5, U.rgba(c, 0.035 + F.A.mid * 0.08)); gg.addColorStop(1, U.rgba(c, 0));
      ctx.fillStyle = gg; ctx.fillRect(0, y - 40, W, 80);
    }
    ctx.globalCompositeOperation = 'source-over';
    // stars
    for (const s of City.stars) {
      const tw = 0.4 + 0.6 * Math.abs(Math.sin(t * 1.3 + s.ph));
      ctx.fillStyle = U.rgba(s.glitch ? P.a : P.text, tw * 0.8);
      const ox = s.glitch && Math.sin(t * 7 + s.ph) > 0.9 ? U.rand(-20, 20) : 0;
      ctx.fillRect(s.x + ox - F.par * 0.1, s.y, s.s, s.s);
    }
  }

  function skyObject(F) {
    const { ctx, W, H, P, t, cfg, A } = F;
    const kind = cfg.moon; if (!kind) return;
    const R = Math.min(W, H) * (0.06 + cfg.moonSize / 100 * 0.22) * (1 + A.bass * 0.06);
    const cx = W * 0.72 - F.par * 0.15, cy = H * 0.3 - F.parY * 0.1;
    ctx.save();
    if (kind === 1) { // broken moon: slices displaced
      const slices = 18;
      for (let i = 0; i < slices; i++) {
        const y0 = cy - R + (2 * R * i) / slices, sh = (2 * R) / slices;
        const n = U.noise2(i * 0.7, t * 0.4);
        const dx = (n > 0.72 ? (n - 0.72) * 600 : 0) * (i % 2 ? 1 : -1) * F.gi;
        ctx.save(); ctx.beginPath(); ctx.rect(cx - R * 2, y0, R * 4, sh + 0.5); ctx.clip();
        const g = ctx.createRadialGradient(cx + dx - R * 0.3, cy - R * 0.3, R * 0.1, cx + dx, cy, R);
        g.addColorStop(0, U.rgba(U.mixc(P.text, [255, 255, 255], 0.5), 0.95));
        g.addColorStop(0.7, U.rgba(U.mixc(P.text, P.b, 0.4), 0.8));
        g.addColorStop(1, U.rgba(P.b, 0.25));
        ctx.fillStyle = g; ctx.beginPath(); ctx.arc(cx + dx, cy, R, 0, U.TAU); ctx.fill();
        ctx.restore();
      }
      // craters
      const r = U.rng(42);
      ctx.fillStyle = U.rgba(P.bg, 0.18);
      for (let i = 0; i < 9; i++) { const a = r() * U.TAU, d = r() * R * 0.75; ctx.beginPath(); ctx.arc(cx + Math.cos(a) * d, cy + Math.sin(a) * d, R * r.range(0.04, 0.16), 0, U.TAU); ctx.fill(); }
      // halo
      const hg = ctx.createRadialGradient(cx, cy, R, cx, cy, R * 2.4);
      hg.addColorStop(0, U.rgba(P.b, 0.18)); hg.addColorStop(1, U.rgba(P.b, 0));
      ctx.fillStyle = hg; ctx.fillRect(cx - R * 3, cy - R * 3, R * 6, R * 6);
    } else if (kind === 2) { // striped sun
      const g = ctx.createLinearGradient(0, cy - R, 0, cy + R);
      g.addColorStop(0, U.rgba(P.c)); g.addColorStop(0.55, U.rgba(P.a)); g.addColorStop(1, U.rgba(P.warn));
      ctx.beginPath(); ctx.arc(cx, cy, R, 0, U.TAU); ctx.fillStyle = g; ctx.fill();
      ctx.globalCompositeOperation = 'destination-out';
      for (let i = 0; i < 9; i++) {
        const yy = cy + R * (0.05 + i * 0.11) + ((t * 12) % (R * 0.11));
        ctx.fillRect(cx - R, yy, R * 2, 1 + i * R * 0.012);
      }
      ctx.globalCompositeOperation = 'source-over';
      const hg = ctx.createRadialGradient(cx, cy, R * 0.8, cx, cy, R * 2.2);
      hg.addColorStop(0, U.rgba(P.a, 0.25)); hg.addColorStop(1, U.rgba(P.a, 0));
      ctx.fillStyle = hg; ctx.fillRect(cx - R * 3, cy - R * 3, R * 6, R * 6);
    } else if (kind === 3) { // black hole
      ctx.globalCompositeOperation = 'lighter';
      for (let i = 0; i < 40; i++) {
        const a = t * (0.4 + i * 0.01) + i * 0.6;
        ctx.strokeStyle = U.rgba(U.mixc(P.c, P.a, i / 40), 0.09);
        ctx.lineWidth = R * 0.05;
        ctx.beginPath(); ctx.ellipse(cx, cy, R * (1.3 + i * 0.025), R * (0.32 + i * 0.006), -0.25, a, a + 2.4); ctx.stroke();
      }
      ctx.globalCompositeOperation = 'source-over';
      const hg = ctx.createRadialGradient(cx, cy, R * 0.6, cx, cy, R * 1.3);
      hg.addColorStop(0, 'rgba(0,0,0,1)'); hg.addColorStop(0.75, 'rgba(0,0,0,1)'); hg.addColorStop(1, U.rgba(P.c, 0.6));
      ctx.fillStyle = hg; ctx.beginPath(); ctx.arc(cx, cy, R * 1.0, 0, U.TAU); ctx.fill();
      ctx.strokeStyle = U.rgba(P.c, 0.9); ctx.lineWidth = 2;
      ctx.beginPath(); ctx.arc(cx, cy, R * 0.72, 0, U.TAU); ctx.stroke();
    } else if (kind === 4) { // giant sky eye
      const lx = U.clamp((F.mx - cx) / W, -0.5, 0.5), ly = U.clamp((F.my - cy) / H, -0.5, 0.5);
      const open = F.blink;
      ctx.beginPath(); ctx.ellipse(cx, cy, R * 1.6, R * 0.8 * open, 0, 0, U.TAU);
      ctx.fillStyle = U.rgba(U.mixc(P.text, P.bg2, 0.3), 0.9); ctx.fill();
      ctx.save(); ctx.clip();
      const ix = cx + lx * R * 1.4, iy = cy + ly * R * 0.8;
      const ig = ctx.createRadialGradient(ix, iy, R * 0.1, ix, iy, R * 0.62);
      ig.addColorStop(0, U.rgba(P.c)); ig.addColorStop(0.5, U.rgba(P.a)); ig.addColorStop(1, U.rgba(P.bg));
      ctx.fillStyle = ig; ctx.beginPath(); ctx.arc(ix, iy, R * 0.62, 0, U.TAU); ctx.fill();
      ctx.fillStyle = '#000'; ctx.beginPath(); ctx.arc(ix, iy, R * (0.18 + A.bass * 0.15), 0, U.TAU); ctx.fill();
      ctx.restore();
      ctx.strokeStyle = U.rgba(P.a, 0.8); ctx.lineWidth = 3;
      ctx.beginPath(); ctx.ellipse(cx, cy, R * 1.6, R * 0.8 * open, 0, 0, U.TAU); ctx.stroke();
    } else if (kind === 5) { // ringed planet
      const tilt = -0.35;
      ctx.strokeStyle = U.rgba(P.b, 0.6); ctx.lineWidth = R * 0.08;
      ctx.beginPath(); ctx.ellipse(cx, cy, R * 1.9, R * 0.45, tilt, Math.PI, U.TAU); ctx.stroke();
      const g = ctx.createRadialGradient(cx - R * 0.4, cy - R * 0.4, R * 0.1, cx, cy, R);
      g.addColorStop(0, U.rgba(P.c)); g.addColorStop(1, U.rgba(P.a, 0.9));
      ctx.fillStyle = g; ctx.beginPath(); ctx.arc(cx, cy, R, 0, U.TAU); ctx.fill();
      for (let i = 0; i < 6; i++) { ctx.fillStyle = U.rgba(P.bg, 0.08); ctx.fillRect(cx - R, cy - R + i * R * 0.33 + Math.sin(t + i) * 4, R * 2, R * 0.1); }
      ctx.strokeStyle = U.rgba(P.b, 0.75); ctx.lineWidth = R * 0.08;
      ctx.beginPath(); ctx.ellipse(cx, cy, R * 1.9, R * 0.45, tilt, 0, Math.PI); ctx.stroke();
    }
    ctx.restore();
  }

  // ---------- searchlights ----------
  function searchlights(F) {
    const { ctx, W, H, P, t } = F;
    ctx.save(); ctx.globalCompositeOperation = 'lighter';
    for (let i = 0; i < 4; i++) {
      const bx = W * (0.12 + i * 0.26), by = H * 0.85;
      const ang = -Math.PI / 2 + Math.sin(t * (0.21 + i * 0.07) + i * 2) * 0.55;
      const len = H * 1.2, spread = 0.05;
      const g = ctx.createLinearGradient(bx, by, bx + Math.cos(ang) * len, by + Math.sin(ang) * len);
      const c = i % 2 ? P.b : P.a;
      g.addColorStop(0, U.rgba(c, 0.16)); g.addColorStop(1, U.rgba(c, 0));
      ctx.fillStyle = g;
      ctx.beginPath(); ctx.moveTo(bx, by);
      ctx.lineTo(bx + Math.cos(ang - spread) * len, by + Math.sin(ang - spread) * len);
      ctx.lineTo(bx + Math.cos(ang + spread) * len, by + Math.sin(ang + spread) * len);
      ctx.closePath(); ctx.fill();
    }
    ctx.restore();
  }

  // ---------- traffic ----------
  function traffic(F, depthMin, depthMax) {
    const { ctx, W, H, P, dt, cfg } = F;
    const want = Math.round(cfg.cars * 0.6);
    while (City.cars.length < want) {
      const d = U.rand(0.15, 1);
      const dir = U.chance(0.5) ? 1 : -1;
      City.cars.push({
        d, dir, x: U.rand(0, W), y: H * U.rand(0.2, 0.72), sp: U.rand(60, 240) * (0.3 + d),
        c: U.chance(0.5) ? P.warn : U.pick([P.a, P.b, P.c, [255, 255, 255]]), bob: U.rand(0, 10), len: U.rand(20, 90) * d,
      });
    }
    if (City.cars.length > want) City.cars.length = want;
    ctx.save(); ctx.globalCompositeOperation = 'lighter';
    for (const c of City.cars) {
      if (c.d < depthMin || c.d >= depthMax) continue;
      c.x += c.dir * c.sp * dt * F.speed;
      if (c.x > W + 120) c.x = -120; if (c.x < -120) c.x = W + 120;
      const y = c.y + Math.sin(F.t * 0.8 + c.bob) * 6 * c.d - F.parY * c.d * 0.3;
      const x = c.x - F.par * c.d;
      const s = 1.5 + c.d * 3;
      const g = ctx.createLinearGradient(x, y, x - c.dir * c.len, y);
      g.addColorStop(0, U.rgba(c.c, 0.9)); g.addColorStop(1, U.rgba(c.c, 0));
      ctx.fillStyle = g; ctx.fillRect(Math.min(x, x - c.dir * c.len), y - s * 0.3, c.len, s * 0.6);
      ctx.fillStyle = U.rgba([255, 255, 255], 0.95); ctx.fillRect(x - s / 2, y - s / 2, s, s);
    }
    ctx.restore();
  }

  // ---------- layer drawing ----------
  function drawLayer(F, L) {
    const { ctx, W, H, P, t } = F;
    const ox = -L.margin - F.par * L.depth, oy = -F.parY * L.depth * 0.4;
    ctx.drawImage(L.cv, ox, oy);
    // flicker windows
    for (const w of L.windows) {
      const n = U.noise1(t * 0.7 + w.ph);
      if (n > 0.62) { ctx.fillStyle = U.rgba(w.c, (n - 0.62) * 2.2 * (0.5 + L.depth * 0.5)); ctx.fillRect(w.x + ox, w.y + oy, w.w, w.h); }
      else if (n < 0.12) { ctx.fillStyle = U.rgba(P.bg, 0.9); ctx.fillRect(w.x + ox, w.y + oy, w.w, w.h); }
    }
    // antenna beacons
    for (const a of L.antennas) {
      const on = Math.sin(t * a.sp * 3 + a.ph) > 0.6;
      if (on) { ctx.fillStyle = U.rgba(P.warn, 0.95); ctx.fillRect(a.x + ox - 1.5, a.y + oy - 1.5, 3, 3); }
    }
    // neon kanji signs
    if (F.cfg.signs) for (const s of L.signs) {
      const flick = s.broken ? (U.noise1(t * 6 + s.ph) > 0.35 ? 1 : 0.15) : 0.85 + 0.15 * Math.sin(t * 30 + s.ph);
      const n = s.text.length, sh = s.w * 1.1 * n + s.w * 0.4;
      const x = s.x + ox, y = s.y + oy;
      ctx.fillStyle = U.rgba(U.scalec(s.c, 0.12), 0.85); ctx.fillRect(x, y, s.w, sh);
      ctx.strokeStyle = U.rgba(s.c, 0.7 * flick); ctx.lineWidth = 1.5; ctx.strokeRect(x, y, s.w, sh);
      ctx.fillStyle = U.rgba(s.c, flick);
      ctx.font = U.font(s.w * 0.85, 'jp', 'bold'); ctx.textAlign = 'center'; ctx.textBaseline = 'top';
      for (let i = 0; i < n; i++) ctx.fillText(s.text[i], x + s.w / 2, y + s.w * 0.25 + i * s.w * 1.1);
      // glow halo (bloom does the rest)
      ctx.fillStyle = U.rgba(s.c, 0.06 * flick); ctx.fillRect(x - s.w * 0.6, y - s.w * 0.3, s.w * 2.2, sh + s.w * 0.6);
    }
  }

  function fog(F, y, strength, c) {
    const { ctx, W, H, t } = F;
    if (strength <= 0) return;
    const g = ctx.createLinearGradient(0, y - H * 0.15, 0, y + H * 0.1);
    g.addColorStop(0, U.rgba(c, 0)); g.addColorStop(0.6, U.rgba(c, 0.35 * strength)); g.addColorStop(1, U.rgba(c, 0.1 * strength));
    ctx.fillStyle = g; ctx.fillRect(0, y - H * 0.15, W, H * 0.25);
    // drifting smog blobs
    for (let i = 0; i < 5; i++) {
      const bx = U.wrap(i * W * 0.3 + t * 12 * (1 + i * 0.2), -W * 0.3, W * 1.3);
      const rg = ctx.createRadialGradient(bx, y, 0, bx, y, W * 0.2);
      rg.addColorStop(0, U.rgba(c, 0.12 * strength)); rg.addColorStop(1, U.rgba(c, 0));
      ctx.fillStyle = rg; ctx.fillRect(bx - W * 0.2, y - W * 0.2, W * 0.4, W * 0.4);
    }
  }

  // ---------- rain ----------
  function rain(F) {
    const { ctx, W, H, P, dt, cfg } = F;
    const want = Math.round(cfg.weather * 6 * (W / 1920));
    while (City.drops.length < want) City.drops.push({ x: U.rand(-W * 0.2, W * 1.1), y: U.rand(-H, H), d: U.rand(0.3, 1), l: U.rand(8, 26) });
    if (City.drops.length > want) City.drops.length = want;
    const wind = 0.25 + F.I.vx * 0.0004;
    ctx.strokeStyle = U.rgba(U.mixc(P.text, P.b, 0.4), 0.35);
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (const d of City.drops) {
      const sp = (700 + 700 * d.d) * dt * F.speed;
      d.y += sp; d.x += sp * wind;
      if (d.y > H) { d.y = U.rand(-80, -10); d.x = U.rand(-W * 0.2, W * 1.05); if (U.chance(0.15)) City.splash(F, d.x, H * U.rand(0.86, 0.99)); }
      ctx.moveTo(d.x, d.y); ctx.lineTo(d.x - d.l * wind * d.d, d.y - d.l * d.d);
    }
    ctx.stroke();
  }
  City.splashes = [];
  City.splash = (F, x, y) => { if (City.splashes.length < 60) City.splashes.push({ x, y, t: 0 }); };
  function splashes(F) {
    const { ctx, P, dt } = F;
    ctx.strokeStyle = U.rgba(P.b, 0.4); ctx.lineWidth = 1;
    for (let i = City.splashes.length - 1; i >= 0; i--) {
      const s = City.splashes[i]; s.t += dt * 3;
      if (s.t > 1) { City.splashes.splice(i, 1); continue; }
      ctx.globalAlpha = 1 - s.t;
      ctx.beginPath(); ctx.ellipse(s.x, s.y, 2 + s.t * 10, 1 + s.t * 2, 0, 0, U.TAU); ctx.stroke();
    }
    ctx.globalAlpha = 1;
  }

  // ---------- data lightning ----------
  City.strike = (F) => {
    const { W, H } = F;
    const pts = []; let x = U.rand(W * 0.1, W * 0.9), y = 0;
    const tx = x + U.rand(-W * 0.15, W * 0.15), ty = H * U.rand(0.45, 0.75);
    const n = 14;
    for (let i = 0; i <= n; i++) { const k = i / n; pts.push([U.lerp(x, tx, k) + (i && i < n ? U.rand(-40, 40) : 0), U.lerp(y, ty, k)]); }
    City.bolts.push({ pts, t: 0, branch: U.randi(0, n - 3) });
    F.flash(0.25, F.P.b);
  };
  function bolts(F) {
    const { ctx, P, dt } = F;
    for (let i = City.bolts.length - 1; i >= 0; i--) {
      const b = City.bolts[i]; b.t += dt * 2.5;
      if (b.t > 1) { City.bolts.splice(i, 1); continue; }
      const a = (1 - b.t) * (Math.random() > 0.3 ? 1 : 0.2);
      ctx.save(); ctx.globalCompositeOperation = 'lighter';
      for (const [w, c] of [[8, P.b], [3, P.text], [1, [255, 255, 255]]]) {
        ctx.strokeStyle = U.rgba(c, a * (w > 4 ? 0.25 : 0.9)); ctx.lineWidth = w;
        ctx.beginPath(); b.pts.forEach((p, k) => (k ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1]))); ctx.stroke();
      }
      // hex fragments along the bolt
      ctx.fillStyle = U.rgba(P.c, a); ctx.font = U.font(11);
      for (let k = 0; k < b.pts.length; k += 3) ctx.fillText(U.hex(U.randi(0, 65535)), b.pts[k][0] + 6, b.pts[k][1]);
      ctx.restore();
    }
  }

  // ---------- synth grid ----------
  function grid(F) {
    const { ctx, W, H, P, t } = F;
    const hy = H * 0.8;
    ctx.save(); ctx.beginPath(); ctx.rect(0, hy, W, H - hy); ctx.clip();
    ctx.fillStyle = U.rgba(U.scalec(P.bg, 0.6), 0.9); ctx.fillRect(0, hy, W, H - hy);
    ctx.strokeStyle = U.rgba(P.a, 0.6); ctx.lineWidth = 1.5;
    const sp = (t * 0.6) % 1;
    for (let i = 0; i < 18; i++) {
      const k = (i + sp) / 18, y = hy + (H - hy) * k * k;
      ctx.globalAlpha = k; ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
    }
    ctx.globalAlpha = 1;
    const vx = W / 2 - F.par * 0.5;
    for (let i = -20; i <= 20; i++) { ctx.beginPath(); ctx.moveTo(vx + i * 12, hy); ctx.lineTo(vx + i * W * 0.12, H); ctx.stroke(); }
    ctx.restore();
  }

  // ---------- public draw ----------
  City.draw = (F) => {
    const { cfg, P, H, A } = F;
    if (City.keyOf(F) !== City.key) City.build(F);
    sky(F);
    const replaced = NS.photo && NS.photo.drawBack(F);
    if (!replaced) skyObject(F);
    if (cfg.searchlights) searchlights(F);
    if (!cfg.city || replaced) { if (cfg.weather) rain(F); return; }
    const fogC = U.mixc(P.bg2, P.a, 0.15);
    const fogK = cfg.fog / 100;
    drawLayer(F, City.layers[0]);
    traffic(F, 0, 0.35);
    fog(F, City.layers[0].base - H * 0.05, fogK * 0.9, fogC);
    drawLayer(F, City.layers[1]);
    traffic(F, 0.35, 0.6);
    fog(F, City.layers[1].base - H * 0.04, fogK * 0.7, fogC);
    if (F.hook && F.hook.mid) F.hook.mid(F); // billboards sit among the mid layers
    drawLayer(F, City.layers[2]);
    traffic(F, 0.6, 0.85);
    fog(F, City.layers[2].base, fogK * 0.5, fogC);
    if (cfg.lightning && (U.chance(F.dt * 0.04 * (0.3 + cfg.chaos / 50)) || (A.kick && A.bass > 0.7 && U.chance(0.25)))) City.strike(F);
    bolts(F);
    drawLayer(F, City.layers[3]);
    traffic(F, 0.85, 1.01);
    if (cfg.grid) grid(F);
    fog(F, H * 0.98, fogK * 0.6, fogC);
    if (cfg.weather) { rain(F); splashes(F); }
  };
})(window.NS);
