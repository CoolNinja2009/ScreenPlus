const container = document.getElementById("container");
let clock = new THREE.Clock();
const gui = new dat.GUI();
let devicePixelRatio = window.devicePixelRatio || 1;

let scene, camera, renderer, material;
let settings = { fps: 30, scale: 1.0, parallaxVal: 1, depth: 0.5 };
// Screen+: the background in 3D (depth/<picture>.png, 255 = near) and the pointer that moves it, smoothed
let mouse = { x: 0, y: 0, tx: 0, ty: 0 };
let videoElement;

//custom events
const sceneLoadedEvent = new Event("sceneLoaded");

async function init() {
  renderer = new THREE.WebGLRenderer({
    antialias: false,
  });
  renderer.setSize(window.innerWidth, window.innerHeight);
  renderer.setPixelRatio(settings.scale * devicePixelRatio);
  container.appendChild(renderer.domElement);
  scene = new THREE.Scene();
  camera = new THREE.OrthographicCamera(-1, 1, 1, -1, 0, 1);

  material = new THREE.ShaderMaterial({
    uniforms: {
      u_tex0: { type: "t" },
      u_time: { value: 0, type: "f" },
      u_intensity: { value: 0.4, type: "f" },
      u_speed: { value: 0.25, type: "f" },
      u_brightness: { value: 0.8, type: "f" },
      u_normal: { value: 0.5, type: "f" },
      u_zoom: { value: 2.61, type: "f" },
      u_blur_intensity: { value: 0.5, type: "f" },
      u_blur_iterations: { value: 16, type: "i" },
      u_panning: { value: false, type: "b" },
      u_post_processing: { value: true, type: "b" },
      u_lightning: { value: false, type: "b" },
      u_texture_fill: { value: true, type: "b" },
      u_depth: { value: null, type: "t" },
      u_has_depth: { value: false, type: "b" },
      u_mouse: { value: new THREE.Vector2(0, 0), type: "v2" },
      u_depth_amt: { value: 0.02, type: "f" },
      u_weather: { value: 0, type: "i" },
      u_scene: { value: 0.6, type: "f" },
      u_splash: { value: 0.3, type: "f" },
      u_land: { value: null, type: "t" },
      u_resolution: { value: new THREE.Vector2(window.innerWidth, window.innerHeight), type: "v2" },
      u_tex0_resolution: { value: new THREE.Vector2(window.innerWidth, window.innerHeight), type: "v2" },
    },
    vertexShader: `
          varying vec2 vUv;        
          void main() {
              vUv = uv;
              gl_Position = vec4( position, 1.0 );    
          }
        `,
  });
  material.fragmentShader = await (await fetch("shaders/rain.frag")).text();
  resize();

  // material.uniforms.u_tex0_resolution.value = new THREE.Vector2(1920, 1080);
  // material.uniforms.u_tex0.value = await new THREE.TextureLoader().loadAsync("assets/temp_background.webp");

  const quad = new THREE.Mesh(new THREE.PlaneGeometry(2, 2, 1, 1), material);
  scene.add(quad);

  window.addEventListener("resize", (e) => resize());
  render();
  datUI();

  document.dispatchEvent(sceneLoadedEvent);
}

function setScale(userScale) {
  settings.scale = userScale;
  const finalScale = devicePixelRatio * settings.scale;
  if (renderer.getPixelRatio() == finalScale)
    return;

  renderer.setPixelRatio(finalScale);
  material.uniforms.u_resolution.value = new THREE.Vector2(
    window.innerWidth * finalScale,
    window.innerHeight * finalScale
  );
}

function resize() {
  if (window.devicePixelRatio !== devicePixelRatio) {
    devicePixelRatio = window.devicePixelRatio || 1;
    setScale(settings.scale);
  }
  const finalScale = devicePixelRatio * settings.scale;

  renderer.setSize(window.innerWidth, window.innerHeight);
  material.uniforms.u_resolution.value = new THREE.Vector2(
    window.innerWidth * finalScale,
    window.innerHeight * finalScale
  );
}

