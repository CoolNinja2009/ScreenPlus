'use strict';
/* NULL//SAINT - WebGL2 post pipeline: scene upload -> mosh feedback -> bloom -> signal damage */
(function (NS) {
  const SH = NS.SH;
  const INTS = new Set(['uColorMode', 'uMirror']);
  const SAMPLERS = { uScene: 0, uPrev: 1, uSrc: 0, uBloom: 1, uAtlas: 2 };

  const P = NS.post = { ok: false, gl: null };

  function compile(gl, type, src) {
    const s = gl.createShader(type);
    gl.shaderSource(s, src); gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
      const log = gl.getShaderInfoLog(s);
      console.error('[NULL//SAINT] shader error:', log);
      throw new Error(log);
    }
    return s;
  }
  function program(gl, fsSrc) {
    const p = gl.createProgram();
    gl.attachShader(p, compile(gl, gl.VERTEX_SHADER, SH.vert));
    gl.attachShader(p, compile(gl, gl.FRAGMENT_SHADER, fsSrc));
    gl.bindAttribLocation(p, 0, 'aPos');
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
    const locs = {};
    return {
      p,
      loc(name) { if (!(name in locs)) locs[name] = gl.getUniformLocation(p, name); return locs[name]; },
    };
  }
  function texture(gl, w, h, filter) {
    const t = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, t);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter || gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filter || gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    if (w) gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    return t;
  }
  function fbo(gl, w, h) {
    const tex = texture(gl, w, h);
    const fb = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    return { fb, tex, w, h };
  }
  function freeFbo(gl, f) { if (!f) return; gl.deleteFramebuffer(f.fb); gl.deleteTexture(f.tex); }

  function makeAtlas() {
    const chars = ' .:-=+*xo%#&8@▓█';
    const cw = 32, ch = 48;
    const c = NS.U.makeCanvas(cw * 16, ch);
    const x = c.getContext('2d');
    x.fillStyle = '#000'; x.fillRect(0, 0, c.width, c.height);
    x.fillStyle = '#fff'; x.textAlign = 'center'; x.textBaseline = 'middle';
    x.font = NS.U.font(40, 'mono', 'bold');
    for (let i = 0; i < 16; i++) x.fillText(chars[i], i * cw + cw / 2, ch / 2 + 2);
    return c;
  }

  P.init = (canvas) => {
    let gl = null;
    try { gl = canvas.getContext('webgl2', { antialias: false, alpha: false, premultipliedAlpha: false, preserveDrawingBuffer: /[?&]capture/.test(location.search), powerPreference: 'high-performance' }); } catch (e) { gl = null; }
    if (!gl) return false;
    P.gl = gl;
    try {
      P.prog = {
        mosh: program(gl, SH.mosh), bright: program(gl, SH.bright), blur: program(gl, SH.blur),
        final: program(gl, SH.final), copy: program(gl, SH.copy),
      };
    } catch (e) { console.error(e); return false; }
    const vao = gl.createVertexArray(); gl.bindVertexArray(vao);
    const buf = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    P.sceneTex = texture(gl);
    P.atlasTex = texture(gl);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
    gl.bindTexture(gl.TEXTURE_2D, P.atlasTex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, makeAtlas());
    canvas.addEventListener('webglcontextlost', (e) => { e.preventDefault(); P.ok = false; NS.contextLost = true; });
    P.ok = true;
    return true;
  };

  P.resize = (w, h, sw, sh) => {
    const gl = P.gl; if (!gl) return;
    P.w = w; P.h = h; // output size
    freeFbo(gl, P.a); freeFbo(gl, P.b); freeFbo(gl, P.bl1); freeFbo(gl, P.bl2);
    P.a = fbo(gl, sw, sh); P.b = fbo(gl, sw, sh);
    const bw = Math.max(2, sw >> 2), bh = Math.max(2, sh >> 2);
    P.bl1 = fbo(gl, bw, bh); P.bl2 = fbo(gl, bw, bh);
    P.sw = sw; P.sh = sh;
    P.cleared = false;
  };

  function use(prog, uniforms) {
    const gl = P.gl;
    gl.useProgram(prog.p);
    for (const k in uniforms) {
      const l = prog.loc(k); if (l == null) continue;
      const v = uniforms[k];
      if (k in SAMPLERS) { gl.uniform1i(l, SAMPLERS[k]); continue; }
      if (typeof v === 'number') { if (INTS.has(k)) gl.uniform1i(l, v | 0); else gl.uniform1f(l, v); }
      else if (v.length === 2) gl.uniform2f(l, v[0], v[1]);
      else if (v.length === 3) gl.uniform3f(l, v[0], v[1], v[2]);
      else if (v.length === 4) gl.uniform4f(l, v[0], v[1], v[2], v[3]);
      else gl.uniform4fv(l, v);
    }
  }
  function bindTex(unit, tex) { const gl = P.gl; gl.activeTexture(gl.TEXTURE0 + unit); gl.bindTexture(gl.TEXTURE_2D, tex); }
  function target(f) {
    const gl = P.gl;
    if (f) { gl.bindFramebuffer(gl.FRAMEBUFFER, f.fb); gl.viewport(0, 0, f.w, f.h); }
    else { gl.bindFramebuffer(gl.FRAMEBUFFER, null); gl.viewport(0, 0, P.w, P.h); }
  }
  const draw = () => P.gl.drawArrays(P.gl.TRIANGLES, 0, 6);

  /* u: { mosh: {...}, final: {...}, bloomThr } */
  P.render = (sceneCanvas, u) => {
    const gl = P.gl; if (!P.ok || gl.isContextLost()) return;
    // upload the 2D scene
    bindTex(0, P.sceneTex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, sceneCanvas);

    // feedback: scene + previous -> a
    if (!P.cleared) { // seed history with the scene so the first frames don't mosh from black
      target(P.b); use(P.prog.copy, { uSrc: 0 }); bindTex(0, P.sceneTex); draw(); P.cleared = true;
    }
    target(P.a);
    use(P.prog.mosh, Object.assign({ uScene: 0, uPrev: 1 }, u.mosh));
    bindTex(0, P.sceneTex); bindTex(1, P.b.tex);
    draw();

    // bloom from a
    target(P.bl1); use(P.prog.bright, { uSrc: 0, uThr: u.bloomThr }); bindTex(0, P.a.tex); draw();
    for (let i = 0; i < 2; i++) {
      target(P.bl2); use(P.prog.blur, { uSrc: 0, uDir: [(1 + i) / P.bl1.w, 0] }); bindTex(0, P.bl1.tex); draw();
      target(P.bl1); use(P.prog.blur, { uSrc: 0, uDir: [0, (1 + i) / P.bl1.h] }); bindTex(0, P.bl2.tex); draw();
    }

    // final to screen
    target(null);
    use(P.prog.final, Object.assign({ uSrc: 0, uBloom: 1, uAtlas: 2 }, u.final));
    bindTex(0, P.a.tex); bindTex(1, P.bl1.tex); bindTex(2, P.atlasTex);
    draw();

    // swap history
    const t = P.a; P.a = P.b; P.b = t;
  };
})(window.NS);
