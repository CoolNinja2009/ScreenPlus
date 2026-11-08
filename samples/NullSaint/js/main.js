'use strict';
/* NULL//SAINT - boot, resize, frame loop and composition */
(function (NS) {
  const U = NS.U;
  NS.load();

  const glCanvas = document.getElementById('gl');
  const hudCanvas = document.getElementById('hud');
  const hudCtx = hudCanvas.getContext('2d');
  const scene = U.makeCanvas(2, 2);
  const sctx = scene.getContext('2d', { alpha: false, desynchronized: true });
  const glOK = NS.post.init(glCanvas);
  let fallbackCtx = null;
  if (!glOK) {
    // no WebGL2: show the 2D scene directly (no signal damage shader)
    const c2 = document.createElement('canvas'); c2.id = 'fallback'; glCanvas.replaceWith(c2);
    fallbackCtx = c2.getContext('2d'); NS.fallbackCanvas = c2;
    console.warn('[NULL//SAINT] WebGL2 unavailable, running without the post pipeline');
  }

  const F = NS.F = {
    ctx: sctx, W: 2, H: 2, cssW: innerWidth, cssH: innerHeight, k: 1, t: 0, dt: 0.016, speed: 1,
    cfg: NS.cfg, P: NS.palette(), A: NS.audio, I: NS.input, gi: 1, rot: 0, par: 0, parY: 0, mx: 0, my: 0, blink: 1,
    G: null, hook: {}, flashState: { a: 0, c: [255, 255, 255] }, frame: 0,
  };
  F.flash = (a, c) => { const cap = F.cfg.safe ? 0.08 : 1; if (a * cap > F.flashState.a) { F.flashState.a = Math.min(a, cap); F.flashState.c = c || [255, 255, 255]; } };
  F.kick = (a) => NS.director.kick(a);

  let hudDpr = 1;
  function resize() {
    const cw = innerWidth, ch = innerHeight;
    const q = NS.cfg.quality;
    const scale = [0.5, 0.75, 1][q] * (q === 2 ? Math.min(devicePixelRatio || 1, 1.5) : 1);
    const sw = Math.max(64, Math.round(cw * scale)), sh = Math.max(64, Math.round(ch * scale));
    scene.width = sw; scene.height = sh;
    F.W = sw; F.H = sh; F.cssW = cw; F.cssH = ch; F.k = sw / cw;
    if (glOK) { glCanvas.width = sw; glCanvas.height = sh; NS.post.resize(sw, sh, sw, sh); }
    else { NS.fallbackCanvas.width = sw; NS.fallbackCanvas.height = sh; }
    hudDpr = Math.min(devicePixelRatio || 1, 2);
    hudCanvas.width = Math.round(cw * hudDpr); hudCanvas.height = Math.round(ch * hudDpr);
  }
  let resizeT = 0;
  addEventListener('resize', () => { clearTimeout(resizeT); resizeT = setTimeout(resize, 120); });
  resize();

  NS.onConfig((k) => {
    F.P = NS.palette();
    if (k === 'quality' || k === '*') resize();
    if (k === 'boot' && NS.cfg.boot) NS.director.startBoot();
  });

  // billboards are drawn between the city's mid layers
  F.hook.mid = (FF) => NS.holo.drawBoards(FF);

  // ---------- frame ----------
  let last = performance.now(), acc = 0;
  function frame(now) {
    requestAnimationFrame(frame);
    const cap = [30, 60, 0][NS.cfg.fps];
    const elapsed = now - last;
    if (cap && elapsed < 1000 / cap - 2) return;
    last = now;
    const rawDt = Math.min(0.1, elapsed / 1000);
    if (document.hidden) return;
    const cfg = NS.cfg;
    F.speed = cfg.speed / 100;
    F.dt = rawDt * F.speed;
    F.t += F.dt;
    F.frame++;
    const I = NS.input;
    I.update(rawDt);
    NS.audio.update(rawDt, F.t);
    NS.sys.update(rawDt, F.t);
    F.mx = I.sx * F.k; F.my = I.sy * F.k;
    const par = cfg.parallax / 100;
    F.par = (I.sx / F.cssW - 0.5) * par * 140 * F.k;
    F.parY = (I.sy / F.cssH - 0.5) * par * 70 * F.k;
    F.blink = NS.saint.blink;

    const G = NS.director.update(F);

    // input
    NS.sound.update(F);
    for (const c of I.takeClicks()) {
      NS.ghosts.noteClick(c.x, c.y, F.t);
      if (NS.panel && NS.panel.open) continue;
      if (NS.hud.click(c.x, c.y, F.cssW, F.cssH)) continue;
      const x = c.x * F.k, y = c.y * F.k;
      if (NS.girl.poke(F, x, y)) { NS.sound.blip(true); NS.fx.burst(F, x, y, 0.15); continue; }
      NS.sound.boom();
      if (NS.cfg.saint) NS.saint.poke(F, x, y);
      NS.fx.click(F, x, y);
    }
    NS.director.keys(F, I.takeKeys());

    // ---------- scene ----------
    const ctx = sctx;
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.globalAlpha = 1; ctx.globalCompositeOperation = 'source-over';
    NS.city.draw(F);
    NS.holo.drawArtifact(F);
    NS.glyphs.draw(F);
    NS.saint.draw(F);
    NS.girl.draw(F);
    NS.ghosts.draw(F);
    NS.photo.drawOver(F);
    NS.fx.draw(F);
    NS.director.draw(F);
    const booting = NS.director.drawBoot(F);
    if (cfg.hud && cfg.hudGlitch && !booting) { ctx.save(); ctx.setTransform(F.k, 0, 0, F.k, 0, 0); NS.hud.draw(ctx, F, F.cssW, F.cssH); ctx.restore(); }

    // ---------- post ----------
    const fs = F.flashState;
    fs.a = Math.max(0, fs.a - rawDt * 3);
    const flash = G.flash[3] > fs.a ? G.flash : [fs.c[0] / 255, fs.c[1] / 255, fs.c[2] / 255, fs.a];
    if (glOK && NS.post.ok) {
      const mvx = (I.vx * rawDt) / F.cssW, mvy = -(I.vy * rawDt) / F.cssH;
      const trail = cfg.trail / 100;
      NS.post.render(scene, {
        bloomThr: 0.55 - G.bloom * 0.15,
        mosh: {
          uRes: [F.W, F.H], uMouse: [I.sx / F.cssW, 1 - I.sy / F.cssH], uMouseVel: [mvx * trail, mvy * trail],
          uTime: F.t, uMosh: U.clamp(G.mosh, 0, 1), uMelt: U.clamp(G.melt, 0, 1), uTrail: trail, uSeed: G.seedJump + NS.director.burstSeed, uFreeze: G.freeze, uSwirl: G.swirl,
        },
        final: {
          uRes: [F.W, F.H], uMouse: [I.sx / F.cssW, 1 - I.sy / F.cssH], uMouseVel: [mvx, mvy], uTime: F.t, uSeed: G.seedJump + NS.director.burstSeed,
          uRGB: G.rgb, uScan: G.scan, uCurve: G.curve, uGrain: G.grain, uBlock: G.blocks, uTear: G.tear, uVHS: G.vhs, uSort: U.clamp(G.sort, 0, 1),
          uBloomK: G.bloom * 1.4, uVig: G.vig, uWave: G.wave, uPix: U.clamp(G.pix, 0, 1), uPost: U.clamp(G.post, 0, 1), uDither: cfg.dither ? 1 : 0,
          uMask: cfg.mask ? 1 : 0, uAscii: U.clamp(G.ascii, 0, 1), uAsciiCell: Math.max(6, Math.round(9 * F.k)), uLens: G.lens, uInvert: G.invert,
          uShuffle: G.shuffle, uHue: G.hue, uStatic: G.static, uBars: G.bars, uZoom: G.zoom,
          uColorMode: G.colorMode >= 0 ? G.colorMode : cfg.colorMode, uMirror: G.mirror >= 0 ? G.mirror : cfg.mirror,
          uShock: NS.fx.shockUniform(F), uFlash: flash, uTint: [F.P.a[0] / 255, F.P.a[1] / 255, F.P.a[2] / 255],
        },
      });
    } else if (fallbackCtx) {
      fallbackCtx.drawImage(scene, 0, 0);
      if (flash[3] > 0) { fallbackCtx.fillStyle = `rgba(${flash[0] * 255 | 0},${flash[1] * 255 | 0},${flash[2] * 255 | 0},${flash[3]})`; fallbackCtx.fillRect(0, 0, F.W, F.H); }
    }

    // ---------- clean HUD overlay ----------
    hudCtx.setTransform(1, 0, 0, 1, 0, 0);
    hudCtx.clearRect(0, 0, hudCanvas.width, hudCanvas.height);
    if (!booting && !(cfg.hud && cfg.hudGlitch)) {
      hudCtx.setTransform(hudDpr, 0, 0, hudDpr, 0, 0);
      NS.hud.draw(hudCtx, F, F.cssW, F.cssH);
    }
    if (NS.panel) NS.panel.tick(F);
  }

  NS._frame = frame; // lets tests step frames when the page is in the background
  if (NS.cfg.boot) NS.director.startBoot();
  // fonts can arrive late; rebuild text-heavy caches when they do
  if (document.fonts && document.fonts.ready) document.fonts.ready.then(() => { NS.city.key = ''; NS.glyphs.key = ''; });
  requestAnimationFrame(frame);
})(window.NS);
