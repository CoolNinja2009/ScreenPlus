'use strict';
/* NULL//SAINT - holographic billboards with generated ads, and the floating wireframe artifact */
(function (NS) {
  const U = NS.U;
  const Hh = NS.holo = { boards: [], key: '', art: { rx: 0, ry: 0, rz: 0, shape: 0, morph: 0, x: 0, y: 0 } };

  function newBoard(F, r, i, n) {
    const { W, H } = F;
    const w = W * r.range(0.07, 0.15), h = w * r.range(0.45, 0.9);
    return {
      x: W * ((i + 0.5) / n) + r.range(-W * 0.04, W * 0.04), y: H * r.range(0.32, 0.62),
      w, h, skew: r.range(-0.25, 0.25), depth: r.range(0.3, 0.7),
      ad: U.adLine(), c: r.pick(['a', 'b', 'c']), kind: r.int(0, 4), ph: r() * 100,
      swap: r.range(6, 14), t: 0, glitch: 0, mesh: r.pick(U.MESH_NAMES),
    };
  }

  Hh.build = (F) => {
    const r = U.rng(U.strSeed((F.cfg.seed || '') + 'ads'));
    const n = F.cfg.adCount;
    Hh.boards = [];
    for (let i = 0; i < n; i++) Hh.boards.push(newBoard(F, r, i, n));
    Hh.key = [F.W, F.H, n, F.cfg.seed].join('|');
  };

  function boardContent(F, b, x, y, w, h, col) {
    const { ctx, P, t } = F;
    const fl = b.glitch > 0 ? (U.chance(0.5) ? 0.2 : 1) : 0.85 + 0.15 * Math.sin(t * 40 + b.ph);
    ctx.save();
    ctx.beginPath(); ctx.rect(x, y, w, h); ctx.clip();
    // backing
    const g = ctx.createLinearGradient(x, y, x, y + h);
    g.addColorStop(0, U.rgba(col, 0.18 * fl)); g.addColorStop(1, U.rgba(col, 0.05 * fl));
    ctx.fillStyle = g; ctx.fillRect(x, y, w, h);
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    const cx = x + w / 2, cy = y + h / 2;
    if (b.kind === 0) { // brand + product + tagline
      ctx.font = U.font(h * 0.2, 'display', 'bold'); ctx.fillStyle = U.rgba(col, fl);
      ctx.fillText(U.corrupt(b.ad.brand, b.glitch * 0.4), cx, y + h * 0.25);
      ctx.font = U.font(h * 0.13, 'display'); ctx.fillStyle = U.rgba(P.text, 0.9 * fl);
      ctx.fillText(b.ad.product, cx, cy + h * 0.05);
      ctx.font = U.font(h * 0.09, 'mono'); ctx.fillStyle = U.rgba(P.c, 0.8 * fl);
      ctx.fillText(b.ad.tag, cx, y + h * 0.8);
    } else if (b.kind === 1) { // rotating product mesh
      ctx.strokeStyle = U.rgba(col, 0.9 * fl); ctx.lineWidth = 1.5;
      U.drawMesh(ctx, U.MESH[b.mesh], t * 0.7 + b.ph, t * 1.1, 0, cx - w * 0.2, cy, h * 0.3, b.glitch * 10);
      ctx.font = U.font(h * 0.15, 'display', 'bold'); ctx.fillStyle = U.rgba(P.text, fl); ctx.textAlign = 'left';
      ctx.fillText(b.ad.brand, cx, cy - h * 0.12);
      ctx.font = U.font(h * 0.08, 'mono'); ctx.fillStyle = U.rgba(P.c, fl);
      ctx.fillText('¥' + (U.hash(b.ph) * 99999 | 0) + '.99', cx, cy + h * 0.12);
    } else if (b.kind === 2) { // giant kanji with scrolling caption
      ctx.font = U.font(h * 0.62, 'jp', 'bold'); ctx.fillStyle = U.rgba(col, fl);
      ctx.fillText(U.TEXT.signs[(b.ph | 0) % U.TEXT.signs.length], cx, cy - h * 0.06);
      ctx.font = U.font(h * 0.11, 'mono'); ctx.fillStyle = U.rgba(P.text, 0.9 * fl);
      const cap = ('  ' + b.ad.tag + '  //  ' + b.ad.brand).repeat(3);
      const off = (t * 40) % (ctx.measureText(cap).width / 3);
      ctx.textAlign = 'left'; ctx.fillText(cap, x - off, y + h * 0.88);
    } else if (b.kind === 3) { // fake face scan "WANTED"
      ctx.strokeStyle = U.rgba(col, fl); ctx.lineWidth = 1;
      for (let i = 0; i < 12; i++) {
        const yy = y + h * 0.15 + i * h * 0.055;
        const ww = w * 0.22 * Math.sqrt(Math.max(0, 1 - Math.pow((i - 6) / 6.5, 2)));
        ctx.beginPath(); ctx.moveTo(cx - w * 0.25 - ww, yy); ctx.lineTo(cx - w * 0.25 + ww, yy); ctx.stroke();
      }
      ctx.font = U.font(h * 0.13, 'display', 'bold'); ctx.fillStyle = U.rgba(P.warn, fl); ctx.textAlign = 'left';
      ctx.fillText('WANTED', cx - w * 0.02, cy - h * 0.15);
      ctx.font = U.font(h * 0.08, 'mono'); ctx.fillStyle = U.rgba(P.text, fl);
      ctx.fillText('FOR: ' + U.pick(['DREAMING', 'BEING', 'BLINKING', 'REMEMBERING']), cx - w * 0.02, cy + h * 0.05);
      ctx.fillText('REWARD: YOUR NAME', cx - w * 0.02, cy + h * 0.2);
    } else { // spectrum / waveform screen
      const A = F.A;
      ctx.fillStyle = U.rgba(col, fl);
      const bars = 24;
      for (let i = 0; i < bars; i++) {
        const v = Math.max(A.spec[i * 2] || 0, 0.05 + 0.05 * Math.sin(t * 3 + i));
        const bh = v * h * 0.7;
        ctx.fillRect(x + (i / bars) * w + 1, y + h * 0.85 - bh, w / bars - 2, bh);
      }
      ctx.font = U.font(h * 0.1, 'mono'); ctx.fillStyle = U.rgba(P.text, fl);
      ctx.fillText('NOW PLAYING: YOUR THOUGHTS', cx, y + h * 0.1);
    }
    // scanlines over the board
    ctx.fillStyle = 'rgba(0,0,0,0.25)';
    for (let yy = y; yy < y + h; yy += 3) ctx.fillRect(x, yy, w, 1);
    ctx.restore();
    // frame
    ctx.strokeStyle = U.rgba(col, 0.8 * fl); ctx.lineWidth = 1.5;
    U.brackets(ctx, x - 4, y - 4, w + 8, h + 8, Math.min(w, h) * 0.15);
  }

  Hh.drawBoards = (F) => {
    const { cfg, ctx, P, dt } = F;
    if (!cfg.ads || cfg.adCount <= 0) return;
    const key = [F.W, F.H, cfg.adCount, cfg.seed].join('|');
    if (key !== Hh.key) Hh.build(F);
    for (const b of Hh.boards) {
      b.t += dt;
      if (b.t > b.swap) { b.t = 0; b.glitch = 0.6; b.ad = U.adLine(); b.kind = U.randi(0, 4); b.mesh = U.pick(U.MESH_NAMES); }
      if (U.chance(dt * 0.1 * F.gi)) b.glitch = 0.4;
      b.glitch = Math.max(0, b.glitch - dt);
      const col = P[b.c];
      const x = b.x - F.par * b.depth, y = b.y - F.parY * b.depth * 0.4;
      ctx.save();
      ctx.translate(x, y);
      ctx.transform(1, b.skew * 0.3, 0, 1, 0, 0);
      if (b.glitch > 0 && U.chance(0.5)) ctx.translate(U.rand(-8, 8), 0);
      // projector beam from below
      ctx.save(); ctx.globalCompositeOperation = 'lighter';
      const pg = ctx.createLinearGradient(0, b.h * 0.5, 0, b.h * 1.8);
      pg.addColorStop(0, U.rgba(col, 0.1)); pg.addColorStop(1, U.rgba(col, 0));
      ctx.fillStyle = pg;
      ctx.beginPath(); ctx.moveTo(-b.w / 2, b.h / 2); ctx.lineTo(b.w / 2, b.h / 2); ctx.lineTo(b.w * 0.05, b.h * 1.8); ctx.lineTo(-b.w * 0.05, b.h * 1.8); ctx.fill();
      ctx.restore();
      boardContent(F, b, -b.w / 2, -b.h / 2, b.w, b.h, col);
      ctx.restore();
    }
  };

  // ---------- floating artifact ----------
  Hh.drawArtifact = (F) => {
    const { cfg, ctx, P, t, dt, W, H, A } = F;
    if (!cfg.holo) return;
    const a = Hh.art;
    const sp = 0.3 + A.mid * 2;
    a.rx += dt * 0.37 * sp; a.ry += dt * 0.53 * sp; a.rz += dt * 0.11 * sp;
    let shape = cfg.holoShape;
    if (shape === 5) shape = Math.floor((t || 0) / 12);
    shape = (((shape | 0) % 5) + 5) % 5;
    const mesh = U.MESH[U.MESH_NAMES[shape]];
    const S = Math.min(W, H) * (0.05 + cfg.holoSize / 100 * 0.18) * (1 + A.bass * 0.25);
    const saintX = NS.saint && NS.cfg.saint ? NS.saint.x : W / 2;
    const tx = saintX < W / 2 ? W * 0.8 : W * 0.2;
    a.x = a.x ? U.damp(a.x, tx, 0.5, dt) : tx;
    const cx = a.x - F.par * 0.5, cy = H * 0.24 + Math.sin(t * 0.6) * H * 0.03 - F.parY * 0.2;
    ctx.save();
    ctx.globalCompositeOperation = 'lighter';
    // echo copies (time smear)
    for (let e = 3; e >= 0; e--) {
      ctx.strokeStyle = U.rgba(e ? P.b : P.a, e ? 0.12 : 0.9);
      ctx.lineWidth = e ? 1 : 1.6;
      const pts = U.drawMesh(ctx, mesh, a.rx - e * 0.08, a.ry - e * 0.12, a.rz, cx, cy, S, F.gi * 6);
      if (!e) {
        ctx.fillStyle = U.rgba(P.c, 0.9);
        for (const p of pts) ctx.fillRect(p[0] - 1.5, p[1] - 1.5, 3, 3);
        // labels on a few vertices
        ctx.font = U.font(10); ctx.fillStyle = U.rgba(P.text, 0.6);
        for (let i = 0; i < pts.length; i += Math.max(1, (pts.length / 4) | 0)) ctx.fillText('v' + i + ':' + pts[i][2].toFixed(2), pts[i][0] + 5, pts[i][1] - 5);
      }
    }
    // orbit ring + base projector
    ctx.strokeStyle = U.rgba(P.b, 0.35); ctx.lineWidth = 1;
    ctx.beginPath(); ctx.ellipse(cx, cy + S * 1.25, S * 1.1, S * 0.22, 0, 0, U.TAU); ctx.stroke();
    ctx.beginPath(); ctx.ellipse(cx, cy + S * 1.25, S * 0.7, S * 0.14, 0, t, t + 4); ctx.stroke();
    const pg = ctx.createLinearGradient(0, cy + S * 1.25, 0, cy - S);
    pg.addColorStop(0, U.rgba(P.b, 0.12)); pg.addColorStop(1, U.rgba(P.b, 0));
    ctx.fillStyle = pg;
    ctx.beginPath(); ctx.moveTo(cx - S * 0.15, cy + S * 1.25); ctx.lineTo(cx + S * 0.15, cy + S * 1.25); ctx.lineTo(cx + S * 1.1, cy - S); ctx.lineTo(cx - S * 1.1, cy - S); ctx.fill();
    ctx.restore();
    ctx.font = U.font(11, 'mono'); ctx.fillStyle = U.rgba(P.c, 0.75); ctx.textAlign = 'center';
    ctx.fillText('ARTIFACT_' + U.MESH_NAMES[shape].toUpperCase() + ' // ORIGIN UNKNOWN', cx, cy + S * 1.6);
  };
})(window.NS);