function render() {
  setTimeout(function () {
    requestAnimationFrame(render);
  }, 1000 / settings.fps);

  //reset every 6hr
  if (clock.getElapsedTime() > 21600) clock = new THREE.Clock();
  material.uniforms.u_time.value = clock.getElapsedTime();
  mouse.x += (mouse.tx - mouse.x) * 0.12;
  mouse.y += (mouse.ty - mouse.y) * 0.12;
  material.uniforms.u_mouse.value.set(mouse.x, mouse.y);

  renderer.render(scene, camera);
}

init();

//lively api
//docs: https://github.com/rocksdanister/lively/wiki/Web-Guide-IV-:-Interaction
function livelyPropertyListener(name, val) {
  switch (name) {
    case "blurIntensity":
      material.uniforms.u_blur_intensity.value = val / 100;
      break;
    case "blurQuality":
      material.uniforms.u_blur_iterations.value = [1, 16, 32, 64][val];
      break;
    case "rainIntensity":
      material.uniforms.u_intensity.value = val / 100;
      break;
    case "rainSpeed":
      material.uniforms.u_speed.value = val / 100;
      break;
    case "brightness":
      material.uniforms.u_brightness.value = val / 100;
      break;
    case "rainNormal":
      material.uniforms.u_normal.value = val / 100;
      break;
    case "rainZoom":
      material.uniforms.u_zoom.value = val / 100;
      break;
    case "weather":
      material.uniforms.u_weather.value = val;
      break;
    case "sceneAmount":
      material.uniforms.u_scene.value = val / 100;
      break;
    case "splashAmount":
      material.uniforms.u_splash.value = val / 100;
      break;
    case "depthStrength":
      settings.depth = val / 100;
      material.uniforms.u_depth_amt.value = 0.06 * settings.depth;
      break;
    case "mediaSelect":
      {
        let ext = getExtension(val);
        loadDepth(val);
        disposeVideoElement(videoElement);
        material.uniforms.u_tex0.value?.dispose();
        if (["jpg", "jpeg", "jfif", "png", "webp", "bmp", "gif", "avif"].includes(ext)) {
          new THREE.TextureLoader().load(val, function (tex) {
            material.uniforms.u_tex0.value = tex;
            material.uniforms.u_tex0_resolution.value = new THREE.Vector2(tex.image.width, tex.image.height);
          });
        } else if (ext == "webm" || ext == "mp4") {
          videoElement = createVideoElement(val);
          let videoTexture = new THREE.VideoTexture(videoElement);
          videoElement.addEventListener(
            "loadedmetadata",
            function (e) {
              material.uniforms.u_tex0_resolution.value = new THREE.Vector2(
                videoTexture.image.videoWidth,
                videoTexture.image.videoHeight
              );
            },
            false
          );
          material.uniforms.u_tex0.value = videoTexture;
        }
      }
      break;
    case "mediaScaling":
      material.uniforms.u_texture_fill.value = [false, true][val];
      break;
    case "animateChk":
      material.uniforms.u_panning.value = val;
      break;
    case "lightningChk":
      material.uniforms.u_lightning.value = val;
      break;
    case "postProcessingChk":
      material.uniforms.u_post_processing.value = val;
      break;
    case "parallaxIntensity":
      settings.parallaxVal = val;
      break;
    case "fpsLock":
      settings.fps = val ? 30 : 60;
      break;
    case "displayScaling":
      setScale(val);
      break;  
    case "debug":
      if (val) gui.show();
      else gui.hide();
      break;
  }
}

