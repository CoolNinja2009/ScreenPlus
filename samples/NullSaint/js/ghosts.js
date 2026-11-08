'use strict';
/* NULL//SAINT - phantom cursors: the wallpaper records your mouse and, later, replays you back at yourself */
(function (NS) {
  const U = NS.U;
  const Gh = NS.ghosts = { rec: [], clicks: [], recT: 0, list: [], next: 25 };
  const RATE = 1 / 20, KEEP = 90; // 20 samples/s, 90 s of memory

  Gh.noteClick = (x, y, t) => Gh.clicks.push({ x, y, t });

  function record(F) {
    Gh.recT -= F.dt / Math.max(0.1, F.speed);
    if (Gh.recT > 0) return;
    Gh.recT = RATE;
    const I = NS.input;
    Gh.rec.push({ x: I.x / F.cssW, y: I.y / F.cssH, t: F.t });
    while (Gh.rec.length && F.t - Gh.rec[0].t > KEEP) Gh.rec.shift();
    while (Gh.clicks.length && F.t - Gh.clicks[0].t > KEEP) Gh.clicks.shift();
  }

  function spawn(F) {
    if (Gh.rec.length < 120) return;
    // pick a stretch where you actually moved
    for (let tries = 0; tries < 8; tries++) {
      const len = U.randi(80, 200);
      const start = U.randi(0, Math.max(0, Gh.rec.length - len - 40));
      const seg = Gh.rec.slice(start, start + len);
      let moved = 0;
      for (let i = 1; i < seg.length; i++) moved += Math.hypot(seg[i].x - seg[i - 1].x, seg[i].y - seg[i - 1].y);
      if (moved < 0.4) continue;
      const t0 = seg[0].t, t1 = seg[seg.length - 1].t;
      const clicks = Gh.clicks.filter((c) => c.t >= t0 && c.t <= t1).map((c) => Object.assign({ done: false }, c));
      Gh.list.push({ seg, clicks, p: 0, t0, ago: F.t - t0, mirror: U.chance(0.3), hue: U.pick(['a', 'b', 'c']), trail: [] });
      return;
    }
  }

  function arrow(ctx, x, y, s, col, a) {
    ctx.save(); ctx.translate(x, y); ctx.scale(s, s);
    ctx.beginPath(); ctx.moveTo(0, 0); ctx.lineTo(0, 17); ctx.lineTo(4.5, 13); ctx.lineTo(7.5, 20); ctx.lineTo(10, 19); ctx.lineTo(7, 12); ctx.lineTo(12.5, 12); ctx.closePath();
    ctx.fillStyle = U.rgba([0, 0, 0], 0.6 * a); ctx.fill();
    ctx.strokeStyle = U.rgba(col, a); ctx.lineWidth = 1.4; ctx.stroke();
    ctx.restore();
  }

  Gh.draw = (F) => {
    const cfg = F.cfg;
    record(F);
    if (!cfg.ghosts) { Gh.list.length = 0; return; }
    Gh.next -= F.dt;
    if (Gh.next <= 0) { Gh.next = U.rand(18, 45) / (0.4 + cfg.chaos / 100); if (Gh.list.length < cfg.ghostCount) spawn(F); }
    const { ctx, W, H, P } = F;
    for (let i = Gh.list.length - 1; i >= 0; i--) {
      const g = Gh.list[i];
      const dur = g.seg[g.seg.length - 1].t - g.t0;
      g.p += F.dt;
      if (g.p > dur) { Gh.list.splice(i, 1); continue; }
      // find the sample for this moment
      const tt = g.t0 + g.p;
      let k = 0; while (k < g.seg.length - 2 && g.seg[k + 1].t < tt) k++;
      const a0 = g.seg[k], a1 = g.seg[k + 1] || a0;
      const f = a1.t > a0.t ? (tt - a0.t) / (a1.t - a0.t) : 0;
      let nx = U.lerp(a0.x, a1.x, f), ny = U.lerp(a0.y, a1.y, f);
      if (g.mirror) nx = 1 - nx;
      const x = nx * W, y = ny * H;
      g.trail.push([x, y]); if (g.trail.length > 24) g.trail.shift();
      const fade = Math.min(1, g.p * 2, (dur - g.p) * 2);
      const col = P[g.hue];
      // trail
      ctx.strokeStyle = U.rgba(col, 0.35 * fade); ctx.lineWidth = 2 * F.k;
      ctx.beginPath(); g.trail.forEach((p, j) => (j ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1]))); ctx.stroke();
      // the phantom pointer, jittering between two frames of itself
      const jx = U.chance(0.1) ? U.rand(-6, 6) * F.k : 0;
      arrow(ctx, x + jx, y, F.k * 1.3, col, fade);
      ctx.font = U.font(11 * F.k, 'mono'); ctx.textAlign = 'left'; ctx.fillStyle = U.rgba(col, 0.8 * fade);
      ctx.fillText(g.mirror ? `you (reflected), ${g.ago | 0}s ago` : `you, ${g.ago | 0}s ago`, x + 18 * F.k, y + 26 * F.k);
      // replay your clicks
      for (const c of g.clicks) {
        if (!c.done && c.t <= tt) {
          c.done = true;
          const cx = (g.mirror ? 1 - c.x / F.cssW : c.x / F.cssW) * W, cy = (c.y / F.cssH) * H;
          NS.fx.shock(cx, cy, 0.5); NS.fx.ring(F, cx, cy, 0.5);
          if (NS.sound) NS.sound.chirp(0.5, 0.15);
        }
      }
    }
  };
})(window.NS);
