'use strict';
/* NULL//SAINT - the director: glitch budget, random events, beat hits, idle rot, boot sequence */
(function (NS) {
  const U = NS.U;
  const D = NS.director = { active: [], cool: 8, burst: 0, burstSeed: 0, rot: 0, wasRotten: false, boot: null, history: [] };

  // fresh per-frame glitch state, filled by base settings + events
  D.fresh = () => ({
    rgb: 0, scan: 0, curve: 0, grain: 0, blocks: 0, tear: 0, vhs: 0, mosh: 0, melt: 0, sort: 0, bloom: 0, vig: 0, wave: 0,
    pix: 0, post: 0, invert: 0, flash: [1, 1, 1, 0], shuffle: 0, ascii: 0, colorMode: -1, mirror: -1, static: 0, bars: 0,
    zoom: 0, swirl: 0, freeze: 0, hue: 0, banner: null, hudShake: 0, seedJump: 0, lens: 0,
  });

  // ---------- event catalogue ----------
  const EV = D.EVENTS = [
    {
      id: 'breach', key: 'evBreach', w: 3, dur: [4, 6], name: 'SECURITY BREACH',
      start(F, e) { for (let i = 0; i < 3; i++) setTimeout(() => NS.hud.spawnPopup(F.cssW, F.cssH, null, null, U.pick(['INTRUSION DETECTED', 'FIREWALL ON FIRE', 'SOMEONE IS INSIDE THE WALLPAPER', 'ACCESS GRANTED TO: IT'])), i * 400); NS.saint.say('I AM INSIDE'); },
      update(F, e, k, G) {
        const p = Math.sin(k * Math.PI);
        G.blocks += 0.6 * p; G.tear += 0.5 * p; G.rgb += 0.6 * p; G.hudShake = p;
        G.banner = { text: 'SECURITY BREACH', sub: 'SECTOR ' + U.hex(e.seed, 4) + ' COMPROMISED // DO NOT PANIC // PANIC', a: Math.min(1, k * 6, (1 - k) * 6), c: F.P.warn };
        if (!F.cfg.safe) G.flash = [F.P.warn[0] / 255, F.P.warn[1] / 255, F.P.warn[2] / 255, Math.max(0, Math.sin(F.t * 8)) * 0.12 * p];
      },
    },
    {
      id: 'melt', key: 'evMelt', w: 2, dur: [6, 9], name: 'MELTDOWN',
      update(F, e, k, G) {
        const p = Math.sin(k * Math.PI);
        G.melt += p * 0.9; G.sort += p * 0.6;
        if (k < 0.25) G.banner = { text: 'THERMAL EVENT', sub: 'THE CITY IS MELTING. THIS IS NORMAL.', a: Math.min(1, k * 8, (0.25 - k) * 8), c: F.P.c };
      },
    },
    {
      id: 'shuffle', key: 'evShuffle', w: 3, dur: [1.5, 3], name: 'TILE SHUFFLE',
      update(F, e, k, G) { G.shuffle = 0.55; G.seedJump = Math.floor(F.t * 5); G.rgb += 0.3; },
    },
    {
      id: 'invert', key: 'evInvert', w: 2, dur: [0.8, 1.5], unsafe: true, name: 'NEGATIVE',
      update(F, e, k, G) { G.invert = Math.floor(F.t * 8) % 2 ? 1 : 0; },
    },
    {
      id: 'panic', key: 'evPanic', w: 1, dur: [3, 4.5], name: 'KERNEL PANIC',
      start(F, e) { e.code = '0x' + U.hex(U.randi(0, 0xffffffff), 8); e.pct = 0; },
      draw(F, e, k) {
        const { ctx, W, H, P } = F;
        if (k > 0.92) return;
        e.pct = Math.min(100, e.pct + F.dt * U.rand(5, 60));
        ctx.fillStyle = U.rgba(U.mixc(P.warn, [0, 0, 0], 0.55), 1); ctx.fillRect(0, 0, W, H);
        const s = F.k;
        ctx.fillStyle = U.rgba([255, 255, 255], 1); ctx.textAlign = 'left'; ctx.textBaseline = 'top';
        ctx.font = U.font(110 * s, 'display', '900'); ctx.fillText(':(', W * 0.12, H * 0.16);
        ctx.font = U.font(30 * s, 'display', '600');
        const lines = ['Your reality ran into a problem and needs to restart.', 'We are collecting some of your memories, and then', 'we will forget them for you.'];
        lines.forEach((l, i) => ctx.fillText(l, W * 0.12, H * 0.36 + i * 42 * s));
        ctx.font = U.font(30 * s, 'display', '600');
        ctx.fillText(`${e.pct | 0}% forgotten`, W * 0.12, H * 0.36 + 150 * s);
        ctx.font = U.font(16 * s, 'mono');
        ctx.fillText('Stop code: REALITY_EXCEPTION_NOT_HANDLED  ' + e.code, W * 0.12, H * 0.36 + 220 * s);
        ctx.fillText('What failed: you.sys', W * 0.12, H * 0.36 + 246 * s);
        U.qrish(ctx, W * 0.12, H * 0.36 + 290 * s, 110 * s, 21, 666);
      },
      update(F, e, k, G) { G.scan += 0.3; G.grain += 0.2; G.curve += 0.2; if (k > 0.9 && !F.cfg.safe) G.flash = [1, 1, 1, (1 - k) * 4]; },
    },
    {
      id: 'worm', key: 'evWorm', w: 3, dur: [0.5, 0.5], name: 'WORM',
      start(F) { const n = U.randi(1, 3); for (let i = 0; i < n; i++) NS.fx.worm(F); },
    },
    {
      id: 'ascii', key: 'evAscii', w: 2, dur: [4, 7], name: 'ASCII COLLAPSE', asciiEvent: true,
      update(F, e, k, G) { G.ascii = Math.max(G.ascii, U.smooth(0, 0.15, k) * (1 - U.smooth(0.85, 1, k))); },
    },
    {
      id: 'blackout', key: 'evBlackout', w: 1, dur: [3.5, 5], name: 'BLACKOUT',
      start(F) { NS.saint.say(U.pick(['I SEE YOU', 'THERE YOU ARE', 'DONT TURN AROUND'])); },
      draw(F, e, k) {
        const { ctx, W, H, P } = F;
        const a = Math.min(1, k * 5, (1 - k) * 5);
        ctx.fillStyle = U.rgba([0, 0, 0], 0.97 * a); ctx.fillRect(0, 0, W, H);
        // two eyes in the dark, where the Saint is (or right behind the cursor)
        const sx = F.cfg.saint ? NS.saint.x : F.mx, sy = F.cfg.saint ? NS.saint.y : F.my - 120 * F.k;
        const R = (NS.saint.R || 200) * 0.32;
        for (const sd of [-1, 1]) {
          const ex = sx + sd * R, ey = sy - R * 0.2;
          const g = ctx.createRadialGradient(ex, ey, 0, ex, ey, R * 0.7);
          g.addColorStop(0, U.rgba(P.warn, a)); g.addColorStop(0.15, U.rgba(P.warn, 0.6 * a)); g.addColorStop(1, U.rgba(P.warn, 0));
          ctx.fillStyle = g; ctx.fillRect(ex - R, ey - R, R * 2, R * 2);
          ctx.fillStyle = U.rgba([255, 255, 255], a * NS.saint.blink); ctx.beginPath(); ctx.ellipse(ex + (F.mx - ex) * 0.01, ey, R * 0.12, R * 0.12 * NS.saint.blink + 0.5, 0, 0, U.TAU); ctx.fill();
        }
        ctx.font = U.font(18 * F.k, 'mono', 'bold'); ctx.fillStyle = U.rgba(P.warn, a * (0.6 + 0.4 * Math.random()));
        ctx.textAlign = 'center'; ctx.fillText(U.zalgo('I SEE YOU', 2), F.mx, F.my + 40 * F.k);
      },
      update(F, e, k, G) { G.grain += 0.3; G.vig += 0.5; },
    },
    {
      id: 'takeover', key: 'evTakeover', w: 1, dur: [6, 8], name: 'SAINT TAKEOVER',
      start(F) { NS.saint.say(U.pick(['THIS SCREEN BELONGS TO ME', 'LOOK AT ME', 'I AM THE WALLPAPER NOW', 'KNEEL'])); NS.saint.reroll(); },
      update(F, e, k, G) {
        const p = Math.sin(k * Math.PI);
        NS.saint.takeover = F.cfg.saint ? p : 0;
        G.swirl += p * 0.6; G.zoom += p * 0.05; G.rgb += p * 0.4; G.mosh += p * 0.3;
      },
      end() { NS.saint.takeover = 0; },
    },
    {
      id: 'signal', key: 'evSignal', w: 2, dur: [1.5, 3], name: 'SIGNAL LOST',
      update(F, e, k, G) {
        const p = Math.min(1, k * 8, (1 - k) * 8);
        G.static = Math.max(G.static, p * (k < 0.5 ? 0.85 : 0.3)); G.bars = k >= 0.5 ? p * 0.9 : 0; G.vhs += p;
      },
      draw(F, e, k) {
        const { ctx, W, H, P } = F;
        const p = Math.min(1, k * 8, (1 - k) * 8);
        ctx.font = U.font(48 * F.k, 'display', '900'); ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
        ctx.fillStyle = U.rgba([255, 255, 255], p);
        ctx.fillText(k < 0.5 ? 'NO SIGNAL' : 'CH 00 // ' + U.pick(U.TEXT.whispers), W / 2, H / 2);
      },
    },
    {
      id: 'mirror', key: 'evMirror', w: 2, dur: [4, 6], name: 'KALEIDOSCOPE',
      update(F, e, k, G) { G.mirror = k < 0.08 || k > 0.92 ? -1 : 3; G.hue += Math.sin(k * Math.PI) * 1.5; },
    },
    {
      id: 'freeze', key: 'evFreeze', w: 2, dur: [1.5, 3], name: 'TIME FREEZE',
      update(F, e, k, G) {
        G.freeze = k < 0.85 ? 0.985 : 0; G.grain += 0.2;
        G.banner = k < 0.85 ? { text: '▌▌ PAUSED BY SOMETHING', a: 0.6 + 0.4 * Math.sin(F.t * 10), c: F.P.b } : null;
      },
    },
  ];
  const byId = {}; EV.forEach((e) => (byId[e.id] = e));

  D.trigger = (F, id) => {
    const def = typeof id === 'string' ? byId[id] : id; if (!def) return;
    if (def.unsafe && F.cfg.safe) return;
    if (D.active.some((a) => a.def === def)) return;
    const e = { def, t: 0, dur: U.rand(def.dur[0], def.dur[1]), seed: U.randi(0, 65535) };
    D.active.push(e);
    if (def.start) def.start(F, e);
    const S = NS.sound;
    if (S) ({ breach: () => S.alarm(2.5), panic: () => S.boom(), signal: () => S.static(e.dur, 0.25), blackout: () => S.whisper(), takeover: () => { S.boom(); S.whisper(); }, invert: () => S.chirp(0.3, 0.4) }[def.id] || (() => S.chirp(1, 0.2)))();
    D.history.push(def.name); if (D.history.length > 6) D.history.shift();
    NS.hud.logs.push({ s: `[${F.t.toFixed(3).padStart(9, '0')}] EVENT ${def.name}`, t: 0 });
  };
  D.randomEvent = (F) => {
    const pool = EV.filter((e) => F.cfg[e.key] && !(e.unsafe && F.cfg.safe) && !D.active.some((a) => a.def === e));
    if (!pool.length) return;
    let sum = 0; for (const e of pool) sum += e.w;
    let r = Math.random() * sum;
    for (const e of pool) { r -= e.w; if (r <= 0) { D.trigger(F, e); return; } }
  };
  D.kick = (amount) => { D.burst = Math.min(1.5, D.burst + amount); D.burstSeed = Math.random() * 100; };

  // ---------- boot ----------
  const BOOT = [
    'NULL//SAINT BIOS 6.6.6  (C) NOBODY, ALL RIGHTS DISSOLVED',
    'CPU: 1x SOUL @ UNKNOWN GHZ ............ OK',
    'MEMORY TEST: 640K OK  32G OK  ∞ FAIL',
    'DETECTING SKY ......................... CRACKED',
    'MOUNTING /city ON /dev/dream ........... OK',
    'LOADING NEON DRIVERS ................... OK',
    'CALIBRATING RAIN ....................... 0x' + U.hex(0xbeef, 4),
    'SUMMONING SAINT [##########----] 71%',
    'WARNING: SOMETHING IS ALREADY WATCHING',
    'HANDING CONTROL TO OPERATOR ...',
  ];
  D.startBoot = () => { D.boot = { t: 0 }; };
  D.drawBoot = (F) => {
    const b = D.boot; if (!b) return false;
    b.t += F.dt;
    const { ctx, W, H, P } = F;
    const T = 5.2;
    if (b.t > T) { D.boot = null; F.flash(F.cfg.safe ? 0.1 : 0.7, P.text); D.kick(1.2); NS.fx.shock(W / 2, H / 2, 1.2); NS.saint.say('HELLO, ' + (F.cfg.operator || 'OPERATOR').toUpperCase()); return false; }
    const fade = U.smooth(T - 0.6, T, b.t);
    ctx.fillStyle = U.rgba([0, 0, 0], 1 - fade * 0.7); ctx.fillRect(0, 0, W, H);
    const s = F.k, lh = 22 * s;
    ctx.font = U.font(15 * s, 'mono'); ctx.textAlign = 'left'; ctx.textBaseline = 'top';
    const shown = Math.floor(b.t / 0.28);
    for (let i = 0; i < Math.min(shown, BOOT.length); i++) {
      const line = BOOT[i];
      const warn = line.startsWith('WARNING') || line.includes('FAIL') || line.includes('CRACKED');
      ctx.fillStyle = U.rgba(warn ? P.warn : P.b, 1 - fade);
      ctx.fillText(i === shown - 1 ? U.decode(line, (b.t % 0.28) / 0.2) : line, 40 * s, 40 * s + i * lh);
    }
    if (Math.floor(b.t * 3) % 2) { ctx.fillStyle = U.rgba(P.b, 1 - fade); ctx.fillRect(40 * s, 40 * s + Math.min(shown, BOOT.length) * lh, 10 * s, 16 * s); }
    if (b.t > 3) {
      const k = U.smooth(3, 3.8, b.t);
      ctx.font = U.font(Math.min(W / 7, 160 * s), 'display', '900'); ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
      U.glitchText(ctx, U.decode('NULL//SAINT', k), W / 2, H / 2, P.a, P.b, 4 + Math.random() * 10, (1 - fade));
      ctx.font = U.font(16 * s, 'mono'); ctx.fillStyle = U.rgba(P.text, (1 - fade) * k);
      ctx.fillText(U.decode('GHOST SIGNAL ESTABLISHED // YOU ARE NOT ALONE', k), W / 2, H / 2 + Math.min(W / 7, 160 * s) * 0.6);
    }
    return true;
  };

  // ---------- per-frame ----------
  D.update = (F) => {
    const cfg = F.cfg, dt = F.dt, I = NS.input, A = NS.audio;
    const G = D.fresh();
    const k = cfg.intensity / 100;

    // idle rot
    if (cfg.idleRot) {
      const t0 = cfg.idleTime;
      const target = U.smooth(t0, t0 * 3, I.idle);
      D.rot = target > D.rot ? U.damp(D.rot, target, 0.3, dt) : U.damp(D.rot, target, 8, dt);
      if (D.rot > 0.4) D.wasRotten = true;
      if (D.wasRotten && I.idle < 0.5) {
        D.wasRotten = false;
        F.flash(cfg.safe ? 0.1 : 0.6, F.P.text); D.kick(1);
        NS.saint.say('WELCOME BACK, ' + (cfg.operator || 'OPERATOR').toUpperCase());
      }
    } else D.rot = 0;
    F.rot = D.rot;

    // random mini bursts + beat hits
    const chaos = cfg.chaos / 100;
    if (U.chance(dt * chaos * 0.9 * k)) D.kick(U.rand(0.2, 0.8));
    if (cfg.beatGlitch && A.kick) D.kick(0.25 + A.bass * 0.5);
    D.burst = Math.max(0, D.burst - dt * 2.8);
    const bu = D.burst;

    // events
    if (cfg.events && !D.boot) {
      D.cool -= dt * (0.15 + chaos * 1.6) * (1 + D.rot * 2);
      if (D.cool <= 0) { D.cool = U.rand(8, 20); D.randomEvent(F); }
    }

    const gi = F.gi = k * (1 + D.rot * 1.5) + bu;
    // base signal damage from settings
    const v = (key) => cfg[key] / 100;
    G.rgb = v('rgb') * (0.3 + gi * 0.7) + bu * 0.6;
    G.scan = v('scan');
    G.curve = v('curve');
    G.grain = v('grain') * (0.5 + gi * 0.5) + D.rot * 0.4;
    G.blocks = v('blocks') * gi + bu * 0.7;
    G.tear = v('tear') * gi + bu * 0.6;
    G.vhs = v('vhs') * (0.4 + gi * 0.6);
    G.mosh = v('mosh') * gi * 0.8 + D.rot * 0.5 + bu * 0.2;
    G.sort = v('sort') * gi + D.rot * 0.3;
    G.bloom = v('bloom') * (1 + A.bass * 0.6);
    G.vig = v('vignette');
    G.wave = v('wave') * (0.5 + gi * 0.5) + bu * 0.2;
    G.pix = v('pixelate') + D.rot * 0.15;
    G.post = v('posterize');
    G.lens = v('lens');
    G.ascii = cfg.ascii === 2 ? 1 : 0;
    G.hue = (F.t * cfg.hueCycle * 0.004) % U.TAU;

    for (let i = D.active.length - 1; i >= 0; i--) {
      const e = D.active[i];
      e.t += dt;
      const p = U.clamp(e.t / e.dur, 0, 1);
      if (e.def.update) e.def.update(F, e, p, G);
      if (e.t >= e.dur) { D.active.splice(i, 1); if (e.def.end) e.def.end(F, e); }
    }
    if (cfg.ascii === 0) G.ascii = 0;

    // safety: cap flicker
    if (cfg.safe) {
      G.invert = 0; G.flash[3] = Math.min(G.flash[3], 0.06);
      G.blocks *= 0.4; G.tear *= 0.4; G.shuffle = 0; G.static *= 0.3;
    }
    D.G = F.G = G;
    return G;
  };

  // event overlays drawn on the scene (after fx)
  D.draw = (F) => {
    for (const e of D.active) if (e.def.draw) e.def.draw(F, e, U.clamp(e.t / e.dur, 0, 1));
  };

  // keyboard triggers in the browser
  D.keys = (F, keys) => {
    if (!F.cfg.keys) return;
    for (const key of keys) {
      const n = '1234567890-='.indexOf(key);
      if (n >= 0 && EV[n]) D.trigger(F, EV[n]);
      else if (key === ' ') { D.kick(1.2); NS.fx.click(F, F.mx, F.my); }
      else if (key === 'b' || key === 'B') D.startBoot();
      else if (key === 'h' || key === 'H') NS.set('hud', !F.cfg.hud);
      else if (key === 's' || key === 'S') NS.saint.say(U.pick(U.TEXT.whispers));
      else if (key === 'w' || key === 'W') NS.fx.worm(F);
    }
  };
})(window.NS);
