'use strict';
/* NULL//SAINT - optional synthesized sound: a low city drone, glitch chirps, clicks and event stingers.
   Everything is generated with WebAudio; no audio files. Off by default. */
(function (NS) {
  const U = NS.U;
  const Snd = NS.sound = { ctx: null, master: null, drone: null, on: false, lastKick: 0 };

  function init() {
    if (Snd.ctx) return true;
    const AC = window.AudioContext || window.webkitAudioContext; if (!AC) return false;
    const c = Snd.ctx = new AC();
    Snd.master = c.createGain(); Snd.master.gain.value = 0;
    const comp = c.createDynamicsCompressor();
    Snd.master.connect(comp); comp.connect(c.destination);
    // noise buffer shared by chirps and static
    const len = c.sampleRate * 2, buf = c.createBuffer(1, len, c.sampleRate), d = buf.getChannelData(0);
    for (let i = 0; i < len; i++) d[i] = Math.random() * 2 - 1;
    Snd.noise = buf;
    // drone: two detuned saws through a slowly moving low-pass
    const g = c.createGain(); g.gain.value = 0.05;
    const f = c.createBiquadFilter(); f.type = 'lowpass'; f.frequency.value = 220; f.Q.value = 6;
    const lfo = c.createOscillator(), lg = c.createGain(); lfo.frequency.value = 0.07; lg.gain.value = 140;
    lfo.connect(lg); lg.connect(f.frequency); lfo.start();
    for (const fr of [41.2, 41.7, 82.1]) { const o = c.createOscillator(); o.type = 'sawtooth'; o.frequency.value = fr; o.connect(f); o.start(); }
    f.connect(g); g.connect(Snd.master);
    Snd.drone = { g, f };
    return true;
  }

  function env(node, t0, a, peak, rel) {
    node.gain.cancelScheduledValues(t0);
    node.gain.setValueAtTime(0.0001, t0);
    node.gain.exponentialRampToValueAtTime(peak, t0 + a);
    node.gain.exponentialRampToValueAtTime(0.0001, t0 + a + rel);
  }

  Snd.chirp = (pitch = 1, len = 0.08) => {
    if (!Snd.on) return;
    const c = Snd.ctx, t = c.currentTime;
    const o = c.createOscillator(), g = c.createGain();
    o.type = U.pick(['square', 'sawtooth', 'triangle']);
    o.frequency.setValueAtTime(U.rand(300, 2400) * pitch, t);
    o.frequency.exponentialRampToValueAtTime(U.rand(60, 4000) * pitch, t + len);
    env(g, t, 0.005, 0.12, len);
    o.connect(g); g.connect(Snd.master); o.start(t); o.stop(t + len + 0.05);
  };
  Snd.static = (len = 0.3, vol = 0.15) => {
    if (!Snd.on) return;
    const c = Snd.ctx, t = c.currentTime;
    const s = c.createBufferSource(); s.buffer = Snd.noise; s.playbackRate.value = U.rand(0.5, 1.5);
    const f = c.createBiquadFilter(); f.type = 'bandpass'; f.frequency.value = U.rand(800, 5000); f.Q.value = 0.8;
    const g = c.createGain(); env(g, t, 0.01, vol, len);
    s.connect(f); f.connect(g); g.connect(Snd.master); s.start(t, Math.random()); s.stop(t + len + 0.1);
  };
  Snd.boom = () => {
    if (!Snd.on) return;
    const c = Snd.ctx, t = c.currentTime;
    const o = c.createOscillator(), g = c.createGain();
    o.type = 'sine'; o.frequency.setValueAtTime(140, t); o.frequency.exponentialRampToValueAtTime(30, t + 0.6);
    env(g, t, 0.005, 0.5, 0.7);
    o.connect(g); g.connect(Snd.master); o.start(t); o.stop(t + 0.8);
    Snd.static(0.25, 0.2);
  };
  Snd.blip = (up = true) => {
    if (!Snd.on) return;
    const c = Snd.ctx, t = c.currentTime;
    [0, 0.07].forEach((dt, i) => {
      const o = c.createOscillator(), g = c.createGain(); o.type = 'sine';
      o.frequency.value = (up ? [880, 1320] : [1320, 880])[i];
      env(g, t + dt, 0.005, 0.08, 0.09);
      o.connect(g); g.connect(Snd.master); o.start(t + dt); o.stop(t + dt + 0.15);
    });
  };
  Snd.alarm = (dur = 2) => {
    if (!Snd.on) return;
    const c = Snd.ctx, t = c.currentTime;
    const o = c.createOscillator(), g = c.createGain(); o.type = 'square';
    for (let i = 0; i < dur * 4; i++) o.frequency.setValueAtTime(i % 2 ? 620 : 880, t + i * 0.25);
    g.gain.setValueAtTime(0.05, t); g.gain.setValueAtTime(0.0001, t + dur);
    o.connect(g); g.connect(Snd.master); o.start(t); o.stop(t + dur);
  };
  Snd.whisper = () => { Snd.static(1.2, 0.06); };

  Snd.update = (F) => {
    const cfg = F.cfg;
    const want = !!cfg.sound;
    if (want && !Snd.ctx) { if (!init()) return; }
    if (!Snd.ctx) return;
    if (want && Snd.ctx.state === 'suspended') Snd.ctx.resume().catch(() => {});
    Snd.on = want && Snd.ctx.state === 'running';
    const vol = want ? cfg.soundVol / 100 : 0;
    Snd.master.gain.setTargetAtTime(vol * 0.8, Snd.ctx.currentTime, 0.3);
    if (Snd.drone) {
      Snd.drone.g.gain.setTargetAtTime(cfg.drone ? 0.05 + F.gi * 0.02 : 0, Snd.ctx.currentTime, 0.5);
      Snd.drone.f.Q.setTargetAtTime(4 + F.gi * 6 + F.rot * 10, Snd.ctx.currentTime, 0.5);
    }
    if (!Snd.on) return;
    // ambient glitch chatter follows the glitch budget
    if (U.chance(F.dt * 1.2 * F.gi * (cfg.chaos / 100))) Snd.chirp(U.rand(0.5, 2), U.rand(0.03, 0.12));
    if (U.chance(F.dt * 0.15 * F.gi)) Snd.static(U.rand(0.05, 0.3), 0.08);
  };

  // browsers block audio until a gesture; the first click/key unlocks it
  const unlock = () => { if (NS.cfg.sound && init()) Snd.ctx.resume().catch(() => {}); };
  addEventListener('pointerdown', unlock); addEventListener('keydown', unlock);
})(window.NS);
