'use strict';
/* NULL//SAINT - photo signal: a real photo (bundled CC0 "Neon streets" or your own) fed through the corruption */
(function (NS) {
  const U = NS.U;
  const Ph = NS.photo = { img: null, src: '', ok: false, cv: null, key: '', slices: [] };
  const BUNDLED = 'media/neon-streets.jpg';

  function source(cfg) {
    if (cfg.photo === 1) return BUNDLED;
    if (cfg.photo === 2 && cfg.photoFile) return 'photos/' + cfg.photoFile;
    return '';
  }
  function ensure(F) {
    const src = source(F.cfg);
    if (src !== Ph.src) {
      Ph.src = src; Ph.ok = false; Ph.cv = null; Ph.key = '';
      if (!src) return false;
      const im = new Image();
      im.onload = () => { Ph.ok = true; Ph.key = ''; };
      im.onerror = () => { Ph.ok = false; console.warn('[NULL//SAINT] photo failed to load:', src); };
      im.src = src; Ph.img = im;
    }
    if (!Ph.ok) return false;
    const key = [F.W, F.H, F.cfg.theme, F.cfg.photoTint, F.cfg.colA, F.cfg.colB, Ph.src].join('|');
    if (key !== Ph.key) bake(F, key);
    return true;
  }

  // cover-fit with a margin for drifting, tinted to the theme
  function bake(F, key) {
    const { W, H, P, cfg } = F;
    const m = 1.18, cw = Math.round(W * m), ch = Math.round(H * m);
    const cv = U.makeCanvas(cw, ch), c = cv.getContext('2d');
    const im = Ph.img, k = Math.max(cw / im.width, ch / im.height);
    c.drawImage(im, (cw - im.width * k) / 2, (ch - im.height * k) / 2, im.width * k, im.height * k);
    if (cfg.photoTint) {
      c.globalCompositeOperation = 'color';
      const g = c.createLinearGradient(0, 0, cw, ch);
      g.addColorStop(0, U.rgba(P.a, 0.55)); g.addColorStop(1, U.rgba(P.b, 0.55));
      c.fillStyle = g; c.fillRect(0, 0, cw, ch);
      c.globalCompositeOperation = 'multiply';
      c.fillStyle = U.rgba(U.mixc(P.bg2, [255, 255, 255], 0.5)); c.fillRect(0, 0, cw, ch);
    }
    // bake scanlines into the photo so it reads as a transmission
    c.globalCompositeOperation = 'source-over';
    c.fillStyle = 'rgba(0,0,0,0.22)';
    for (let y = 0; y < ch; y += 3) c.fillRect(0, y, cw, 1);
    Ph.cv = cv; Ph.key = key;
  }

  function blit(F, alpha, op) {
    const { ctx, W, H, t, cfg } = F;
    const cv = Ph.cv;
    const drift = cfg.photoDrift / 100;
    const mx = (cv.width - W) / 2, my = (cv.height - H) / 2;
    const ox = -mx + Math.sin(t * 0.05) * mx * drift - F.par * 0.3, oy = -my + Math.cos(t * 0.037) * my * drift - F.parY * 0.2;
    ctx.save();
    ctx.globalAlpha = alpha; ctx.globalCompositeOperation = op;
    // glitch slices: some horizontal bands slip sideways
    const n = F.gi > 0.6 ? Math.min(14, Math.floor(F.gi * 6)) : 0;
    if (!n) ctx.drawImage(cv, ox, oy);
    else {
      const bands = 24, bh = cv.height / bands;
      for (let i = 0; i < bands; i++) {
        const slip = U.hash(i, Math.floor(t * 6)) < n / 40 ? (U.hash(i * 3, Math.floor(t * 6)) - 0.5) * 140 * F.k : 0;
        ctx.drawImage(cv, 0, i * bh, cv.width, bh + 1, ox + slip, oy + i * bh, cv.width, bh + 1);
      }
    }
    ctx.restore();
  }

  // sky backdrop / full replacement: called by the city right after the sky gradient
  Ph.drawBack = (F) => {
    const cfg = F.cfg;
    if (!cfg.photo || cfg.photoPlace === 1 || !ensure(F)) return false;
    const a = cfg.photoMix / 100;
    if (cfg.photoPlace === 2) { blit(F, Math.min(1, a * 1.4), 'source-over'); return true; }
    // sky only: fade out towards the skyline
    blit(F, a, 'screen');
    return false;
  };
  // ghost overlay: called after the scene
  Ph.drawOver = (F) => {
    const cfg = F.cfg;
    if (!cfg.photo || cfg.photoPlace !== 1 || !ensure(F)) return;
    const a = (cfg.photoMix / 100) * (0.35 + 0.15 * Math.sin(F.t * 0.7));
    blit(F, a, 'screen');
  };
})(window.NS);