//web
function datUI() {
  let rain = gui.addFolder("Rain");
  let bg = gui.addFolder("Background");
  let perf = gui.addFolder("Performance");
  let misc = gui.addFolder("More");
  rain.open();
  bg.open();
  perf.open();
  misc.open();
  rain.add(material.uniforms.u_intensity, "value", 0, 1, 0.01).name("Intensity");
  rain.add(material.uniforms.u_speed, "value", 0, 10, 0.01).name("Speed");
  rain.add(material.uniforms.u_brightness, "value", 0, 1, 0.01).name("Brightness");
  rain.add(material.uniforms.u_normal, "value", 0, 3, 0.01).name("Normal");
  rain.add(material.uniforms.u_zoom, "value", 0.1, 3.0, 0.01).name("Zoom");
  rain.add(material.uniforms.u_lightning, "value").name("Lightning");
  // bg.add(
  //   {
  //     picker: function () {
  //       document.getElementById("filePicker").click();
  //     },
  //   },
  //   "picker"
  // ).name("Change Background");
  bg.add(material.uniforms.u_blur_iterations, "value", 1, 64, 1).name("Blur Quality");
  bg.add(material.uniforms.u_blur_intensity, "value", 0, 10, 0.01).name("Blur");
  bg.add(settings, "parallaxVal", 0, 5, 1).name("Parallax");
  bg.add(material.uniforms.u_texture_fill, "value").name("Scale to Fill");
  bg.add(material.uniforms.u_panning, "value").name("Panning");
  bg.add(material.uniforms.u_post_processing, "value").name("Post Processing");
  perf.add(settings, "fps", 15, 120, 15).name("FPS");
  let tempScale = { value: settings.scale }; //don't update global value
  perf
    .add(tempScale, "value", 0.1, 2, 0.01)
    .name("Scale")
    .onChange(function () {
      setScale(tempScale.value);
    });
  misc
    .add(
      {
        lively: function () {
          window.open("https://www.rocksdanister.com/lively");
        },
      },
      "lively"
    )
    .name("Try It On Your Desktop!");
  misc
    .add(
      {
        source: function () {
          window.open("https://github.com/rocksdanister/rain");
        },
      },
      "source"
    )
    .name("Source Code");
  gui.close();
}

document.getElementById("filePicker").addEventListener("change", function () {
  if (this.files[0] === undefined) return;
  let file = this.files[0];
  if (file.type == "image/jpg" || file.type == "image/jpeg" || file.type == "image/png") {
    disposeVideoElement(videoElement);
    material.uniforms.u_tex0.value?.dispose();

    new THREE.TextureLoader().load(URL.createObjectURL(file), function (tex) {
      material.uniforms.u_tex0.value = tex;
      material.uniforms.u_tex0_resolution.value = new THREE.Vector2(tex.image.width, tex.image.height);
    });
  } else if (file.type == "video/mp4" || file.type == "video/webm") {
    disposeVideoElement(videoElement);
    material.uniforms.u_tex0.value?.dispose();

    videoElement = createVideoElement(URL.createObjectURL(file));
    let videoTexture = new THREE.VideoTexture(videoElement);
    videoElement.addEventListener(
      "loadedmetadata",
      function (e) {
        material.uniforms.u_tex0_resolution.value = new THREE.Vector2(
          videoTexture.image.videoWidth,
          videoTexture.image.videoHeight
        );
      },
      false
    );
    material.uniforms.u_tex0.value = videoTexture;
  }
});

//parallax: in 3D when the picture has a depth map, else the whole picture slides
document.addEventListener("mousemove", function (event) {
  if (material && material.uniforms.u_has_depth.value) {
    mouse.tx = (event.clientX / window.innerWidth - 0.5) * 2;
    mouse.ty = -(event.clientY / window.innerHeight - 0.5) * 2;
    if (event.clientX < 0 || event.clientY < 0) mouse.tx = mouse.ty = 0;   // Screen+ sends -16,-16 when the pointer leaves
    return;
  }
  if (settings.parallaxVal == 0) return;

  const x = (window.innerWidth - event.pageX * settings.parallaxVal) / 90;
  const y = (window.innerHeight - event.pageY * settings.parallaxVal) / 90;

  container.style.transform = `translateX(${x}px) translateY(${y}px) scale(1.09)`;
});

