'use strict';
/* NULL//SAINT - Kei, the cyber girl: an original procedurally drawn character (or your own image).
   She stands on the near rooftop, follows the cursor, blinks, bobs to audio and glitches. */
(function (NS) {
  const U = NS.U;
  const K = NS.girl = {
    lx: 0, ly: 0, fs: 1, blink: 1, blinkT: 2.5, blinkP: null, surprise: 0, happy: 0, wave: 0,
    say: null, sayT: 9, glitch: 0, cv: null, img: null, imgSrc: '', imgOK: false, x: 0, hearts: [],
  };

  const LINES = [
    'nya~ system ok', "don't look at the moon", 'i like your cursor', 'is it watching us?', 'rain tastes like static',
    'brb, defragmenting', 'hi operator ♡', 'my headphones hear ghosts', 'the face is new', 'stay a while?',
    'ur wallpaper is haunted lol', '404: sleep not found', 'neon is my love language', 'shh... it hears us',
  ];
  const POKED = ['hey!', 'eek!', '!?', 'that tickles', 'rude.', 'hehe', 'stop it~', 'ow (not really)'];

  const HAIR = [null, null, [235, 240, 255], [24, 20, 34], [255, 120, 200], [120, 255, 230]];
  const HAIR_NAMES = ['Neon A', 'Neon B', 'Silver', 'Midnight', 'Sakura', 'Mint'];
  K.HAIR_NAMES = HAIR_NAMES;

  K.poke = (F, x, y) => {
    if (!F.cfg.girl || !K.box) return false;
    const b = K.box;
    if (x < b.x || x > b.x + b.w || y < b.y || y > b.y + b.h) return false;
    K.surprise = 1; K.glitch = 0.6; K.happy = 1;
    K.talk(U.pick(POKED));
    for (let i = 0; i < 6; i++) K.hearts.push({ x: x + U.rand(-20, 20), y, vx: U.rand(-40, 40), vy: U.rand(-160, -60), t: 0, c: U.chance(0.5) });
    return true;
  };
  K.talk = (text) => { K.say = { text, t: 0 }; };

  // ---------- user image ----------
  function userImage(cfg) {
    const src = cfg.girlFile ? 'girls/' + cfg.girlFile : (cfg.girlUrl || '').trim();
    if (src !== K.imgSrc) {
      K.imgSrc = src; K.imgOK = false; K.img = null;
      if (src) { const im = new Image(); im.crossOrigin = 'anonymous'; im.onload = () => { K.imgOK = true; }; im.onerror = () => { K.imgOK = false; }; im.src = src; K.img = im; }
    }
    return K.imgOK ? K.img : null;
  }

  // ---------- drawing helpers (local units: 1u = size/100, origin at the base of the neck) ----------
  function eye(c, x, y, side, P, iris, look, blink, surprise) {
    const open = Math.max(0.06, blink) * (1 + surprise * 0.15);
    c.save(); c.translate(x, y); c.scale(1, open);
    // white
    c.fillStyle = '#fbf7ff';
    c.beginPath(); c.ellipse(0, 0, 5.2, 6.2, 0, 0, U.TAU); c.fill();
    c.save(); c.beginPath(); c.ellipse(0, 0, 5.2, 6.2, 0, 0, U.TAU); c.clip();
    // iris
    const ix = look[0] * 1.6, iy = look[1] * 1.2 + 0.4;
    const g = c.createLinearGradient(0, iy - 5.5, 0, iy + 5.5);
    g.addColorStop(0, U.rgba(U.scalec(iris, 0.25))); g.addColorStop(0.55, U.rgba(iris)); g.addColorStop(1, U.rgba(U.mixc(iris, [255, 255, 255], 0.55)));
    c.fillStyle = g; c.beginPath(); c.ellipse(ix, iy, 4.1, 5.4, 0, 0, U.TAU); c.fill();
    // digital iris ring
    c.strokeStyle = U.rgba(U.mixc(iris, [255, 255, 255], 0.7), 0.7); c.lineWidth = 0.35;
    c.beginPath(); c.ellipse(ix, iy, 2.9, 3.9, 0, 0.3, 5.2); c.stroke();
    c.fillStyle = '#100818'; c.beginPath(); c.ellipse(ix, iy, 1.7 - surprise * 0.6, 2.4 - surprise * 0.8, 0, 0, U.TAU); c.fill();
    // highlights
    c.fillStyle = '#fff';
    c.beginPath(); c.ellipse(ix - 1.6 * side + 0.2, iy - 2.2, 1.35, 1.6, 0, 0, U.TAU); c.fill();
    c.beginPath(); c.arc(ix + 1.5, iy + 2.3, 0.6, 0, U.TAU); c.fill();
    c.restore();
    // upper lash line with a little wing
    c.strokeStyle = '#1a1024'; c.lineWidth = 1.5; c.lineCap = 'round';
    c.beginPath(); c.moveTo(-5.6, 0.6); c.quadraticCurveTo(-1, -7.6, 5.6 , -1.2); c.lineTo(6.8 * 1, -3.2); c.stroke();
    c.restore();
    if (blink < 0.2) { // closed: a happy arc
      c.strokeStyle = '#1a1024'; c.lineWidth = 1.3;
      c.beginPath(); c.arc(x, y + 1, 4.5, Math.PI * 1.1, Math.PI * 1.9); c.stroke();
    }
  }

  function drawKei(c, F, P, t) {
    const cfg = F.cfg, A = F.A;
    const hair = HAIR[cfg.girlHair] || (cfg.girlHair === 0 ? P.a : P.b);
    const hairDark = U.scalec(hair, 0.55), hairLight = U.mixc(hair, [255, 255, 255], 0.45);
    const skin = [255, 226, 214], skinShade = [240, 190, 186];
    const iris = cfg.girlHair === 1 ? P.a : P.b;
    const sway = Math.sin(t * 1.3) * 2 + F.I.vx * 0.0006 + A.bass * 2;
    const bob = A.live ? Math.sin(t * 9) * A.bass * 1.2 : 0;
    const breath = Math.sin(t * 1.7) * 0.6;
    const tilt = K.lx * K.fs * 0.09 + Math.sin(t * 0.7) * 0.025;

    // ---- back hair ----
    c.save(); c.translate(0, breath);
    c.save(); c.translate(0, -38 + bob); c.rotate(tilt); c.translate(0, 38);
    c.fillStyle = U.rgba(hairDark);
    c.beginPath();
    c.moveTo(-21, -52);
    c.bezierCurveTo(-30, -30, -27 + sway * 0.5, -2, -31 + sway, 22);
    c.lineTo(-22 + sway, 14); c.lineTo(-24 + sway, 28); c.lineTo(-14 + sway * 0.6, 12);
    c.lineTo(14 + sway * 0.6, 12); c.lineTo(24 + sway, 28); c.lineTo(22 + sway, 14);
    c.lineTo(31 + sway, 22);
    c.bezierCurveTo(27 + sway * 0.5, -2, 30, -30, 21, -52);
    c.closePath(); c.fill();
    c.restore();

    // ---- body: oversized bomber jacket ----
    const jacket = U.scalec(P.bg2, 0.6), trim = cfg.girlHair === 0 ? P.b : P.a;
    c.fillStyle = U.rgba(U.mixc(jacket, [30, 26, 44], 0.5));
    c.beginPath();
    c.moveTo(-7, -6); c.bezierCurveTo(-22, -4, -36, 2, -40, 18); c.lineTo(-46, 110); c.lineTo(46, 110); c.lineTo(40, 18);
    c.bezierCurveTo(36, 2, 22, -4, 7, -6); c.closePath(); c.fill();
    // shirt + glowing logo
    c.fillStyle = '#120a1c';
    c.beginPath(); c.moveTo(-8, -5); c.lineTo(8, -5); c.lineTo(12, 110); c.lineTo(-12, 110); c.closePath(); c.fill();
    c.font = 'bold 5px ' + U.font(5, 'display').split('px ')[1];
    c.textAlign = 'center'; c.fillStyle = U.rgba(P.c, 0.75 + 0.25 * Math.sin(t * 3));
    c.save(); c.scale(K.fs, 1); c.fillText('NULL', 0, 22); c.fillText('♡', 0, 29); c.restore();
    // neon trims
    c.strokeStyle = U.rgba(trim, 0.95); c.lineWidth = 1.1;
    c.beginPath(); c.moveTo(-8, -5); c.lineTo(-13, 110); c.moveTo(8, -5); c.lineTo(13, 110); c.stroke();
    c.beginPath(); c.moveTo(-40, 26); c.lineTo(-30, 30); c.moveTo(40, 26); c.lineTo(30, 30); c.stroke();
    // shoulder patch
    c.fillStyle = U.rgba(P.warn, 0.9); c.fillRect(-35, 12, 7, 5);
    c.fillStyle = '#000'; c.font = '3px monospace'; c.save(); c.translate(-31.5, 16); c.scale(K.fs, 1); c.fillText('07', 0, 0); c.restore();
    // collar fur
    c.fillStyle = U.rgba(U.mixc(jacket, [255, 255, 255], 0.25));
    c.beginPath(); c.moveTo(-8, -6); c.quadraticCurveTo(-20, -6, -24, 4); c.quadraticCurveTo(-14, 2, -9, 4); c.closePath(); c.fill();
    c.beginPath(); c.moveTo(8, -6); c.quadraticCurveTo(20, -6, 24, 4); c.quadraticCurveTo(14, 2, 9, 4); c.closePath(); c.fill();

    // ---- waving hand ----
    if (K.wave > 0) {
      const wa = Math.sin(t * 14) * 0.35;
      c.save(); c.translate(30, 18); c.rotate(-0.4 + wa);
      c.fillStyle = U.rgba(U.mixc(jacket, [30, 26, 44], 0.5)); c.fillRect(-5, -30 * K.wave, 10, 30 * K.wave);
      c.fillStyle = U.rgba(skin); c.beginPath(); c.arc(0, -32 * K.wave, 5, 0, U.TAU); c.fill();
      c.restore();
    }

    // ---- neck + choker ----
    c.fillStyle = U.rgba(skinShade); c.fillRect(-4.5, -19, 9, 15);
    c.fillStyle = '#16101e'; c.fillRect(-5, -11, 10, 2.6);
    c.fillStyle = U.rgba(P.c); c.beginPath(); c.arc(0, -8.6, 1.2, 0, U.TAU); c.fill();

    // ---- head ----
    c.save(); c.translate(0, -38 + bob); c.rotate(tilt); c.translate(0, 38);
    // face
    c.fillStyle = U.rgba(skin);
    c.beginPath();
    c.moveTo(-17.5, -44);
    c.bezierCurveTo(-18.5, -30, -9, -16, 0, -15);
    c.bezierCurveTo(9, -16, 18.5, -30, 17.5, -44);
    c.arc(0, -44, 17.5, 0, Math.PI, true);
    c.closePath(); c.fill();
    // blush
    const blush = 0.35 + K.happy * 0.4 + K.surprise * 0.3;
    c.fillStyle = `rgba(255,110,150,${blush * 0.5})`;
    c.beginPath(); c.ellipse(-10.5, -26.5, 3.6, 1.8, 0, 0, U.TAU); c.ellipse(10.5, -26.5, 3.6, 1.8, 0, 0, U.TAU); c.fill();
    c.strokeStyle = `rgba(230,80,120,${blush * 0.6})`; c.lineWidth = 0.4;
    for (const sx of [-1, 1]) for (let i = 0; i < 3; i++) { c.beginPath(); c.moveTo(sx * 9 + i * 1.4 - 1.4, -27.6); c.lineTo(sx * 9 + i * 1.4 - 2.2, -25.6); c.stroke(); }
    // eyes + brows
    const look = [K.lx * K.fs, K.ly];
    eye(c, -7.6, -32, -1, P, iris, look, K.blink, K.surprise);
    eye(c, 7.6, -32, 1, P, iris, look, K.blink, K.surprise);
    c.strokeStyle = U.rgba(hairDark); c.lineWidth = 0.9; c.lineCap = 'round';
    const by = -42 - K.surprise * 1.5;
    c.beginPath(); c.moveTo(-11, by + 0.5); c.quadraticCurveTo(-7.5, by - 1.2, -4.5, by + 0.3); c.moveTo(11, by + 0.5); c.quadraticCurveTo(7.5, by - 1.2, 4.5, by + 0.3); c.stroke();
    // under-eye cyber tattoo
    c.fillStyle = U.rgba(trim, 0.9);
    c.fillRect(11.5, -24.5, 0.7, 2.4); c.fillRect(12.6, -24.5, 0.7, 1.4); c.fillRect(13.7, -24.5, 0.7, 2);
    // mouth
    c.strokeStyle = '#7a2b44'; c.lineWidth = 0.8;
    const talking = K.say && K.say.t < K.say.text.length / 14;
    if (K.surprise > 0.3) { c.fillStyle = '#7a2b44'; c.beginPath(); c.ellipse(0, -21, 1.6, 2.1 * K.surprise, 0, 0, U.TAU); c.fill(); }
    else if (talking) { c.fillStyle = '#7a2b44'; c.beginPath(); c.ellipse(0, -21.5, 1.8, 0.5 + Math.abs(Math.sin(t * 20)) * 1.4, 0, 0, U.TAU); c.fill(); }
    else { c.beginPath(); c.moveTo(-2.4, -22); c.quadraticCurveTo(-1.2, -20.6, 0, -21.7); c.quadraticCurveTo(1.2, -20.6, 2.4, -22); c.stroke(); } // cat mouth "ω"
    // fang
    if (!talking && K.surprise < 0.3) { c.fillStyle = '#fff'; c.beginPath(); c.moveTo(1, -21.6); c.lineTo(1.6, -20.4); c.lineTo(2, -21.7); c.fill(); }

    // front hair: bangs and side locks
    c.fillStyle = U.rgba(hair);
    c.beginPath();
    c.moveTo(-19, -40); c.bezierCurveTo(-22, -62, 22, -66, 19, -40);
    const tips = [[-17, -30], [-13, -41], [-9, -35], [-5, -42], [-1, -36], [3, -43], [7, -36], [11, -41], [16, -31]];
    for (let i = tips.length - 1; i >= 0; i--) { const [x, y] = tips[i]; c.lineTo(x + Math.sin(t * 1.5 + i) * 0.4, y); c.lineTo(x - 2, y - 7); }
    c.closePath(); c.fill();
    // side locks
    for (const s of [-1, 1]) {
      c.beginPath();
      c.moveTo(s * 15, -46); c.bezierCurveTo(s * 22, -36, s * (20 + sway * 0.3), -22, s * (17 + sway * 0.4), -6);
      c.lineTo(s * (15 + sway * 0.3), -14); c.bezierCurveTo(s * 16, -26, s * 14, -36, s * 12, -42); c.closePath(); c.fill();
    }
    // hair shine
    c.strokeStyle = U.rgba(hairLight, 0.8); c.lineWidth = 1.2;
    c.beginPath(); c.arc(0, -46, 13, Math.PI * 1.15, Math.PI * 1.45); c.stroke();
    c.beginPath(); c.arc(0, -46, 13, Math.PI * 1.55, Math.PI * 1.7); c.stroke();
    // neon hair streak
    c.strokeStyle = U.rgba(trim, 0.9); c.lineWidth = 1;
    c.beginPath(); c.moveTo(-6, -58); c.quadraticCurveTo(-12, -48, -13, -41); c.stroke();
    // ahoge (antenna strand)
    c.strokeStyle = U.rgba(hair); c.lineWidth = 1.6;
    c.beginPath(); c.moveTo(1, -61); c.quadraticCurveTo(6 + sway, -72, 1 + sway * 1.5, -70); c.stroke();

    // accessories
    const acc = cfg.girlAcc;
    if (acc === 0 || acc === 2) { // cat-ear headphones
      const lvl = A.level;
      c.strokeStyle = '#1b1426'; c.lineWidth = 2.6;
      c.beginPath(); c.arc(0, -42, 21, Math.PI * 1.08, Math.PI * 1.92); c.stroke();
      for (const s of [-1, 1]) {
        // ears
        c.fillStyle = '#1b1426';
        c.beginPath(); c.moveTo(s * 9, -60); c.lineTo(s * 17, -72); c.lineTo(s * 19, -56); c.closePath(); c.fill();
        c.strokeStyle = U.rgba(trim, 0.9); c.lineWidth = 0.8;
        c.beginPath(); c.moveTo(s * 11, -60); c.lineTo(s * 16.5, -68.5); c.lineTo(s * 17.5, -58); c.stroke();
        // cups
        c.fillStyle = '#211a2e'; c.beginPath(); c.ellipse(s * 19.5, -36, 4.6, 6.5, 0, 0, U.TAU); c.fill();
        c.strokeStyle = U.rgba(trim, 0.6 + lvl * 0.4); c.lineWidth = 1 + lvl * 1.5;
        c.beginPath(); c.ellipse(s * 19.5, -36, 3.2, 4.8, 0, 0, U.TAU); c.stroke();
        c.fillStyle = U.rgba(P.c, 0.5 + 0.5 * Math.sin(t * 4 + s)); c.fillRect(s * 19.5 - 0.6, -37, 1.2, 2);
      }
    }
    if (acc === 1 || acc === 2) { // visor
      const vg = c.createLinearGradient(-20, 0, 20, 0);
      vg.addColorStop(0, U.rgba(P.a, 0.35)); vg.addColorStop(0.5, U.rgba(P.b, 0.25)); vg.addColorStop(1, U.rgba(P.a, 0.35));
      c.fillStyle = vg;
      c.beginPath(); c.moveTo(-19, -38); c.quadraticCurveTo(0, -42, 19, -38); c.lineTo(18, -28); c.quadraticCurveTo(0, -25, -18, -28); c.closePath(); c.fill();
      c.strokeStyle = U.rgba(P.b, 0.9); c.lineWidth = 0.6; c.stroke();
      const sy = -38 + ((t * 8) % 10);
      c.fillStyle = U.rgba([255, 255, 255], 0.35); c.fillRect(-18, sy, 36, 0.6);
      c.font = '2.4px monospace'; c.fillStyle = U.rgba(P.c, 0.9); c.textAlign = 'left';
      c.save(); c.translate(13, -36); c.scale(K.fs, 1); c.textAlign = 'center'; c.fillText('TRK ' + U.hex((F.mx * 7) | 0, 3), 0, 0); c.restore();
    }
    c.restore(); // head
    c.restore(); // breath

    // ---- umbrella ----
    if (cfg.girlUmbrella) {
      c.save(); c.translate(-6, 6); c.rotate(-0.18 + Math.sin(t * 0.8) * 0.03 + sway * 0.004);
      c.strokeStyle = '#ccc'; c.lineWidth = 1; c.beginPath(); c.moveTo(0, 30); c.lineTo(0, -92); c.stroke();
      const ug = c.createRadialGradient(0, -92, 4, 0, -92, 70);
      ug.addColorStop(0, U.rgba(P.a, 0.08)); ug.addColorStop(1, U.rgba(P.b, 0.28));
      c.fillStyle = ug;
      c.beginPath(); c.moveTo(-62, -66);
      for (let i = 0; i < 6; i++) { const x0 = -62 + (i * 124) / 6, x1 = x0 + 124 / 6; c.quadraticCurveTo((x0 + x1) / 2, -72, x1, -66); }
      c.quadraticCurveTo(0, -118, -62, -66); c.closePath(); c.fill();
      c.strokeStyle = U.rgba(P.b, 0.95); c.lineWidth = 1.2; c.stroke();
      c.strokeStyle = U.rgba(P.a, 0.4); c.lineWidth = 0.5;
      for (let i = 0; i <= 6; i++) { c.beginPath(); c.moveTo(0, -94); c.lineTo(-62 + (i * 124) / 6, -66); c.stroke(); }
      // drips off the rim
      c.fillStyle = U.rgba(P.b, 0.8);
      for (let i = 0; i < 7; i++) { const dy = ((t * 40 + i * 13) % 30); c.fillRect(-62 + (i * 124) / 6, -66 + dy, 0.6, 2); }
      c.restore();
    }
  }

  // ---------- update + draw ----------
  K.draw = (F) => {
    const cfg = F.cfg; if (!cfg.girl) { K.box = null; return; }
    const { ctx, W, H, P, dt, t } = F;
    const size = H * (0.35 + (cfg.girlSize / 100) * 0.6);
    const u = size / 100;
    // placement
    let side = cfg.girlPos === 0 ? -1 : cfg.girlPos === 1 ? 1 : (F.mx > W / 2 ? -1 : 1);
    const tx = side < 0 ? W * 0.2 : W * 0.8;
    K.x = K.x ? U.damp(K.x, tx, 2.5, dt) : tx;
    const nx = K.x - F.par * 1.1, ny = H - 42 * u - F.parY * 0.2;
    // look at the cursor (or at the Saint when idle)
    let ax = F.mx, ay = F.my;
    if (F.I.idle > 5 && cfg.saint && NS.saint.x) { ax = NS.saint.x; ay = NS.saint.y; }
    K.lx = U.damp(K.lx, U.clamp((ax - nx) / (W * 0.35), -1, 1), 6, dt);
    K.ly = U.damp(K.ly, U.clamp((ay - (ny - 32 * u)) / (H * 0.35), -1, 1), 6, dt);
    // blink
    K.blinkT -= dt;
    if (K.blinkT <= 0) { K.blinkT = U.rand(1.8, 5.5); K.blinkP = 0; }
    if (K.blinkP != null) { K.blinkP += dt * 9; K.blink = Math.abs(Math.cos(Math.min(K.blinkP, Math.PI))); if (K.blinkP >= Math.PI) { K.blinkP = null; K.blink = 1; } }
    if (K.happy > 0.6) K.blink = Math.min(K.blink, 0.1); // ^_^ eyes
    K.surprise = Math.max(0, K.surprise - dt * 1.5);
    K.happy = Math.max(0, K.happy - dt * 0.6);
    K.glitch = Math.max(0, K.glitch - dt * 2);
    if (U.chance(dt * 0.08 * F.gi)) K.glitch = 0.4;
    // wave when the cursor comes back or arrives near
    const near = U.dist(F.mx, F.my, nx, ny - 30 * u) < 140 * u;
    K.wave = U.damp(K.wave, near ? 1 : 0, 5, dt);
    // chatter
    if (cfg.girlTalk) {
      K.sayT -= dt;
      if (K.sayT <= 0 && !K.say) { K.sayT = U.rand(14, 30); K.talk(U.pick(LINES.concat([`hi ${(cfg.operator || 'operator').toLowerCase()} ♡`]))); }
    }
    if (K.say) { K.say.t += dt; if (K.say.t > K.say.text.length / 14 + 3) K.say = null; }

    // render into an offscreen canvas so she can be glitched as a sprite
    const bw = Math.ceil(150 * u), bh = Math.ceil(H - (ny - 125 * u));
    if (!K.cv || K.cv.width !== bw || K.cv.height !== bh) K.cv = U.makeCanvas(bw, bh);
    const c = K.cv.getContext('2d');
    c.setTransform(1, 0, 0, 1, 0, 0); c.clearRect(0, 0, bw, bh);
    const img = cfg.girlMode === 1 ? userImage(cfg) : null;
    if (img) {
      const k = Math.min(bw / img.width, bh / img.height);
      c.drawImage(img, (bw - img.width * k) / 2, bh - img.height * k, img.width * k, img.height * k);
    } else {
      K.fs = (side < 0 ? 1 : -1) * (cfg.girlFlip ? -1 : 1);
      c.setTransform(u * K.fs, 0, 0, u, bw / 2, 125 * u);
      drawKei(c, F, P, t);
      c.setTransform(1, 0, 0, 1, 0, 0);
    }
    const ox = nx - bw / 2, oy = ny - 125 * u;
    K.box = { x: ox, y: oy, w: bw, h: bh };

    // rim light from the city, then composite
    ctx.save();
    if (K.glitch > 0.05) {
      const slices = 10;
      for (let i = 0; i < slices; i++) {
        const sy = (bh * i) / slices, sh = bh / slices + 1;
        const dx = U.chance(0.4) ? U.rand(-30, 30) * u * K.glitch : 0;
        ctx.globalAlpha = 0.6; ctx.globalCompositeOperation = 'lighter';
        ctx.drawImage(K.cv, 0, sy, bw, sh, ox + dx - 4 * u * K.glitch, oy + sy, bw, sh);
        ctx.globalAlpha = 1; ctx.globalCompositeOperation = 'source-over';
        ctx.drawImage(K.cv, 0, sy, bw, sh, ox + dx, oy + sy, bw, sh);
      }
    } else ctx.drawImage(K.cv, ox, oy);
    ctx.restore();

    // name tag hologram
    ctx.font = U.font(Math.max(10, 4 * u), 'mono'); ctx.textAlign = 'center';
    ctx.fillStyle = U.rgba(P.b, 0.7);
    ctx.fillText(img ? '[ GUEST ]' : '[ KEI // NETRUNNER ]', nx, oy + 8 * u);

    // speech bubble
    if (K.say) {
      const txt = K.say.text.slice(0, Math.floor(K.say.t * 14));
      const fs = Math.max(12, 5.2 * u);
      ctx.font = U.font(fs, 'mono', 'bold');
      const w = ctx.measureText(K.say.text).width + fs * 1.4, h = fs * 1.9;
      let bx = nx - w / 2, by = oy - h * 0.2;
      bx = U.clamp(bx, 8, W - w - 8);
      ctx.fillStyle = 'rgba(255,255,255,0.92)'; U.roundRect(ctx, bx, by, w, h, h / 2); ctx.fill();
      ctx.strokeStyle = U.rgba(P.a); ctx.lineWidth = 2; ctx.stroke();
      ctx.fillStyle = '#2a1030'; ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
      ctx.fillText(txt, bx + fs * 0.7, by + h / 2);
      ctx.textBaseline = 'alphabetic';
    }

    // hearts
    for (let i = K.hearts.length - 1; i >= 0; i--) {
      const h = K.hearts[i]; h.t += dt; h.x += h.vx * dt; h.y += h.vy * dt; h.vy += 40 * dt;
      if (h.t > 1.6) { K.hearts.splice(i, 1); continue; }
      ctx.font = U.font(18 * u / 4 + 10, 'mono', 'bold');
      ctx.fillStyle = U.rgba(h.c ? P.a : [255, 120, 180], 1 - h.t / 1.6);
      ctx.fillText(U.chance(0.1) ? '♥' : '♡', h.x, h.y);
    }
  };
})(window.NS);
