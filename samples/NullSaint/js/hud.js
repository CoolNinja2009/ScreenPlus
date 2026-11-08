'use strict';
/* NULL//SAINT - HUD: brand, clock, logs, system monitor, spectrum radar, reticle, ticker, haunted popups.
   Works in CSS pixels; the caller sets the transform. */
(function (NS) {
  const U = NS.U;
  const H = NS.hud = {
    logs: [], logT: 0, popups: [], popT: 8, hist: { cpu: [], gpu: [], ram: [] }, histT: 0,
    tickerX: 0, ticker: '', lastSec: -1, secGlitch: 0, start: performance.now(), banner: null,
  };

  // ---------- popups ----------
  H.spawnPopup = (W, Hh, x, y, text) => {
    if (H.popups.length > 14) return;
    const w = U.rand(260, 380), h = U.rand(130, 170);
    const p = {
      x: x != null ? x : U.rand(40, W - w - 40), y: y != null ? y : U.rand(80, Hh - h - 120), w, h, t: 0,
      title: U.pick(U.TEXT.titles), text: text || U.pick(U.TEXT.errors), btns: U.shuffle(U.TEXT.buttons.slice()).slice(0, U.randi(1, 3)),
      prog: U.rand(0.2, 1), progDir: U.chance(0.5) ? -1 : 1, hydra: U.chance(0.25), jitter: U.chance(0.3), closing: 0, code: U.hex(U.randi(0, 0xffffff), 6),
    };
    H.popups.push(p);
    if (NS.sound) NS.sound.blip(false);
    return p;
  };
  H.click = (x, y, W, Hh) => {
    for (let i = H.popups.length - 1; i >= 0; i--) {
      const p = H.popups[i];
      if (p.closing) continue;
      if (x >= p.x && x <= p.x + p.w && y >= p.y && y <= p.y + p.h) {
        // buttons or the close box
        const hitBtn = p.btnRects && p.btnRects.find((r) => x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h);
        const hitClose = x > p.x + p.w - 28 && y < p.y + 24;
        if (hitBtn || hitClose) {
          p.closing = 0.001;
          if (p.hydra) for (let k = 0; k < 2; k++) H.spawnPopup(W, Hh, U.clamp(p.x + U.rand(-80, 80), 0, W - p.w), U.clamp(p.y + U.rand(-60, 60), 0, Hh - p.h), U.pick(['YOU CANNOT CLOSE ME', 'I HAVE CHILDREN NOW', 'NICE TRY', 'THERE ARE MORE OF US']));
          if (NS.fx && NS.F) NS.fx.burst(NS.F, (p.x + p.w / 2) * NS.F.k, (p.y + p.h / 2) * NS.F.k, 0.4);
        } else { p.dragOff = [x - p.x, y - p.y]; }
        // bring to front
        H.popups.splice(i, 1); H.popups.push(p);
        return true;
      }
    }
    return false;
  };

  function drawPopup(c, p, P, t, s) {
    const fs = 13 * s;
    const ox = p.jitter && Math.random() < 0.08 ? U.rand(-6, 6) : 0;
    const k = p.closing ? 1 - p.closing : U.easeOutBack(Math.min(1, p.t * 4));
    c.save();
    c.translate(p.x + p.w / 2 + ox, p.y + p.h / 2);
    c.scale(p.closing ? 1 + p.closing * 0.3 : k, p.closing ? Math.max(0.02, 1 - p.closing * 3) : k);
    c.translate(-p.w / 2, -p.h / 2);
    // shadow copies (window trails)
    c.fillStyle = U.rgba(P.a, 0.12); c.fillRect(6, 6, p.w, p.h);
    c.fillStyle = U.rgba(U.scalec(P.bg, 0.5), 0.94); c.fillRect(0, 0, p.w, p.h);
    c.strokeStyle = U.rgba(P.a, 0.9); c.lineWidth = 1.5; c.strokeRect(0.5, 0.5, p.w - 1, p.h - 1);
    // title bar
    const g = c.createLinearGradient(0, 0, p.w, 0);
    g.addColorStop(0, U.rgba(P.a, 0.9)); g.addColorStop(1, U.rgba(P.b, 0.6));
    c.fillStyle = g; c.fillRect(0, 0, p.w, 22 * s);
    c.font = U.font(fs, 'mono', 'bold'); c.fillStyle = '#000'; c.textBaseline = 'middle'; c.textAlign = 'left';
    c.fillText(U.corrupt(p.title, 0.05) + '  [' + p.code + ']', 8, 11 * s);
    c.fillText('×', p.w - 18, 11 * s);
    // warning glyph
    c.fillStyle = U.rgba(P.c, 0.95);
    c.beginPath(); c.moveTo(28, 38 * s); c.lineTo(46, 70 * s); c.lineTo(10, 70 * s); c.closePath(); c.fill();
    c.fillStyle = '#000'; c.font = U.font(18 * s, 'mono', 'bold'); c.textAlign = 'center'; c.fillText('!', 28, 59 * s);
    // message (wrapped)
    c.textAlign = 'left'; c.font = U.font(fs, 'mono'); c.fillStyle = U.rgba(P.text, 1);
    const words = p.text.split(' '); let line = '', ly = 44 * s;
    for (const w of words) {
      const test = line ? line + ' ' + w : w;
      if (c.measureText(test).width > p.w - 70) { c.fillText(line, 58, ly); line = w; ly += fs * 1.3; } else line = test;
    }
    c.fillText(U.corrupt(line, 0.03), 58, ly);
    // backwards progress bar
    p.prog = U.clamp(p.prog + p.progDir * 0.05 * (1 / 60), 0, 1);
    if (p.prog <= 0 || p.prog >= 1) p.progDir *= -1;
    const by = p.h - 54 * s;
    c.strokeStyle = U.rgba(P.b, 0.8); c.strokeRect(58, by, p.w - 76, 8 * s);
    c.fillStyle = U.rgba(P.b, 0.9); c.fillRect(58, by, (p.w - 76) * p.prog, 8 * s);
    c.font = U.font(10 * s, 'mono'); c.fillStyle = U.rgba(P.text, 0.6);
    c.fillText((p.prog * 100).toFixed(1) + '% ' + (p.progDir < 0 ? 'UNINSTALLING YOU' : 'INSTALLING YOU'), 58, by - 8 * s);
    // buttons
    p.btnRects = [];
    let bx = p.w - 12;
    c.font = U.font(12 * s, 'mono', 'bold');
    for (let i = p.btns.length - 1; i >= 0; i--) {
      const bw = c.measureText(p.btns[i]).width + 22, bh = 22 * s;
      bx -= bw;
      const r = { x: p.x + bx, y: p.y + p.h - bh - 10, w: bw, h: bh };
      p.btnRects.push(r);
      const hot = NS.input.x >= r.x && NS.input.x <= r.x + r.w && NS.input.y >= r.y && NS.input.y <= r.y + r.h;
      c.fillStyle = hot ? U.rgba(P.a, 0.9) : U.rgba(P.a, 0.15);
      c.fillRect(bx, p.h - bh - 10, bw, bh);
      c.strokeStyle = U.rgba(P.a, 0.9); c.strokeRect(bx + 0.5, p.h - bh - 9.5, bw - 1, bh - 1);
      c.fillStyle = hot ? '#000' : U.rgba(P.text, 1); c.textAlign = 'center';
      c.fillText(p.btns[i], bx + bw / 2, p.h - bh / 2 - 10);
      bx -= 8;
    }
    c.restore();
  }

  // ---------- pieces ----------
  function brand(c, F, P, s, W) {
    const t = F.t, x = 28 * s, y = 34 * s;
    c.textAlign = 'left'; c.textBaseline = 'alphabetic';
    c.font = U.font(30 * s, 'display', '900');
    const title = U.chance(0.03 * F.gi) ? U.corrupt('NULL//SAINT', 0.4) : 'NULL//SAINT';
    U.glitchText(c, title, x, y + 10 * s, P.a, P.b, 2 + F.gi * 2 * Math.random(), 1);
    c.font = U.font(11 * s, 'mono');
    c.fillStyle = U.rgba(P.c, 0.9);
    c.fillText('GHOST SIGNAL ' + U.pick(['v6.6.6', 'v6.6.6', 'v6.6.6', 'v?.?.?']) + ' // NODE ' + U.hex(U.strSeed(F.cfg.seed || '') & 0xffff), x, y + 30 * s);
    const up = (performance.now() - H.start) / 1000;
    const op = (F.cfg.operator || 'OPERATOR').toUpperCase().slice(0, 18);
    c.fillStyle = U.rgba(P.text, 0.75);
    c.fillText(`OPERATOR: ${op}   UPTIME ${U.pad((up / 3600) | 0)}:${U.pad(((up / 60) | 0) % 60)}:${U.pad((up | 0) % 60)}`, x, y + 46 * s);
    const link = F.gi > 1.2 ? 'COLLAPSING' : F.gi > 0.7 ? 'UNSTABLE' : 'STABLE';
    c.fillStyle = U.rgba(F.gi > 0.7 ? P.warn : P.b, 0.9);
    c.fillText(`LINK ${link}  ${'▮'.repeat(Math.max(1, 8 - Math.round(F.gi * 4)))}${'▯'.repeat(Math.min(7, Math.round(F.gi * 4)))}`, x, y + 62 * s);
    c.fillStyle = U.rgba(P.text, 0.5);
    U.barcode(c, x, y + 72 * s, 140 * s, 12 * s, 99 + ((t * 2) | 0));
  }

  function clock(c, F, P, s, W) {
    const now = new Date(); const cfg = F.cfg;
    let h = now.getHours(); const m = now.getMinutes(), sec = now.getSeconds();
    let suffix = '';
    if (!cfg.clock24) { suffix = h >= 12 ? ' PM' : ' AM'; h = h % 12 || 12; }
    if (sec !== H.lastSec) { H.lastSec = sec; H.secGlitch = 1; }
    H.secGlitch = Math.max(0, H.secGlitch - F.dt * 5);
    let str = `${U.pad(h)}:${U.pad(m)}`;
    // sometimes the time is wrong
    if (F.gi > 0.4 && U.chance(0.004 * F.gi)) H.wrong = 0.4;
    if (H.wrong > 0) { H.wrong -= F.dt; str = `${U.pad(U.randi(0, 99))}:${U.pad(U.randi(0, 99))}`; }
    const x = W - 28 * s, y = 70 * s;
    c.textAlign = 'right'; c.textBaseline = 'alphabetic';
    c.font = U.font(64 * s, 'display', '900');
    U.glitchText(c, str, x - 60 * s, y, P.a, P.b, 1.5 + H.secGlitch * 4, 1);
    c.font = U.font(28 * s, 'display', '700'); c.fillStyle = U.rgba(P.c, 1);
    c.fillText(U.pad(sec) + suffix, x, y);
    c.font = U.font(12 * s, 'mono'); c.fillStyle = U.rgba(P.text, 0.8);
    const day = U.TEXT.days[now.getDay()];
    c.fillText(`${day} ${now.getFullYear()}.${U.pad(now.getMonth() + 1)}.${U.pad(now.getDate())}  CYCLE ${(now.getTime() / 86400000) | 0}`, x, y + 20 * s);
    c.fillStyle = U.rgba(P.b, 0.6);
    const beat = ((now.getUTCHours() * 3600 + now.getUTCMinutes() * 60 + now.getUTCSeconds() + 3600) / 86.4) % 1000;
    c.fillText(`@${beat.toFixed(2)} .beats  ${U.hex(now.getTime() / 1000 | 0, 8)}`, x, y + 36 * s);
  }

  function logs(c, F, P, s, Hh) {
    const rate = 0.15 + (1 - F.cfg.chaos / 100) * 0.6;
    H.logT -= F.dt * (1 + F.gi);
    if (H.logT <= 0) {
      H.logT = rate * U.rand(0.3, 1.6);
      const msgs = (F.cfg.messages || '').split('|').map((m) => m.trim()).filter(Boolean);
      const line = msgs.length && U.chance(0.15) ? `[${F.t.toFixed(3).padStart(9, '0')}] >> ${msgs[U.randi(0, msgs.length - 1)]}` : U.logLine(F.t);
      H.logs.push({ s: line, t: 0 });
      if (H.logs.length > 16) H.logs.shift();
    }
    const x = 28 * s, lh = 14 * s;
    let y = Hh - 70 * s - H.logs.length * lh;
    c.textAlign = 'left'; c.textBaseline = 'alphabetic'; c.font = U.font(11 * s, 'mono');
    c.fillStyle = U.rgba(P.b, 0.6); c.fillText('// KERNEL STREAM', x, y - 6 * s);
    for (let i = 0; i < H.logs.length; i++) {
      const L = H.logs[i]; L.t += F.dt;
      const shown = U.decode(L.s, L.t * 4);
      const a = 0.3 + 0.7 * (i / H.logs.length);
      const warn = L.s.includes('WARN') || L.s.includes('FAULT');
      c.fillStyle = U.rgba(warn ? P.warn : P.text, a);
      c.fillText(shown.slice(0, 64), x, y + (i + 1) * lh);
    }
  }

  function sysmon(c, F, P, s, W, Hh) {
    const sys = NS.sys;
    H.histT -= F.dt;
    if (H.histT <= 0) {
      H.histT = 0.25;
      H.hist.cpu.push(sys.cpuPct()); H.hist.gpu.push(sys.gpuPct()); H.hist.ram.push(sys.ramPct());
      for (const k in H.hist) if (H.hist[k].length > 40) H.hist[k].shift();
    }
    const w = 220 * s, x = W - w - 28 * s, y0 = Hh - 260 * s;
    c.textAlign = 'left'; c.font = U.font(11 * s, 'mono');
    c.fillStyle = U.rgba(P.b, 0.6); c.fillText(sys.live ? '// VITALS (HOST)' : '// VITALS (SIMULATED)', x, y0);
    const rows = [['CPU', sys.cpuPct(), H.hist.cpu, P.a], ['GPU', sys.gpuPct(), H.hist.gpu, P.b], ['MEM', sys.ramPct(), H.hist.ram, P.c],
      ['SOUL', 100 - F.gi * 40 - F.rot * 50, null, P.warn]];
    rows.forEach(([name, v, hist, col], i) => {
      const y = y0 + 14 * s + i * 26 * s;
      v = U.clamp(v, 0, 100);
      c.fillStyle = U.rgba(P.text, 0.8); c.fillText(name, x, y + 10 * s);
      c.fillText(String(v | 0).padStart(3) + '%', x + w - 34 * s, y + 10 * s);
      const bx = x + 40 * s, bw = w - 80 * s;
      c.strokeStyle = U.rgba(col, 0.5); c.lineWidth = 1; c.strokeRect(bx, y + 2 * s, bw, 9 * s);
      const segs = 20;
      for (let k = 0; k < segs; k++) if (k / segs < v / 100) { c.fillStyle = U.rgba(k / segs > 0.8 ? P.warn : col, 0.85); c.fillRect(bx + 1 + (k * bw) / segs, y + 3 * s, bw / segs - 2, 7 * s); }
      if (hist && hist.length > 1) {
        c.strokeStyle = U.rgba(col, 0.35); c.beginPath();
        hist.forEach((hv, k) => { const px = bx + (k / 39) * bw, py = y + 20 * s - (hv / 100) * 8 * s; k ? c.lineTo(px, py) : c.moveTo(px, py); });
        c.stroke();
      }
    });
  }

  function radar(c, F, P, s, W, Hh) {
    if (!F.cfg.audioRing) return;
    const A = F.A;
    const R = 62 * s, cx = W - 28 * s - R - 10 * s, cy = Hh - 112 * s - R - 10 * s - 160 * s;
    c.save();
    c.strokeStyle = U.rgba(P.b, 0.35); c.lineWidth = 1;
    for (let i = 1; i <= 3; i++) { c.beginPath(); c.arc(cx, cy, (R * i) / 3, 0, U.TAU); c.stroke(); }
    c.beginPath(); c.moveTo(cx - R, cy); c.lineTo(cx + R, cy); c.moveTo(cx, cy - R); c.lineTo(cx, cy + R); c.stroke();
    // sweep
    const a = F.t * 1.6;
    const g = c.createConicGradient ? c.createConicGradient(a - 0.8, cx, cy) : null;
    if (g) { g.addColorStop(0, U.rgba(P.a, 0)); g.addColorStop(0.12, U.rgba(P.a, 0.35)); g.addColorStop(0.125, U.rgba(P.a, 0)); c.fillStyle = g; c.beginPath(); c.arc(cx, cy, R, 0, U.TAU); c.fill(); }
    // spectrum ring
    c.strokeStyle = U.rgba(P.c, 0.9); c.lineWidth = 2 * s;
    c.beginPath();
    for (let i = 0; i <= 64; i++) {
      const v = A.spec[i % 64], ang = (i / 64) * U.TAU - Math.PI / 2;
      const rr = R * 0.55 + v * R * 0.6 + (A.live || F.cfg.fakeBeat ? 0 : Math.sin(F.t * 2 + i * 0.5) * 2);
      const px = cx + Math.cos(ang) * rr, py = cy + Math.sin(ang) * rr;
      i ? c.lineTo(px, py) : c.moveTo(px, py);
    }
    c.stroke();
    // blips = the Saint and the cursor
    const blip = (bx, by, col, label) => {
      const dx = (bx - W / 2) / W, dy = (by - Hh / 2) / Hh;
      const px = cx + dx * R * 1.6, py = cy + dy * R * 1.6;
      c.fillStyle = U.rgba(col, 0.9); c.fillRect(px - 2, py - 2, 4, 4);
      c.font = U.font(9 * s, 'mono'); c.fillText(label, px + 5, py + 3);
    };
    blip(NS.input.x, NS.input.y, P.c, 'YOU');
    if (F.cfg.saint && NS.saint.x) blip(NS.saint.x / F.k, NS.saint.y / F.k, P.warn, 'IT');
    c.font = U.font(10 * s, 'mono'); c.fillStyle = U.rgba(P.text, 0.6); c.textAlign = 'center';
    c.fillText(A.live ? `AUDIO ${(A.level * 100) | 0}%  BEAT ${A.beatCount}` : (F.cfg.fakeBeat ? 'PHANTOM PULSE' : 'NO AUDIO // LISTENING'), cx, cy + R + 14 * s);
    c.restore();
  }

  function reticle(c, F, P, s, W, Hh) {
    const I = NS.input; if (!I.inside) return;
    const x = I.sx, y = I.sy, t = F.t;
    c.save();
    c.strokeStyle = U.rgba(P.b, 0.12); c.lineWidth = 1;
    c.beginPath(); c.moveTo(0, y); c.lineTo(W, y); c.moveTo(x, 0); c.lineTo(x, Hh); c.stroke();
    const r = (22 + Math.sin(t * 4) * 3 + Math.min(30, I.speed * 0.01)) * s;
    c.translate(x, y); c.rotate(t * 0.8);
    c.strokeStyle = U.rgba(P.c, 0.9); c.lineWidth = 1.5;
    for (let i = 0; i < 4; i++) { c.rotate(Math.PI / 2); c.beginPath(); c.arc(0, 0, r, -0.4, 0.4); c.stroke(); }
    c.rotate(-t * 1.6);
    c.strokeStyle = U.rgba(P.a, 0.7);
    for (let i = 0; i < 3; i++) { c.rotate(U.TAU / 3); c.beginPath(); c.moveTo(r * 1.4, 0); c.lineTo(r * 1.8, 0); c.stroke(); }
    c.restore();
    c.font = U.font(10 * s, 'mono'); c.fillStyle = U.rgba(P.text, 0.8); c.textAlign = 'left';
    c.fillText(`X:${(x | 0).toString().padStart(4, '0')} Y:${(y | 0).toString().padStart(4, '0')}`, x + r + 14 * s, y - 8 * s);
    const d = NS.saint && F.cfg.saint ? U.dist(x * F.k, y * F.k, NS.saint.x, NS.saint.y) / F.k : 0;
    c.fillStyle = U.rgba(d < 200 ? P.warn : P.c, 0.8);
    c.fillText(d ? `DIST TO IT: ${(d | 0)}px` : 'TARGET: YOU', x + r + 14 * s, y + 6 * s);
    if (I.idle > 3) { c.fillStyle = U.rgba(P.warn, 0.6 + 0.4 * Math.sin(t * 6)); c.fillText(`IDLE ${I.idle.toFixed(0)}s`, x + r + 14 * s, y + 20 * s); }
  }

  function ticker(c, F, P, s, W, Hh) {
    const h = 24 * s, y = Hh - h;
    c.fillStyle = U.rgba(U.scalec(P.bg, 0.4), 0.85); c.fillRect(0, y, W, h);
    c.fillStyle = U.rgba(P.a, 0.9); c.fillRect(0, y, W, 1.5);
    c.fillStyle = U.rgba(P.a, 1); c.fillRect(0, y, 110 * s, h);
    c.font = U.font(12 * s, 'mono', 'bold'); c.textBaseline = 'middle'; c.textAlign = 'left';
    c.fillStyle = '#000'; c.fillText(U.chance(0.02) ? 'L I V E' : 'LIVE // NWS', 10 * s, y + h / 2);
    if (!H.ticker || H.tickerX < -H.tickerW) {
      const own = (F.cfg.messages || '').split('|').map((m) => m.trim()).filter(Boolean);
      const items = U.shuffle(U.TEXT.news.slice()).slice(0, 5).concat(own);
      H.ticker = items.join('   ✦   ') + '   ✦   ';
      c.font = U.font(12 * s, 'mono');
      H.tickerW = c.measureText(H.ticker).width;
      H.tickerX = W;
    }
    H.tickerX -= F.dt * 80 * s;
    c.save(); c.beginPath(); c.rect(110 * s, y, W, h); c.clip();
    c.font = U.font(12 * s, 'mono'); c.fillStyle = U.rgba(P.text, 0.9);
    c.fillText(U.corrupt(H.ticker, 0.005 * F.gi), H.tickerX, y + h / 2);
    c.restore();
  }

  function rulers(c, F, P, s, W, Hh) {
    c.strokeStyle = U.rgba(P.b, 0.35); c.lineWidth = 1; c.fillStyle = U.rgba(P.b, 0.5);
    c.font = U.font(8 * s, 'mono'); c.textAlign = 'left';
    const off = (F.t * 20) % 20;
    for (let y = 140 * s; y < Hh - 320 * s; y += 20) {
      const yy = y + off; const big = ((yy / 20) | 0) % 5 === 0;
      c.beginPath(); c.moveTo(8, yy); c.lineTo(big ? 18 : 12, yy); c.stroke();
      if (big) c.fillText(String((yy * 7) | 0), 22, yy + 3);
    }
    // corner frame
    c.strokeStyle = U.rgba(P.a, 0.4);
    U.brackets(c, 12, 12, W - 24, Hh - 48 * s, 40 * s);
    // floating QR block
    c.fillStyle = U.rgba(P.text, 0.35);
    U.qrish(c, W - 28 * s - 50 * s, 120 * s, 50 * s, 17, (F.t * 0.5) | 0);
  }

  function banner(c, F, P, s, W, Hh) {
    const b = F.G && F.G.banner; if (!b) return;
    const a = b.a;
    const h = 70 * s, y = Hh * 0.5 - h / 2;
    c.save();
    c.fillStyle = U.rgba(b.c || P.warn, 0.18 * a); c.fillRect(0, y, W, h);
    // hazard stripes
    c.beginPath(); c.rect(0, y, W, 8 * s); c.rect(0, y + h - 8 * s, W, 8 * s); c.clip();
    c.fillStyle = U.rgba(b.c || P.warn, 0.9 * a);
    for (let x = -40 + ((F.t * 60) % 40); x < W; x += 40) { c.beginPath(); c.moveTo(x, y); c.lineTo(x + 20, y); c.lineTo(x + 20 + h, y + h); c.lineTo(x + h, y + h); c.fill(); }
    c.restore();
    c.font = U.font(38 * s, 'display', '900'); c.textAlign = 'center'; c.textBaseline = 'middle';
    U.glitchText(c, U.corrupt(b.text, 0.08), W / 2, y + h / 2 - 4 * s, P.a, P.b, 3 + Math.random() * 6, a);
    if (b.sub) { c.font = U.font(12 * s, 'mono'); c.fillStyle = U.rgba(P.text, a); c.fillText(b.sub, W / 2, y + h + 14 * s); }
  }

  // ---------- main ----------
  H.draw = (c, F, W, Hh) => {
    const cfg = F.cfg, P = F.P;
    if (!cfg.hud) { banner(c, F, P, cfg.hudScale / 100, W, Hh); drawPopups(c, F, P, W, Hh); return; }
    const s = cfg.hudScale / 100;
    c.save();
    if (cfg.hudGlitch === false && F.G && F.G.hudShake) c.translate(U.rand(-3, 3) * F.G.hudShake, 0);
    rulers(c, F, P, s, W, Hh);
    brand(c, F, P, s, W);
    if (cfg.clock) clock(c, F, P, s, W);
    if (cfg.logs) logs(c, F, P, s, cfg.ticker ? Hh - 24 * s : Hh);
    if (cfg.sys) sysmon(c, F, P, s, W, cfg.ticker ? Hh - 24 * s : Hh);
    radar(c, F, P, s, W, Hh);
    if (cfg.ticker) ticker(c, F, P, s, W, Hh);
    if (cfg.reticle) reticle(c, F, P, s, W, Hh);
    banner(c, F, P, s, W, Hh);
    c.restore();
    drawPopups(c, F, P, W, Hh);
  };
  function drawPopups(c, F, P, W, Hh) {
    const cfg = F.cfg, s = cfg.hudScale / 100;
    if (cfg.popups && cfg.popupRate > 0) {
      H.popT -= F.dt * (cfg.popupRate / 50) * (0.5 + F.gi);
      if (H.popT <= 0) { H.popT = U.rand(10, 30); if (H.popups.length < 5) H.spawnPopup(W, Hh); }
    }
    const I = NS.input;
    for (let i = H.popups.length - 1; i >= 0; i--) {
      const p = H.popups[i];
      p.t += F.dt;
      if (p.dragOff && I.down) { p.x = I.x - p.dragOff[0]; p.y = I.y - p.dragOff[1]; } else p.dragOff = null;
      if (p.closing) { p.closing += F.dt * 4; if (p.closing >= 1) { H.popups.splice(i, 1); continue; } }
      if (p.t > 40 && !p.closing) p.closing = 0.001; // they give up eventually
    }
    for (const p of H.popups) drawPopup(c, p, P, F.t, s);
  }
})(window.NS);