//helpers
// depth/<name of the picture>.png, made by Screen+'s depth model; without one the scene counts as far everywhere.
// A background just chosen in Screen+ gets its depth map a little later: it is looked for again for a while.
let depthLoad = 0;
function loadDepth(mediaPath) {
  const name = mediaPath.replace(/\\/g, "/").split("/").pop();
  const base = name.substring(0, name.lastIndexOf(".")) || name;
  const load = ++depthLoad;
  material.uniforms.u_depth.value?.dispose();
  material.uniforms.u_depth.value = null;
  material.uniforms.u_has_depth.value = false;
  let tries = 0;
  const attempt = () => {
    if (load !== depthLoad) return;                      // another background was chosen since
    new THREE.TextureLoader().load(
      "depth/" + base + ".png?" + tries,
      function (tex) {
        if (load !== depthLoad) return tex.dispose();
        tex.minFilter = tex.magFilter = THREE.LinearFilter;
        tex.generateMipmaps = false;
        material.uniforms.u_land.value?.dispose();
        material.uniforms.u_land.value = landingMap(tex.image);
        material.uniforms.u_depth.value = tex;
        material.uniforms.u_has_depth.value = true;
        container.style.transform = "";
      },
      undefined,
      function () {
        if (++tries < 120) setTimeout(attempt, 5000);     // up to 10 minutes (a model may be downloading)
      }
    );
  };
  attempt();
}

// The picture as a rough 3D surface: for every column and every distance (LAND_BINS steps, 0 near .. 1 far),
// where a drop falling at that distance first meets something as near as it is. Row b of the texture holds,
// per column, that point's height in the picture (0..1 from the bottom, 16 bits in R and G; 0: it never lands).
const LAND_BINS = 32;
function landingMap(img) {
  const W = Math.min(img.width, 1024), H = Math.max(1, Math.round(img.height * W / img.width));
  const cv = document.createElement("canvas");
  cv.width = W;
  cv.height = H;
  const ctx = cv.getContext("2d", { willReadFrequently: true });
  ctx.drawImage(img, 0, 0, W, H);
  const px = ctx.getImageData(0, 0, W, H).data;
  const out = new Uint8Array(W * LAND_BINS * 4);
  for (let x = 0; x < W; x++) {
    // going down the column, the nearest thing so far stops every drop at least that near: the far bins are
    // reached first, nearer ones as nearer things appear (`reached`: the nearest bin landed so far)
    let nearest = 1, reached = LAND_BINS;
    for (let y = 0; y < H && reached > 0; y++) {
      nearest = Math.min(nearest, 1 - px[(y * W + x) * 4] / 255);
      const first = Math.max(0, Math.ceil((nearest + 0.04) * LAND_BINS - 0.5));
      if (first >= reached) continue;
      const v = Math.round((1 - (y + 0.5) / H) * 65535);
      for (let b = first; b < reached; b++) {
        const o = (b * W + x) * 4;
        out[o] = v >> 8;
        out[o + 1] = v & 255;
        out[o + 3] = 255;
      }
      reached = first;
    }
  }
  const t = new THREE.DataTexture(out, W, LAND_BINS, THREE.RGBAFormat);
  t.minFilter = t.magFilter = THREE.NearestFilter;
  t.generateMipmaps = false;
  t.needsUpdate = true;
  return t;
}

function getExtension(filePath) {
  return filePath.substring(filePath.lastIndexOf(".") + 1, filePath.length).toLowerCase() || filePath;
}

function createVideoElement(src) {
  let htmlVideo = document.createElement("video");
  htmlVideo.src = src;
  htmlVideo.muted = true;
  htmlVideo.loop = true;
  htmlVideo.play();
  return htmlVideo;
}

//ref: https://stackoverflow.com/questions/3258587/how-to-properly-unload-destroy-a-video-element
function disposeVideoElement(video) {
  if (video != null && video.hasAttribute("src")) {
    video.pause();
    video.removeAttribute("src"); // empty source
    video.load();
  }
}
