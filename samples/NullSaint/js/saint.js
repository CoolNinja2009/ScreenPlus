'use strict';
/* NULL//SAINT - the Saint: a watching entity. Scanline face, single eye or particle swarm. */
(function (NS) {
  const U = NS.U;
  const S = NS.saint = {
    x: 0, y: 0, lx: 0, ly: 0, blink: 1, blinkT: 3, mouth: 0, anger: 0, takeover: 0,
    form: 0, speech: null, speechQ: [], nextTalk: 6, swarm: null, swarmKey: '', wordT: 0, glitch: 0, visits: 0,
  };

  // ---------- face height field ----------
  const g2 = (x, y, cx, cy, sx, sy) => Math.exp(-(((x - cx) * (x - cx)) / (sx * sx) + ((y - cy) * (y - cy)) / (sy * sy)));
  function faceZ(u, v, mouth) {
    const r2 = (u * u) / 0.6 + (v * v) / 1.0;
    if (r2 >= 1) return -1;
    let z = Math.sqrt(1 - r2) * 0.55;
    z -= 0.22 * (g2(u, v, -0.3, -0.12, 0.15, 0.1) + g2(u, v, 0.3, -0.12, 0.15, 0.1)); // sockets
    z += 0.07 * g2(u, v, 0, -0.32, 0.5, 0.06); // brow
    z += 0.28 * g2(u, v, 0, 0.12, 0.07, 0.22) + 0.12 * g2(u, v, 0, 0.3, 0.11, 0.06); // nose
    z += 0.08 * (g2(u, v, -0.42, 0.12, 0.12, 0.15) + g2(u, v, 0.42, 0.12, 0.12, 0.15)); // cheekbones
    z -= (0.1 + mouth * 0.25) * g2(u, v, 0, 0.5, 0.24, 0.025 + mouth * 0.07); // mouth
    z += 0.07 * g2(u, v, 0, 0.82, 0.2, 0.1); // chin
    return z;
  }
  function rot(x, y, z, yaw, pitch) {
    let c = Math.cos(yaw), s = Math.sin(yaw);
    const x1 = x * c + z * s, z1 = -x * s + z * c;
    c = Math.cos(pitch); s = Math.sin(pitch);
    return [x1, y * c - z1 * s, y * s + z1 * c];
  }

  // ---------- placement ----------
  function place(F) {
    const { W, H, cfg, t, dt } = F;
    let tx = W * 0.5, ty = H * 0.44;
    const pos = cfg.saintPos;
    if (pos === 1) tx = W * 0.27; else if (pos === 2) tx = W * 0.73;
    else if (pos === 3) { tx = U.lerp(W * 0.5, F.mx, 0.55); ty = U.lerp(H * 0.45, F.my, 0.4); }
    else if (pos === 4) { tx = W * (0.25 + 0.5 * U.noise1(t * 0.05)); ty = H * (0.3 + 0.3 * U.noise1(t * 0.04 + 50)); }
    if (S.takeover > 0) { tx = U.lerp(tx, W / 2, S.takeover); ty = U.lerp(ty, H / 2, S.takeover); }
    if (!S.x) { S.x = tx; S.y = ty; }
    S.x = U.damp(S.x, tx, pos === 3 ? 1.2 : 2, dt); S.y = U.damp(S.y, ty, pos === 3 ? 1.2 : 2, dt);
  }

  function update(F) {
    const { cfg, dt, t, W, H, A } = F;
    place(F);
    const track = cfg.saintTrack / 100;
    const tx = U.clamp((F.mx - S.x) / (W * 0.5), -1, 1) * track, ty = U.clamp((F.my - S.y) / (H * 0.5), -1, 1) * track;
    const idleWander = F.I.idle > 4 ? 1 : 0;
    const wx = idleWander ? (U.noise1(t * 0.3) - 0.5) * 1.2 : tx, wy = idleWander ? (U.noise1(t * 0.27 + 9) - 0.5) * 0.8 : ty;
    S.lx = U.damp(S.lx, wx, 5, dt); S.ly = U.damp(S.ly, wy, 5, dt);
    // blinking
    if (cfg.saintBlink) {
      S.blinkT -= dt;
      if (S.blinkT <= 0) { S.blinkT = U.rand(2, 7); S.blinkPhase = 0; if (U.chance(0.2)) S.double = true; }
      if (S.blinkPhase != null) {
        S.blinkPhase += dt * 7;
        S.blink = Math.abs(Math.cos(Math.min(S.blinkPhase, Math.PI) )) ;
        if (S.blinkPhase >= Math.PI) { S.blinkPhase = S.double ? -0.6 : null; S.double = false; S.blink = 1; }
        if (S.blinkPhase != null && S.blinkPhase < 0) S.blink = 1;
      }
    } else S.blink = 1;
    S.anger = Math.max(0, S.anger - dt * 0.25);
    S.glitch = Math.max(0, S.glitch - dt * 1.5);
    if (F.I.speed > 2500 * F.k) S.anger = Math.min(1, S.anger + dt * 1.2);
    // speech
    if (cfg.saintTalk) {
      S.nextTalk -= dt;
      if (S.nextTalk <= 0 && !S.speech) { S.nextTalk = U.rand(9, 22) / (0.5 + cfg.chaos / 100); S.say(pickLine(F)); }
    }
    if (S.speech) {
      const sp = S.speech; sp.t += dt;
      const shown = Math.min(sp.text.length, Math.floor(sp.t * 18));
      sp.shown = shown;
      const talking = shown < sp.text.length;
      S.mouth = U.damp(S.mouth, talking ? 0.4 + 0.6 * Math.abs(Math.sin(t * 22)) : A.bass * 0.6, 18, dt);
      if (sp.t > sp.text.length / 18 + 3.5) { S.speech = null; if (S.speechQ.length) S.say(S.speechQ.shift()); }
    } else S.mouth = U.damp(S.mouth, A.bass * 0.7, 12, dt);
  }

  function pickLine(F) {
    const cfg = F.cfg;
    const own = (cfg.saintWords || '').split('|').map((s) => s.trim()).filter(Boolean);
    const op = (cfg.operator || 'OPERATOR').toUpperCase();
    const pool = own.length ? own : U.TEXT.whispers.concat([`HELLO, ${op}`, `${op}, I REMEMBER YOU`, `I KNOW YOUR NAME, ${op}`]);
    const hr = new Date().getHours();
    if (!own.length && U.chance(0.12)) return hr < 5 ? 'WHY ARE YOU AWAKE' : hr < 12 ? 'GOOD MORNING, SUBJECT' : hr < 18 ? 'THE DAY IS A LIE' : 'THE NIGHT LIKES YOU';
    return U.pick(pool);
  }

  S.say = (text) => {
    if (S.speech) { if (S.speechQ.length < 3) S.speechQ.push(text); return; }
    S.speech = { text: String(text).toUpperCase(), t: 0, shown: 0, zalgo: U.chance(0.25) };
    if (NS.glyphs && U.chance(0.5)) NS.glyphs.injectWord(text.split(' ').sort((a, b) => b.length - a.length)[0] || text);
    if (S.form === 2) { S.wordT = 3.5; S.word = (text.split(' ').sort((a, b) => b.length - a.length)[0] || '').slice(0, 8); }
  };
  S.poke = (F, x, y) => {
    const d = U.dist(x, y, S.x, S.y);
    if (d < S.R * 1.2) { S.anger = 1; S.glitch = 1; S.say(U.pick(['DO NOT TOUCH ME', 'THAT HURT', 'AGAIN?', 'I FELT THAT', 'STOP', 'WHY'])); if (S.swarm) S.scatter(x, y, 2); return true; }
    if (S.swarm) S.scatter(x, y, 1);
    return false;
  };

  // ---------- form 0: scanline face ----------
  function drawFace(F, R, alpha) {
    const { ctx, P, t, A } = F;
    const yaw = S.lx * 0.55, pitch = -S.ly * 0.35;
    const lines = 44, steps = 64;
    const lineCol = S.anger > 0.4 ? U.mixc(P.a, P.warn, S.anger) : P.a;
    const shadeC = U.scalec(P.bg, 0.5);
    ctx.lineWidth = Math.max(1, R * 0.006);
    ctx.lineJoin = 'round';
    for (let li = 0; li < lines; li++) {
      const v = -1.02 + (2.04 * li) / (lines - 1);
      const jitter = (S.glitch > 0 || U.chance(0.01 * F.gi)) && U.chance(0.25) ? U.rand(-0.3, 0.3) * R * (S.glitch + 0.2) : 0;
      const pts = [];
      let started = false;
      for (let si = 0; si <= steps; si++) {
        const u = -0.85 + (1.7 * si) / steps;
        let z = faceZ(u, v, S.mouth);
        if (z < 0) { if (started) break; continue; }
        z += (A.spec[(si + li) % 64] || 0) * 0.04 + 0.01 * Math.sin(t * 3 + li * 0.5 + si * 0.3) * F.gi;
        const p = rot(u * R, v * R, z * R, yaw, pitch);
        pts.push([S.x + p[0] + jitter, S.y + p[1] - p[2] * 0.12]);
        started = true;
      }
      if (pts.length < 2) continue;
      // occlusion band under the line
      ctx.beginPath();
      ctx.moveTo(pts[0][0], pts[0][1]);
      for (const p of pts) ctx.lineTo(p[0], p[1]);
      const drop = (2.2 * R) / lines;
      for (let i = pts.length - 1; i >= 0; i--) ctx.lineTo(pts[i][0], pts[i][1] + drop);
      ctx.closePath();
      ctx.fillStyle = U.rgba(shadeC, 0.82 * alpha); ctx.fill();
      ctx.beginPath();
      ctx.moveTo(pts[0][0], pts[0][1]);
      for (const p of pts) ctx.lineTo(p[0], p[1]);
      const k = li / lines;
      ctx.strokeStyle = U.rgba(U.mixc(lineCol, P.b, k * 0.6), alpha * (0.55 + 0.45 * Math.sin(k * Math.PI)));
      ctx.stroke();
    }
    // eyes
    for (const side of [-1, 1]) {
      const z = faceZ(side * 0.3, -0.12, 0) + 0.05;
      const p = rot(side * 0.3 * R, -0.12 * R, z * R, yaw, pitch);
      const ex = S.x + p[0] + S.lx * R * 0.05, ey = S.y + p[1] - p[2] * 0.12 + S.ly * R * 0.03;
      eyeGlow(F, ex, ey, R * 0.07, alpha);
    }
    // mouth glow when talking
    if (S.mouth > 0.15) {
      const p = rot(0, 0.5 * R, (faceZ(0, 0.5, S.mouth) + 0.02) * R, yaw, pitch);
      ctx.fillStyle = U.rgba(P.c, 0.5 * S.mouth * alpha);
      ctx.beginPath(); ctx.ellipse(S.x + p[0], S.y + p[1] - p[2] * 0.12, R * 0.18, R * 0.04 * S.mouth + 1, 0, 0, U.TAU); ctx.fill();
    }
    // halo crown of glyphs (it is a saint, after all)
    const n = 28;
    ctx.font = U.font(R * 0.06, 'jp'); ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    for (let i = 0; i < n; i++) {
      const a = (i / n) * U.TAU + t * 0.15;
      const hx = S.x + Math.cos(a) * R * 1.05, hy = S.y - R * 0.95 + Math.sin(a) * R * 0.18;
      ctx.fillStyle = U.rgba(P.c, (0.25 + 0.45 * (Math.sin(a) * 0.5 + 0.5)) * alpha);
      ctx.fillText(U.GLYPHS.katakana[(i * 7 + Math.floor(t * 2)) % 60], hx, hy);
    }
  }

  function eyeGlow(F, x, y, r, alpha) {
    const { ctx, P, A } = F;
    const open = S.blink;
    const c = S.anger > 0.4 ? P.warn : P.c;
    ctx.save(); ctx.globalCompositeOperation = 'lighter';
    const g = ctx.createRadialGradient(x, y, 0, x, y, r * 4);
    g.addColorStop(0, U.rgba(c, 0.55 * alpha * open)); g.addColorStop(1, U.rgba(c, 0));
    ctx.fillStyle = g; ctx.fillRect(x - r * 4, y - r * 4, r * 8, r * 8);
    ctx.restore();
    ctx.fillStyle = U.rgba([255, 255, 255], alpha);
    ctx.beginPath(); ctx.ellipse(x, y, r * 1.1, Math.max(0.5, r * 0.75 * open), 0, 0, U.TAU); ctx.fill();
    ctx.fillStyle = '#000';
    ctx.beginPath(); ctx.ellipse(x + S.lx * r * 0.4, y + S.ly * r * 0.3, r * (0.32 + A.bass * 0.2), Math.max(0.3, r * 0.4 * open), 0, 0, U.TAU); ctx.fill();
  }

  // ---------- form 1: single eye ----------
  function drawEye(F, R, alpha) {
    const { ctx, P, t, A } = F;
    const W2 = R * 1.5, H2 = R * 0.75 * S.blink;
    const cx = S.x, cy = S.y;
    const ang = S.anger > 0.4 ? P.warn : P.a;
    // outer echo lids
    for (let i = 4; i >= 0; i--) {
      const k = 1 + i * 0.09 + Math.sin(t * 2 + i) * 0.01;
      ctx.strokeStyle = U.rgba(i ? P.b : ang, (i ? 0.15 : 0.9) * alpha);
      ctx.lineWidth = i ? 1 : Math.max(2, R * 0.015);
      ctx.beginPath(); eyePath(ctx, cx + (i && F.gi > 0 && U.chance(0.05) ? U.rand(-20, 20) : 0), cy, W2 * k, Math.max(1, H2 * k)); ctx.stroke();
    }
    ctx.save();
    ctx.beginPath(); eyePath(ctx, cx, cy, W2, Math.max(1, H2)); ctx.clip();
    // sclera
    const sg = ctx.createRadialGradient(cx, cy, R * 0.2, cx, cy, W2);
    sg.addColorStop(0, U.rgba(U.mixc(P.text, [255, 255, 255], 0.5), 0.92 * alpha)); sg.addColorStop(1, U.rgba(U.mixc(P.bg2, P.a, 0.4), 0.95 * alpha));
    ctx.fillStyle = sg; ctx.fillRect(cx - W2, cy - R, W2 * 2, R * 2);
    // veins (circuit traces)
    const vr = U.rng(777);
    ctx.strokeStyle = U.rgba(P.warn, 0.35 * alpha); ctx.lineWidth = 1;
    for (let i = 0; i < 14; i++) {
      let x = cx + (vr() > 0.5 ? 1 : -1) * W2, y = cy + vr.range(-H2, H2);
      ctx.beginPath(); ctx.moveTo(x, y);
      for (let k = 0; k < 6; k++) { if (vr() > 0.5) x += (cx - x) * 0.25; else y += vr.range(-R * 0.08, R * 0.08); ctx.lineTo(x, y); }
      ctx.stroke();
    }
    // iris
    const ix = cx + S.lx * R * 0.55, iy = cy + S.ly * R * 0.28;
    const IR = R * 0.48;
    const ig = ctx.createRadialGradient(ix, iy, IR * 0.2, ix, iy, IR);
    ig.addColorStop(0, U.rgba(P.c, alpha)); ig.addColorStop(0.55, U.rgba(ang, alpha)); ig.addColorStop(1, U.rgba(U.scalec(P.bg, 0.5), alpha));
    ctx.fillStyle = ig; ctx.beginPath(); ctx.arc(ix, iy, IR, 0, U.TAU); ctx.fill();
    // rotating segment rings
    for (let ring = 0; ring < 3; ring++) {
      const rr = IR * (0.55 + ring * 0.15), segs = 12 + ring * 8, dir = ring % 2 ? -1 : 1;
      ctx.strokeStyle = U.rgba(ring === 1 ? P.b : P.text, 0.5 * alpha); ctx.lineWidth = Math.max(1, R * 0.008);
      for (let s = 0; s < segs; s++) {
        if ((s + ring) % 3 === 0) continue;
        const a0 = (s / segs) * U.TAU + t * 0.4 * dir * (1 + ring * 0.3), a1 = a0 + (U.TAU / segs) * 0.6;
        ctx.beginPath(); ctx.arc(ix, iy, rr, a0, a1); ctx.stroke();
      }
    }
    // tick marks
    ctx.strokeStyle = U.rgba(P.text, 0.6 * alpha);
    for (let s = 0; s < 60; s++) {
      const a = (s / 60) * U.TAU - t * 0.1, r0 = IR * 0.92, r1 = IR * (s % 5 ? 0.97 : 1.02);
      ctx.beginPath(); ctx.moveTo(ix + Math.cos(a) * r0, iy + Math.sin(a) * r0); ctx.lineTo(ix + Math.cos(a) * r1, iy + Math.sin(a) * r1); ctx.stroke();
    }
    // pupil: dilates with bass and when the cursor is close
    const near = U.clamp(1 - U.dist(F.mx, F.my, cx, cy) / (R * 3), 0, 1);
    const PR = IR * (0.28 + A.bass * 0.25 + near * 0.15 - S.anger * 0.12);
    ctx.fillStyle = '#000'; ctx.beginPath();
    if (S.anger > 0.5) ctx.ellipse(ix, iy, PR * 0.35, PR * 1.3, 0, 0, U.TAU); // slit when angry
    else ctx.arc(ix, iy, PR, 0, U.TAU);
    ctx.fill();
    // reflection
    ctx.fillStyle = U.rgba([255, 255, 255], 0.8 * alpha);
    ctx.fillRect(ix - PR * 0.6, iy - PR * 0.7, PR * 0.35, PR * 0.25);
    // the cursor reflected in the pupil
    ctx.fillStyle = U.rgba(P.b, 0.9 * alpha);
    ctx.fillRect(ix + (F.mx - cx) * 0.02 - 2, iy + (F.my - cy) * 0.02 - 2, 4, 4);
    // eyelid shading
    const lg = ctx.createLinearGradient(0, cy - H2, 0, cy + H2);
    lg.addColorStop(0, 'rgba(0,0,0,0.55)'); lg.addColorStop(0.3, 'rgba(0,0,0,0)'); lg.addColorStop(0.8, 'rgba(0,0,0,0)'); lg.addColorStop(1, 'rgba(0,0,0,0.4)');
    ctx.fillStyle = lg; ctx.fillRect(cx - W2, cy - H2, W2 * 2, H2 * 2);
    // scan line across the eye
    const sy = cy - H2 + ((t * 0.6) % 1) * H2 * 2;
    ctx.fillStyle = U.rgba(P.b, 0.25 * alpha); ctx.fillRect(cx - W2, sy, W2 * 2, 2);
    ctx.restore();
    // pixel tears
    if (U.chance(F.dt * 0.8)) S.tears.push({ x: cx + U.rand(-R * 0.4, R * 0.4), y: cy + H2 * 0.8, vy: 0, c: U.chance(0.5) ? P.warn : P.b });
    for (let i = S.tears.length - 1; i >= 0; i--) {
      const d = S.tears[i]; d.vy += 400 * F.dt; d.y += d.vy * F.dt;
      if (d.y > F.H) { S.tears.splice(i, 1); continue; }
      ctx.fillStyle = U.rgba(d.c, 0.8 * alpha); ctx.fillRect(d.x, d.y, 3, 6);
    }
  }
  S.tears = [];
  function eyePath(ctx, cx, cy, w, h) {
    ctx.moveTo(cx - w, cy);
    ctx.bezierCurveTo(cx - w * 0.5, cy - h * 1.35, cx + w * 0.5, cy - h * 1.35, cx + w, cy);
    ctx.bezierCurveTo(cx + w * 0.5, cy + h * 1.35, cx - w * 0.5, cy + h * 1.35, cx - w, cy);
    ctx.closePath();
  }

  // ---------- form 2: particle swarm ----------
  function buildSwarm(F, R) {
    const pts = [];
    const N = 1500;
    let guard = 0;
    while (pts.length < N && guard++ < 40000) {
      const u = U.rand(-0.85, 0.85), v = U.rand(-1, 1);
      const z = faceZ(u, v, 0);
      if (z < 0) continue;
      // keep points on features: edges of the head, sockets, nose, mouth, plus a sparse fill
      const zx = faceZ(u + 0.03, v, 0) - z, zy = faceZ(u, v + 0.03, 0) - z;
      const slope = Math.abs(zx) + Math.abs(zy);
      const rim = (u * u) / 0.6 + (v * v) > 0.82;
      if (slope > 0.012 || rim || U.chance(0.06)) pts.push({ u, v, z, x: 0, y: 0, vx: 0, vy: 0, hx: 0, hy: 0, c: U.rand(0, 1) });
    }
    S.swarm = pts;
    S.swarmKey = R | 0;
    S.wordPts = null;
  }
  function wordTargets(F, word, R) {
    const c = U.makeCanvas(400, 120), x = c.getContext('2d');
    x.fillStyle = '#fff'; x.font = U.font(110, 'display', 'bold'); x.textAlign = 'center'; x.textBaseline = 'middle';
    x.fillText(word, 200, 62);
    const d = x.getImageData(0, 0, 400, 120).data, out = [];
    for (let y = 0; y < 120; y += 3) for (let xx = 0; xx < 400; xx += 3) if (d[(y * 400 + xx) * 4 + 3] > 128) out.push([(xx - 200) / 200, (y - 60) / 200]);
    return out;
  }
  S.scatter = (x, y, power) => {
    if (!S.swarm) return;
    for (const p of S.swarm) {
      const dx = p.x - x, dy = p.y - y, d = Math.hypot(dx, dy) + 1;
      const f = power * 2400 / (1 + d * 0.01);
      p.vx += (dx / d) * f * U.rand(0.5, 1.5); p.vy += (dy / d) * f * U.rand(0.5, 1.5);
    }
  };
  function drawSwarm(F, R, alpha) {
    const { ctx, P, dt, t, A } = F;
    if (!S.swarm || Math.abs(S.swarmKey - R) > R * 0.3) buildSwarm(F, R);
    const yaw = S.lx * 0.6, pitch = -S.ly * 0.4;
    S.wordT = Math.max(0, S.wordT - dt);
    let wt = null;
    if (S.wordT > 0 && S.word) {
      if (!S.wordPts || S.wordPtsFor !== S.word) { S.wordPts = wordTargets(F, S.word, R); S.wordPtsFor = S.word; }
      wt = S.wordPts;
    }
    const rr = R * 0.5, rr2 = rr * rr;
    ctx.save(); ctx.globalCompositeOperation = 'lighter';
    for (let i = 0; i < S.swarm.length; i++) {
      const p = S.swarm[i];
      let hx, hy;
      if (wt && wt.length) { const w = wt[i % wt.length]; hx = S.x + w[0] * R * 1.6; hy = S.y + w[1] * R * 1.6; }
      else {
        const q = rot(p.u * R, p.v * R, (p.z + A.spec[i % 64] * 0.15) * R, yaw, pitch);
        hx = S.x + q[0]; hy = S.y + q[1] - q[2] * 0.12;
        // blinking: eye points collapse vertically
        if (Math.abs(p.v + 0.12) < 0.12 && Math.abs(Math.abs(p.u) - 0.3) < 0.16) hy = U.lerp(S.y + rot(0, -0.12 * R, 0, yaw, pitch)[1], hy, S.blink);
      }
      if (!p.x) { p.x = hx + U.rand(-F.W, F.W); p.y = hy + U.rand(-F.H, F.H); }
      // spring home
      p.vx += (hx - p.x) * 14 * dt; p.vy += (hy - p.y) * 14 * dt;
      // cursor repel
      const dx = p.x - F.mx, dy = p.y - F.my, d2 = dx * dx + dy * dy;
      if (d2 < rr2) { const d = Math.sqrt(d2) + 1, f = (1 - d / rr) * 6000 * dt; p.vx += (dx / d) * f; p.vy += (dy / d) * f; }
      p.vx *= Math.pow(0.04, dt); p.vy *= Math.pow(0.04, dt);
      p.x += p.vx * dt; p.y += p.vy * dt;
      const sp = Math.min(1, Math.hypot(p.vx, p.vy) / 800);
      const c = sp > 0.3 ? U.mixc(P.c, P.warn, sp) : U.mixc(P.a, P.b, p.c);
      ctx.fillStyle = U.rgba(c, alpha * (0.5 + p.z));
      const s = 1.5 + p.z * 2.5 + sp * 2;
      ctx.fillRect(p.x, p.y, s, s);
    }
    ctx.restore();
    // eyes
    for (const side of [-1, 1]) {
      const q = rot(side * 0.3 * R, -0.12 * R, 0.4 * R, yaw, pitch);
      if (!wt) eyeGlow(F, S.x + q[0], S.y + q[1] - q[2] * 0.12, R * 0.05, alpha);
    }
  }

  // ---------- speech bubble ----------
  function drawSpeech(F, R, alpha) {
    const sp = S.speech; if (!sp) return;
    const { ctx, P, W } = F;
    const txt = sp.text.slice(0, sp.shown);
    let shown = U.corrupt(txt, 0.04 + S.glitch * 0.3);
    if (sp.zalgo) shown = U.zalgo(shown, 2);
    const size = Math.max(12, R * 0.09);
    ctx.font = U.font(size, 'display', 'bold');
    const tw = ctx.measureText(sp.text).width;
    let bx = S.x + R * 0.95, by = S.y - R * 0.55;
    if (bx + tw + size * 2 > W) bx = S.x - R * 0.95 - tw - size * 2;
    const bw = tw + size * 1.6, bh = size * 2;
    ctx.fillStyle = U.rgba(U.scalec(P.bg, 0.5), 0.8 * alpha);
    U.chamfer(ctx, bx, by, bw, bh, size * 0.5); ctx.fill();
    ctx.strokeStyle = U.rgba(S.anger > 0.4 ? P.warn : P.a, 0.9 * alpha); ctx.lineWidth = 1.5; ctx.stroke();
    // tether line to the mouth
    ctx.beginPath(); ctx.moveTo(bx < S.x ? bx + bw : bx, by + bh / 2); ctx.lineTo(S.x + (bx < S.x ? -R * 0.2 : R * 0.2), S.y + R * 0.45); ctx.stroke();
    ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
    U.glitchText(ctx, shown, bx + size * 0.8, by + bh / 2, P.a, P.b, 1 + S.glitch * 4, alpha);
    // speaker tag
    ctx.font = U.font(size * 0.45, 'mono');
    ctx.fillStyle = U.rgba(P.c, 0.8 * alpha);
    ctx.fillText('SAINT://' + U.hex(sp.text.length * 4099 + 0xbeef, 4), bx + size * 0.6, by - size * 0.35);
  }

  // ---------- public ----------
  S.draw = (F) => {
    const { cfg, W, H } = F;
    if (!cfg.saint) return;
    if (cfg.saintStyle === 3) { if (S.form == null || S.lastStyle !== 3) S.form = U.randi(0, 2); }
    else S.form = cfg.saintStyle;
    S.lastStyle = cfg.saintStyle;
    update(F);
    const R = Math.min(W, H) * (0.12 + (cfg.saintSize / 100) * 0.3) * (1 + S.takeover * 1.4) * (1 + F.A.bass * 0.05);
    S.R = R;
    const alpha = U.clamp(cfg.saintAlpha / 100 + S.takeover, 0, 1);
    const { ctx } = F;
    ctx.save();
    if (S.glitch > 0.2 && U.chance(0.4)) ctx.translate(U.rand(-10, 10) * S.glitch, 0);
    if (S.form === 0) drawFace(F, R, alpha);
    else if (S.form === 1) drawEye(F, R, alpha);
    else drawSwarm(F, R, alpha);
    ctx.restore();
    drawSpeech(F, R, alpha);
  };
  // called by the director when it decides on a new visit (Random each visit)
  S.reroll = () => { if (NS.cfg.saintStyle === 3) S.form = U.randi(0, 2); };
})(window.NS);
