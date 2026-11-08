'use strict';
/* NULL//SAINT - mouse, clicks, keyboard, audio and system info */
(function (NS) {
  const U = NS.U;
  const I = NS.input = {
    x: innerWidth / 2, y: innerHeight / 2, // css px
    sx: innerWidth / 2, sy: innerHeight / 2, // smoothed
    vx: 0, vy: 0, speed: 0,
    down: false, inside: true,
    lastMove: performance.now(), idle: 0,
    clicks: [], // queued {x,y,button}
    keys: [], // queued key strings
    path: [], // recent positions for trails
    drag: null,
  };

  const onMove = (x, y) => {
    I.x = x; I.y = y; I.lastMove = performance.now(); I.inside = true;
  };
  addEventListener('pointermove', (e) => onMove(e.clientX, e.clientY), { passive: true });
  addEventListener('mousemove', (e) => onMove(e.clientX, e.clientY), { passive: true });
  addEventListener('pointerdown', (e) => {
    onMove(e.clientX, e.clientY); I.down = true;
    I.drag = { x: e.clientX, y: e.clientY, t: performance.now() };
    I.clicks.push({ x: e.clientX, y: e.clientY, button: e.button, t: performance.now() });
  });
  addEventListener('pointerup', () => { I.down = false; I.drag = null; });
  addEventListener('mouseleave', () => { I.inside = false; });
  addEventListener('contextmenu', (e) => e.preventDefault());
  addEventListener('keydown', (e) => {
    if (e.target && (e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT' || e.target.tagName === 'TEXTAREA')) return;
    I.keys.push(e.key);
    I.lastMove = performance.now();
  });

  I.update = (dt) => {
    const px = I.sx, py = I.sy;
    I.sx = U.damp(I.sx, I.x, 14, dt); I.sy = U.damp(I.sy, I.y, 14, dt);
    const ivx = (I.sx - px) / Math.max(dt, 1e-3), ivy = (I.sy - py) / Math.max(dt, 1e-3);
    I.vx = U.damp(I.vx, ivx, 10, dt); I.vy = U.damp(I.vy, ivy, 10, dt);
    I.speed = Math.hypot(I.vx, I.vy);
    I.idle = (performance.now() - I.lastMove) / 1000;
    I.path.push({ x: I.sx, y: I.sy, s: I.speed });
    if (I.path.length > 40) I.path.shift();
  };
  I.takeClicks = () => { const c = I.clicks; I.clicks = []; return c; };
  I.takeKeys = () => { const k = I.keys; I.keys = []; return k; };

  // ---------- audio ----------
  const A = NS.audio = {
    raw: new Float32Array(128), spec: new Float32Array(64),
    bass: 0, mid: 0, high: 0, level: 0, beat: 0, beatCount: 0,
    avgBass: 0, lastData: 0, live: false, kick: false,
  };
  window.livelyAudioListener = function (arr) {
    if (!arr || !arr.length) return;
    const n = Math.min(arr.length, 128);
    for (let i = 0; i < n; i++) A.raw[i] = Number(arr[i]) || 0;
    A.lastData = performance.now();
  };
  A.update = (dt, t) => {
    const cfg = NS.cfg;
    const fresh = performance.now() - A.lastData < 1500;
    A.live = fresh && cfg.audio;
    const gain = cfg.audioGain / 100;
    // fold L/R into 64 bins
    for (let i = 0; i < 64; i++) {
      let v = 0;
      if (A.live) v = Math.max(A.raw[i], A.raw[i + 64] || 0) * gain;
      else if (cfg.fakeBeat) {
        const beatT = U.fract(t * 2.0);
        v = (i < 8 ? Math.exp(-beatT * 8) * (1 - i / 10) : 0) + 0.15 * U.noise2(i * 0.3, t * 2) * (1 - i / 64);
      }
      A.spec[i] = U.damp(A.spec[i], U.clamp(v, 0, 1.5), v > A.spec[i] ? 30 : 8, dt);
    }
    let b = 0, m = 0, h = 0;
    for (let i = 0; i < 6; i++) b += A.spec[i];
    for (let i = 6; i < 24; i++) m += A.spec[i];
    for (let i = 24; i < 64; i++) h += A.spec[i];
    b /= 6; m /= 18; h /= 40;
    A.bass = b; A.mid = m; A.high = h;
    A.level = U.damp(A.level, (b + m + h) / 3, 6, dt);
    A.avgBass = U.damp(A.avgBass, b, 1.5, dt);
    A.kick = false;
    if (b > 0.25 && b > A.avgBass * 1.45 && A.beat < 0.3) { A.beat = 1; A.beatCount++; A.kick = true; }
    A.beat = Math.max(0, A.beat - dt * 3.5);
  };

  // ---------- system info ----------
  const SYS = NS.sys = { cpu: null, gpu: null, ram: null, ramTotal: null, net: null, name: '', live: false };
  window.livelySystemInformation = function (data) {
    try {
      const o = typeof data === 'string' ? JSON.parse(data) : data;
      SYS.live = true;
      SYS.cpu = num(o.CurrentCpu);
      SYS.gpu = num(o.CurrentGpu3D);
      SYS.ram = num(o.CurrentRamAvail);
      SYS.ramTotal = num(o.TotalRam);
      SYS.net = num(o.CurrentNetDown);
      SYS.name = o.NameCpu || '';
    } catch (e) { /* ignore */ }
  };
  const num = (v) => (v == null || isNaN(Number(v)) ? null : Number(v));
  SYS.fake = { cpu: 30, gpu: 40, ram: 60 };
  SYS.update = (dt, t) => {
    const f = SYS.fake;
    f.cpu = U.clamp(f.cpu + U.gauss() * 8 * dt * 10 * 0.2, 3, 99);
    f.gpu = U.clamp(f.gpu + U.gauss() * 8 * dt * 10 * 0.2, 5, 99);
    f.ram = U.clamp(f.ram + U.gauss() * dt * 2, 20, 95);
  };
  SYS.cpuPct = () => (SYS.cpu != null ? SYS.cpu : SYS.fake.cpu);
  SYS.gpuPct = () => (SYS.gpu != null ? SYS.gpu : SYS.fake.gpu);
  SYS.ramPct = () => (SYS.ram != null && SYS.ramTotal ? 100 * (1 - SYS.ram / SYS.ramTotal) : SYS.fake.ram);
})(window.NS);
