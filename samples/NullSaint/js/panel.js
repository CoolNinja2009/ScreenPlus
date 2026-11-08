'use strict';
/* NULL//SAINT - in-page control deck (press P or ` in a browser; the wallpaper host has its own Customise page) */
(function (NS) {
  const U = NS.U;
  const Pn = NS.panel = { open: false, el: null, inputs: {}, hintT: 7 };

  function el(tag, cls, text) { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; }

  function build() {
    const root = el('aside', 'deck'); root.setAttribute('aria-label', 'NULL//SAINT settings');
    root.hidden = true;
    const head = el('header', 'deck-head');
    const title = el('h1', 'deck-title', 'NULL//SAINT'); title.setAttribute('data-text', 'NULL//SAINT');
    const sub = el('p', 'deck-sub', 'CONTROL DECK // ' + NS.SCHEMA.filter((e) => e.t !== 'h').length + ' PARAMETERS');
    const close = el('button', 'deck-x', '×'); close.setAttribute('aria-label', 'Close settings'); close.onclick = () => Pn.toggle(false);
    head.append(title, sub, close);
    const search = el('input', 'deck-search'); search.type = 'search'; search.placeholder = 'filter parameters…'; search.setAttribute('aria-label', 'Filter settings');
    const body = el('div', 'deck-body');
    let group = null, groupBody = null;
    for (const e of NS.SCHEMA) {
      if (e.t === 'h') {
        group = el('details', 'deck-group'); group.open = e.k === 'h_core';
        const sm = el('summary', 'deck-gtitle', e.text.replace(/▓/g, '').trim());
        groupBody = el('div', 'deck-gbody');
        group.append(sm, groupBody); body.append(group);
        continue;
      }
      const row = el('label', 'deck-row'); row.dataset.k = e.k; row.dataset.text = e.text.toLowerCase();
      const name = el('span', 'deck-name', e.text);
      let input, out = null;
      if (e.t === 's') {
        input = el('input'); input.type = 'range'; input.min = e.min; input.max = e.max; input.step = e.step;
        out = el('output', 'deck-val');
        input.oninput = () => { out.textContent = input.value; NS.set(e.k, Number(input.value)); };
      } else if (e.t === 'c') {
        input = el('input'); input.type = 'checkbox';
        input.onchange = () => NS.set(e.k, input.checked);
        row.classList.add('is-check');
      } else if (e.t === 'd') {
        input = el('select');
        e.items.forEach((it, i) => { const o = el('option', null, it); o.value = i; input.append(o); });
        input.onchange = () => { NS.set(e.k, Number(input.value)); if (e.k === 'preset') sync(); };
      } else if (e.t === 'col') {
        input = el('input'); input.type = 'color';
        input.oninput = () => NS.set(e.k, input.value);
      } else {
        input = el('input'); input.type = 'text'; input.spellcheck = false;
        input.onchange = () => NS.set(e.k, input.value);
      }
      input.className = (input.className ? input.className + ' ' : '') + 'deck-in';
      Pn.inputs[e.k] = { input, out, e };
      row.append(name, input); if (out) row.append(out);
      (groupBody || body).append(row);
    }
    const foot = el('footer', 'deck-foot');
    const bReset = el('button', 'deck-btn', 'RESET'); bReset.onclick = () => { NS.reset(); sync(); };
    const bRand = el('button', 'deck-btn', 'RANDOMISE'); bRand.onclick = () => { randomise(); sync(); };
    const bEv = el('button', 'deck-btn hot', 'TRIGGER EVENT'); bEv.onclick = () => NS.director.randomEvent(NS.F);
    const bCopy = el('button', 'deck-btn', 'COPY LINK'); bCopy.onclick = () => copyLink(bCopy);
    foot.append(bReset, bRand, bEv, bCopy);
    const keys = el('p', 'deck-keys', 'KEYS: 1–0 - = events · SPACE burst · S speak · W worm · B reboot · H hud · P deck');
    root.append(head, search, body, foot, keys);
    search.oninput = () => {
      const q = search.value.trim().toLowerCase();
      for (const row of body.querySelectorAll('.deck-row')) row.hidden = q && !row.dataset.text.includes(q) && !row.dataset.k.toLowerCase().includes(q);
      if (q) for (const g of body.querySelectorAll('details')) g.open = true;
    };
    root.addEventListener('pointerdown', (ev) => ev.stopPropagation());
    root.addEventListener('keydown', (ev) => { if (ev.key === 'Escape') Pn.toggle(false); ev.stopPropagation(); });
    document.body.append(root);
    Pn.el = root;
    sync();
  }

  function sync() {
    for (const k in Pn.inputs) {
      const { input, out, e } = Pn.inputs[k], v = NS.cfg[k];
      if (e.t === 'c') input.checked = !!v; else input.value = v;
      if (out) out.textContent = v;
    }
  }
  Pn.sync = sync;

  function randomise() {
    const keep = new Set(['preset', 'quality', 'fps', 'boot', 'seed', 'operator', 'messages', 'saintWords', 'glyphCustom', 'safe', 'keys', 'audio', 'hud', 'hudScale', 'pixelate', 'posterize', 'mirror', 'colorMode', 'idleTime']);
    for (const e of NS.SCHEMA) {
      if (e.t === 'h' || keep.has(e.k)) continue;
      if (e.t === 's') NS.set(e.k, Math.round(U.rand(e.min, e.min + (e.max - e.min) * 0.8) / e.step) * e.step, true);
      else if (e.t === 'c') NS.set(e.k, U.chance(0.7), true);
      else if (e.t === 'd') NS.set(e.k, U.randi(0, e.items.length - 1), true);
      else if (e.t === 'col') NS.set(e.k, U.rgb2hex(U.hsl(Math.random(), 1, 0.55)), true);
    }
    NS.set('seed', Math.random().toString(36).slice(2, 10).toUpperCase(), true);
    NS.set('theme', U.randi(0, NS.PALETTES.length - 1), true);
    NS.notify();
  }
  function copyLink(btn) {
    const q = new URLSearchParams();
    for (const k in NS.cfg) if (NS.cfg[k] !== NS.DEFAULTS[k] && k !== 'preset') q.set(k, NS.cfg[k]);
    const url = location.href.split('?')[0] + '?' + q.toString();
    try { navigator.clipboard.writeText(url); btn.textContent = 'COPIED'; } catch (e) { btn.textContent = 'FAILED'; }
    setTimeout(() => (btn.textContent = 'COPY LINK'), 1200);
  }

  Pn.toggle = (force) => {
    if (!Pn.el) build();
    Pn.open = force != null ? force : !Pn.open;
    Pn.el.hidden = !Pn.open;
    if (Pn.open) { sync(); const f = Pn.el.querySelector('.deck-search'); if (f) f.focus(); }
  };

  addEventListener('keydown', (e) => {
    if (e.target && /INPUT|SELECT|TEXTAREA/.test(e.target.tagName)) return;
    if (e.key === 'p' || e.key === 'P' || e.key === '`' || e.key === 'F2') { e.preventDefault(); Pn.toggle(); }
    if (e.key === 'Escape' && Pn.open) Pn.toggle(false);
  });
  NS.onConfig(() => { if (Pn.open) sync(); });

  // tiny hint for browser visitors (never shown when a wallpaper host drives the settings)
  const hint = document.getElementById('hint');
  Pn.tick = (F) => {
    if (!hint) return;
    Pn.hintT -= F.dt / Math.max(0.1, F.speed);
    const show = !NS.hosted && Pn.hintT > 0 && Pn.hintT < 6 && !Pn.open;
    hint.classList.toggle('on', show);
  };
})(window.NS);
