'use strict';
/* NULL//SAINT - particles, shockwaves, cursor trail, reality tears, corruption worms, glitch confetti */
(function (NS) {
  const U = NS.U;
  const X = NS.fx = { parts: [], shocks: [], tears: [], worms: [], blocks: [], dead: [], ghosts: [] };
  const MAX_PARTS = 900;

  X.particle = (o) => { if (X.parts.length < MAX_PARTS) X.parts.push(Object.assign({ x: 0, y: 0, vx: 0, vy: 0, life: 1, t: 0, s: 2, c: [255, 255, 255], g: 0, drag: 1, ch: null, kind: 0 }, o)); };

  X.shock = (x, y, strength) => { X.shocks.push({ x, y, t: 0, s: strength }); if (X.shocks.length > 6) X.shocks.shift(); };

  X.burst = (F, x, y, power) => {
    const { P } = F;
    const n = Math.round(60 + 140 * power);
    for (let i = 0; i < n; i++) {
      const a = U.rand(0, U.TAU), sp = U.rand(80, 900) * power * F.k;
      X.particle({
        x, y, vx: Math.cos(a) * sp, vy: Math.sin(a) * sp, life: U.rand(0.6, 2), s: U.rand(2, 9) * F.k, g: 300 * F.k, drag: 0.8,
        c: U.pick([P.a, P.b, P.c, P.warn, P.text]), ch: U.chance(0.45) ? U.pick(U.GLYPHS.hex + U.GLYPHS.box) : null, kind: U.chance(0.2) ? 1 : 0,
      });
    }
  };

  X.tear = (F, x, y, power) => {
    // a jagged crack in reality that leaks light, then heals
    const pts = []; const len = (200 + 500 * power) * F.k; let a = U.rand(0, U.TAU);
    let px = x - Math.cos(a) * len / 2, py = y - Math.sin(a) * len / 2;
    const n = 22;
    for (let i = 0; i <= n; i++) {
      pts.push([px, py, U.rand(0.3, 1)]);
      a += U.rand(-0.6, 0.6) * 0.5;
      px += Math.cos(a) * len / n; py += Math.sin(a) * len / n;
    }
    X.tears.push({ pts, t: 0, life: 3 + power * 3, w: (6 + 20 * power) * F.k });
  };

  X.worm = (F) => {
    const { W, H } = F;
    const side = U.randi(0, 3);
    const x = side === 0 ? 0 : side === 1 ? W : U.rand(0, W), y = side === 2 ? 0 : side === 3 ? H : U.rand(0, H);
    X.worms.push({ seg: [[x, y]], a: Math.atan2(H / 2 - y, W / 2 - x), t: 0, life: U.rand(5, 9), size: U.rand(10, 22) * F.k, sp: U.rand(180, 340) * F.k });
  };

  X.click = (F, x, y) => {
    const cfg = F.cfg;
    const power = 0.3 + (cfg.clickPower / 100) * 1.0;
    let mode = cfg.click;
    if (mode === 0) return;
    if (mode === 4) mode = U.randi(1, 3);
    const all = mode === 5;
    if (all || mode === 1) { X.shock(x, y, power); X.ring(F, x, y, power); }
    if (all || mode === 2) { X.burst(F, x, y, power); }
    if (all || mode === 3) { X.tear(F, x, y, power); X.shock(x, y, power * 0.5); }
    if (NS.glyphs) NS.glyphs.burst(x, y, power);
    if (!cfg.safe) F.flash(0.15 * power, F.P.text);
    F.kick(0.6 * power);
  };
  X.rings = [];
  X.ring = (F, x, y, power) => X.rings.push({ x, y, t: 0, p: power });

  // ---------- update + draw ----------
  X.draw = (F) => {
    const { ctx, dt, P, W, H, t, cfg } = F;

    // ambient glitch confetti blocks
    if (U.chance(dt * 4 * F.gi * (0.3 + cfg.chaos / 100))) {
      X.blocks.push({ x: U.rand(0, W), y: U.rand(0, H), w: U.rand(10, 220) * F.k, h: U.rand(2, 30) * F.k, t: 0, life: U.rand(0.05, 0.4), c: U.pick([P.a, P.b, P.c, P.warn, [255, 255, 255], [0, 0, 0]]) });
    }
    for (let i = X.blocks.length - 1; i >= 0; i--) {
      const b = X.blocks[i]; b.t += dt;
      if (b.t > b.life) { X.blocks.splice(i, 1); continue; }
      ctx.fillStyle = U.rgba(b.c, 0.85); ctx.fillRect(b.x, b.y, b.w, b.h);
    }

    // dead / stuck pixels that slowly accumulate while idle
    if (F.rot > 0.2 && X.dead.length < 400 && U.chance(dt * 20 * F.rot)) X.dead.push({ x: U.rand(0, W), y: U.rand(0, H), c: U.pick([P.a, P.b, P.c, [255, 255, 255]]), s: U.randi(1, 4) * F.k });
    if (F.rot < 0.05 && X.dead.length) X.dead.length = Math.max(0, X.dead.length - 20);
    for (const d of X.dead) { ctx.fillStyle = U.rgba(d.c, 1); ctx.fillRect(d.x, d.y, d.s, d.s); }

    // cursor trail: pixel squares shed along fast motion
    const tr = cfg.trail / 100;
    if (tr > 0 && F.I.speed > 200 * F.k && F.I.inside) {
      const n = Math.min(6, Math.floor(F.I.speed / (400 * F.k)) + 1);
      for (let i = 0; i < n; i++) {
        X.particle({ x: F.mx + U.rand(-8, 8) * F.k, y: F.my + U.rand(-8, 8) * F.k, vx: -F.I.vx * 0.05 * F.k + U.rand(-30, 30), vy: -F.I.vy * 0.05 * F.k + U.rand(-30, 30), life: U.rand(0.3, 0.9) * (0.4 + tr), s: U.rand(2, 7) * F.k, c: U.pick([P.a, P.b, P.c]), drag: 0.5, kind: 2 });
      }
    }

    // particles
    ctx.save(); ctx.globalCompositeOperation = 'lighter';
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    for (let i = X.parts.length - 1; i >= 0; i--) {
      const p = X.parts[i]; p.t += dt;
      if (p.t > p.life) { X.parts.splice(i, 1); continue; }
      p.vx *= Math.pow(1 - p.drag * 0.9, dt * 2); p.vy *= Math.pow(1 - p.drag * 0.9, dt * 2);
      p.vy += p.g * dt;
      p.x += p.vx * dt; p.y += p.vy * dt;
      const a = 1 - p.t / p.life;
      if (p.ch) { ctx.font = U.font(p.s * 2.4, 'mono', 'bold'); ctx.fillStyle = U.rgba(p.c, a); ctx.fillText(p.ch, p.x, p.y); }
      else if (p.kind === 1) { ctx.strokeStyle = U.rgba(p.c, a); ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(p.x, p.y); ctx.lineTo(p.x - p.vx * 0.05, p.y - p.vy * 0.05); ctx.stroke(); }
      else { ctx.fillStyle = U.rgba(p.c, a); ctx.fillRect(p.x - p.s / 2, p.y - p.s / 2, p.s, p.s); }
    }
    ctx.restore();

    // expanding hex rings
    for (let i = X.rings.length - 1; i >= 0; i--) {
      const r = X.rings[i]; r.t += dt;
      if (r.t > 1.2) { X.rings.splice(i, 1); continue; }
      const rad = U.easeOutCubic(r.t / 1.2) * 400 * F.k * (0.5 + r.p);
      ctx.strokeStyle = U.rgba(P.b, (1 - r.t / 1.2) * 0.9); ctx.lineWidth = 2 * F.k;
      ctx.beginPath();
      for (let k = 0; k <= 6; k++) { const a = (k / 6) * U.TAU + r.t; const px = r.x + Math.cos(a) * rad, py = r.y + Math.sin(a) * rad; k ? ctx.lineTo(px, py) : ctx.moveTo(px, py); }
      ctx.stroke();
      ctx.strokeStyle = U.rgba(P.a, (1 - r.t / 1.2) * 0.6);
      ctx.beginPath(); ctx.arc(r.x, r.y, rad * 0.7, 0, U.TAU); ctx.stroke();
      ctx.font = U.font(12 * F.k); ctx.fillStyle = U.rgba(P.text, 1 - r.t / 1.2);
      ctx.fillText('IMPACT ' + U.hex((r.x * 31 + r.y) | 0, 4), r.x + rad * 0.72, r.y);
    }

    // reality tears
    for (let i = X.tears.length - 1; i >= 0; i--) {
      const tr2 = X.tears[i]; tr2.t += dt;
      if (tr2.t > tr2.life) { X.tears.splice(i, 1); continue; }
      const open = Math.min(1, tr2.t * 6) * (1 - U.smooth(tr2.life * 0.6, tr2.life, tr2.t));
      const pts = tr2.pts;
      // void inside the tear
      ctx.beginPath();
      for (let k = 0; k < pts.length; k++) { const p = pts[k]; const nx = 0, w = tr2.w * open * p[2] * Math.sin((k / (pts.length - 1)) * Math.PI); k ? ctx.lineTo(p[0] + nx, p[1] - w) : ctx.moveTo(p[0], p[1]); }
      for (let k = pts.length - 1; k >= 0; k--) { const p = pts[k]; const w = tr2.w * open * p[2] * Math.sin((k / (pts.length - 1)) * Math.PI); ctx.lineTo(p[0], p[1] + w); }
      ctx.closePath();
      ctx.fillStyle = '#000'; ctx.fill();
      ctx.save(); ctx.clip();
      // something on the other side: static and an eye of light
      for (let k = 0; k < 40; k++) { ctx.fillStyle = U.rgba(U.pick([P.a, P.b, P.text]), U.rand(0.2, 0.8)); const p = U.pick(pts); ctx.fillRect(p[0] + U.rand(-30, 30), p[1] + U.rand(-tr2.w, tr2.w), U.rand(2, 20), 2); }
      ctx.restore();
      ctx.save(); ctx.globalCompositeOperation = 'lighter';
      ctx.strokeStyle = U.rgba(P.c, 0.9 * open); ctx.lineWidth = 2 * F.k; ctx.shadowBlur = 0;
      ctx.stroke();
      ctx.restore();
      if (U.chance(dt * 30 * open)) { const p = U.pick(pts); X.particle({ x: p[0], y: p[1], vx: U.rand(-60, 60), vy: U.rand(-60, 60), life: 0.8, s: 3 * F.k, c: P.c, drag: 0.4 }); }
    }

    // worms: pixel snakes that eat the screen
    for (let i = X.worms.length - 1; i >= 0; i--) {
      const w = X.worms[i]; w.t += dt;
      if (w.t > w.life) { X.worms.splice(i, 1); continue; }
      const head = w.seg[w.seg.length - 1];
      // steer towards the cursor, wobble
      const ta = Math.atan2(F.my - head[1], F.mx - head[0]);
      let da = ta - w.a; while (da > Math.PI) da -= U.TAU; while (da < -Math.PI) da += U.TAU;
      w.a += da * dt * 0.8 + Math.sin(t * 5 + i) * dt * 2;
      const nx = head[0] + Math.cos(w.a) * w.sp * dt, ny = head[1] + Math.sin(w.a) * w.sp * dt;
      if (U.dist(nx, ny, head[0], head[1]) > 0) w.seg.push([nx, ny]);
      if (w.seg.length > 90) w.seg.shift();
      for (let k = 0; k < w.seg.length; k += 2) {
        const s = w.seg[k], kk = k / w.seg.length;
        const sz = w.size * (0.3 + kk);
        ctx.fillStyle = U.rgba(kk > 0.95 ? P.warn : U.mixc(P.a, P.c, (k * 0.13) % 1), 0.85 * kk);
        ctx.fillRect(Math.round(s[0] / sz) * sz, Math.round(s[1] / sz) * sz, sz, sz);
      }
      // the head mutters
      ctx.font = U.font(12 * F.k, 'mono', 'bold'); ctx.fillStyle = U.rgba(P.warn, 1);
      ctx.fillText(U.corrupt('EAT', 0.3), nx + 14 * F.k, ny - 14 * F.k);
    }

    // shockwave timers (drawn by the shader)
    for (let i = X.shocks.length - 1; i >= 0; i--) { X.shocks[i].t += dt; if (X.shocks[i].t > 1.6) X.shocks.splice(i, 1); }
  };

  // shader uniform array: 6 x vec4 (u, v, age, strength)
  const SH = new Float32Array(24);
  X.shockUniform = (F) => {
    SH.fill(0);
    X.shocks.forEach((s, i) => { SH[i * 4] = s.x / F.W; SH[i * 4 + 1] = 1 - s.y / F.H; SH[i * 4 + 2] = s.t; SH[i * 4 + 3] = s.s; });
    return SH;
  };
})(window.NS);
