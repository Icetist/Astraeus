// the whole site's motion. gsap and lenis drive the scroll; paper's liquid metal draws the chrome star.
// everything degrades to a plain, readable page without javascript, without webgl, or with reduced motion
import { ShaderMount } from "./vendor/paper/shader-mount.js";
import { liquidMetalFragmentShader } from "./vendor/paper/shaders/liquid-metal.js";
import { getShaderColorFromString as color } from "./vendor/paper/get-shader-color-from-string.js";

const root = document.documentElement;
root.classList.remove("no-js");
root.classList.add("js");
const $ = (s, el = document) => el.querySelector(s);
const $$ = (s, el = document) => [...el.querySelectorAll(s)];
const still = matchMedia("(prefers-reduced-motion: reduce)").matches;
const { gsap, ScrollTrigger, Lenis } = window;
const moving = !still && !!gsap && !!ScrollTrigger;
const svgUse = (id) => `<svg aria-hidden="true" focusable="false"><use href="#i-${id}"/></svg>`;
if (moving) gsap.registerPlugin(ScrollTrigger);

/* ---------- sound: soft ticks, off until asked for ---------- */
let audio = null, soundOn = false;
function tick(freq = 1500, len = 0.035, vol = 0.05) {
  if (!soundOn) return;
  audio ??= new (window.AudioContext || window.webkitAudioContext)();
  const o = audio.createOscillator(), g = audio.createGain(), t = audio.currentTime;
  o.frequency.value = freq; o.type = "sine";
  g.gain.setValueAtTime(vol, t); g.gain.exponentialRampToValueAtTime(0.0001, t + len);
  o.connect(g).connect(audio.destination); o.start(t); o.stop(t + len + 0.02);
}
const soundBtn = $("#sound");
if (soundBtn) {
  soundBtn.innerHTML = svgUse("soundoff");
  soundBtn.addEventListener("click", () => {
    soundOn = !soundOn;
    soundBtn.setAttribute("aria-pressed", soundOn);
    soundBtn.innerHTML = svgUse(soundOn ? "sound" : "soundoff");
    tick(900, 0.08, 0.06);
  });
}

/* ---------- theme: light by default, dark on the switch ---------- */
const themeBtn = $("#theme");
const paintTheme = () => {
  const dark = root.dataset.theme === "dark";
  themeBtn.innerHTML = svgUse(dark ? "sun" : "moon");
  themeBtn.setAttribute("aria-label", dark ? "Light mode" : "Dark mode");
  themeBtn.setAttribute("aria-pressed", dark);
  $('meta[name="theme-color"]').content = dark ? "#000000" : "#f5f5f7";
};
if (themeBtn) {
  paintTheme();
  themeBtn.addEventListener("click", () => {
    root.dataset.theme = root.dataset.theme === "dark" ? "light" : "dark";
    try { localStorage.setItem("astraeus-theme", root.dataset.theme); } catch {}
    paintTheme(); tick(700, 0.06);
  });
}

/* ---------- the narrator: the long pill's middle, like a sung line ---------- */
const say = $(".p-say");
const SAY_SETTLE = 140; // ms a line must hold before it's spoken, so a fling doesn't stack lines in the pill
let said = "", sayTimer = 0;
function speak(html) {
  if (html === said) return;
  said = html;
  const lines = $$(".p-line", say), next = document.createElement("span");
  next.className = "p-line"; next.innerHTML = html;
  if (!moving) { say.replaceChildren(next); return; }
  // only the line on show leaves; anything still mid-flight is dropped at once
  gsap.killTweensOf(lines);
  const old = lines.pop();
  lines.forEach((l) => l.remove());
  say.style.position = "relative";
  next.style.cssText = "position:absolute;left:0;right:0";
  say.append(next);
  // the old line clears out first, so two lines never cross in the pill
  if (old) gsap.to(old, { yPercent: -70, opacity: 0, filter: "blur(4px)", duration: 0.2, ease: "power2.in", onComplete: () => old.remove() });
  gsap.fromTo(next, { yPercent: 70, opacity: 0, filter: "blur(4px)" }, { yPercent: 0, opacity: 1, filter: "blur(0px)", duration: 0.45, delay: old ? 0.16 : 0, ease: "expo.out", onComplete: () => { next.style.cssText = ""; } });
}
function setSay(html, now = false) {
  if (!say || !html) return;
  clearTimeout(sayTimer);
  if (now || !moving) speak(html); else sayTimer = setTimeout(() => speak(html), SAY_SETTLE);
}

/* ---------- liquid chrome: the logo's star, and the download buttons ---------- */
// each mount wanders through its own settings on slow waves that never line up, so the metal doesn't loop
const metals = [];
const wave = (t, k) => Math.sin(t * 0.131 + k) * 0.5 + Math.sin(t * 0.0713 + k * 2.31) * 0.3 + Math.sin(t * 0.0377 + k * 5.13) * 0.2;
let lean = 0, leanTo = 0; // the pointer tips the light a little
const BASE = { u_softness: 0.1, u_contour: 0.4, u_rotation: 0, u_offsetX: 0, u_offsetY: 0, u_originX: 0.5, u_originY: 0.5, u_worldWidth: 0, u_worldHeight: 0, u_colorBack: color("rgba(0,0,0,0)"), u_colorTint: color("#ffffff") };
function mount(el, uniforms, mipmaps, look) {
  try {
    const m = new ShaderMount(el, liquidMetalFragmentShader, { ...BASE, ...uniforms }, { premultipliedAlpha: false }, still ? 0 : 1, Math.random() * 6e5, 2, undefined, mipmaps);
    metals.push({ m, k: Math.random() * 100, ...look });
    return true;
  } catch { el.classList.add("no-gl"); return false; } // no webgl: the css chrome stays
}
function metalStar(el) {
  if (!el) return;
  const img = new Image();
  img.onload = () => mount(el, { u_image: img, u_isImage: true, u_repetition: 2, u_shiftRed: 0.3, u_shiftBlue: 0.3, u_distortion: 0.07, u_angle: 70, u_shape: 0, u_fit: 1, u_scale: 0.9 }, ["u_image"], { rep: 2, ang: 70, shift: 0.3 });
  img.onerror = () => el.classList.add("no-gl");
  img.src = "assets/img/star-metal.png";
}
metalStar($("#metal"));
metalStar($("#metal2"));
$$(".btn.chrome, .p-dl").forEach((b) => {
  const layer = document.createElement("span");
  layer.className = "btn-metal"; layer.setAttribute("aria-hidden", "true");
  b.prepend(layer);
  // only the button's edge shows it (css masks the middle away); shape 0 fills the box, so the ring follows any size
  if (mount(layer, { u_isImage: false, u_shape: 0, u_repetition: 1.4, u_shiftRed: 0.04, u_shiftBlue: 0.04, u_distortion: 0.08, u_angle: 80, u_fit: 0, u_scale: 1 }, [], { rep: 1.4, ang: 80, shift: 0.04 })) b.classList.add("live");
  else layer.remove();
});
if (!still) {
  const t0 = performance.now();
  const drift = () => {
    const t = (performance.now() - t0) / 1000;
    lean += (leanTo - lean) * 0.04;
    for (const x of metals) {
      if (!x.m.isInViewport) continue;
      const a = wave(t, x.k), b = wave(t * 1.37, x.k + 17), c = wave(t * 0.83, x.k + 41);
      x.m.setUniforms({ u_angle: x.ang + a * 34 + lean * 16, u_repetition: x.rep + b * 0.7, u_distortion: 0.1 + c * 0.07, u_shiftRed: x.shift * (1 + b * 0.6), u_shiftBlue: x.shift * (1 - c * 0.6) });
      x.m.setSpeed(0.75 + a * 0.45); // it hurries and lingers
    }
    requestAnimationFrame(drift);
  };
  requestAnimationFrame(drift);
  window.addEventListener("pointermove", (e) => { leanTo = e.clientX / innerWidth - 0.5; }, { passive: true });
}

/* ---------- smooth scroll ---------- */
let lenis = null;
if (moving && Lenis) {
  lenis = new Lenis({ lerp: 0.075, wheelMultiplier: 0.85, touchMultiplier: 1.2, smoothWheel: true }); // a touch heavier, so a flick glides instead of jumping
  lenis.on("scroll", ScrollTrigger.update);
  gsap.ticker.add((t) => lenis.raf(t * 1000));
  gsap.ticker.lagSmoothing(250, 33); // a slow frame (shaders compiling) mustn't skip the intro ahead
}
/* the back-to-top ring fills as you read */
const toTop = $(".to-top");
let maxY = 1;
const measure = () => { maxY = Math.max(1, document.documentElement.scrollHeight - innerHeight); };
function onScrolled() {
  const y = scrollY;
  toTop?.style.setProperty("--read", (y / maxY).toFixed(4));
  toTop?.classList.toggle("on", y > innerHeight * 0.9);
}
measure();
window.addEventListener("resize", measure);
if (moving) ScrollTrigger.addEventListener("refresh", measure);
window.addEventListener("scroll", onScrolled, { passive: true });
onScrolled();
$$('a[href^="#"]').forEach((a) => a.addEventListener("click", (e) => {
  const id = a.getAttribute("href"); if (id.length < 2) return;
  const el = $(id); if (!el) return;
  e.preventDefault();
  const far = Math.abs(el.getBoundingClientRect().top) > innerHeight * 4;
  if (lenis) lenis.scrollTo(id === "#top" ? 0 : el, { duration: far ? 2.2 : 1.4, easing: (x) => 1 - Math.pow(1 - x, 4) }); else el.scrollIntoView({ behavior: still ? "auto" : "smooth" });
  history.replaceState(null, "", id);
}));

/* ---------- the intro: press it open ---------- */
const intro = $("#intro");
let opened = false;
if ("scrollRestoration" in history) history.scrollRestoration = "manual"; // a reload starts at the top, where the intro is
if (intro && moving) window.scrollTo(0, 0);
function heroIn() {
  if (!moving) return null;
  return gsap.timeline({ defaults: { ease: "expo.out" } })
    .fromTo(".hero-panel", { clipPath: "inset(0% 0% 100% 0% round 18px)", y: -30 }, { clipPath: "inset(0% 0% 0% 0% round 18px)", y: 0, duration: 1.4, clearProps: "clipPath" })
    .fromTo(".hero .line > span", { yPercent: 110 }, { yPercent: 0, duration: 1.2, stagger: 0.08 }, 0.35)
    .fromTo(".hero-foot", { opacity: 0, y: 20 }, { opacity: 1, y: 0, duration: 1 }, 0.7);
}
function onKey(e) {
  if (e.key === "Enter" || e.key === " " || (e.altKey && e.key.toLowerCase() === "a")) { e.preventDefault(); open(false); }
}
function open(fast) {
  if (opened) return;
  opened = true;
  window.removeEventListener("keydown", onKey);
  if (!intro) return;
  // skip is a cut: the page is simply there
  if (fast || !moving) { gsap?.killTweensOf("#metal, .intro-ui, .intro kbd"); intro.remove(); lenis?.start(); return; }
  tick(520, 0.25, 0.07);
  gsap.killTweensOf("#metal, .intro-ui, .intro kbd"); // an early press: the entrance gives way to the opening
  gsap.set(".intro kbd", { opacity: 1, y: 0 });
  const star = $("#metal"), mark = $(".p-brand svg"), pill = $("#pill");
  gsap.set(pill, { xPercent: -50, x: 0 }); // the pill centres by percent, so a resize mid-flight can't strand it
  const pr = mark.getBoundingClientRect(), sr = star.getBoundingClientRect();
  const keys = $$(".intro kbd");
  const tl = gsap.timeline({ onComplete: () => { intro.remove(); gsap.set(pill, { clearProps: "transform" }); lenis?.start(); } });
  // the shortcut is pressed, key by key
  keys.forEach((k, i) => tl.add(() => { k.classList.add("down"); tick(1500 + i * 220, 0.03); }, i * 0.13));
  tl.add(() => keys.forEach((k) => k.classList.remove("down")), keys.length * 0.13 + 0.12)
    .to(".intro-ui, .intro-skip", { opacity: 0, y: 10, filter: "blur(4px)", duration: 0.35, ease: "power2.in", overwrite: true }, 0.5) // overwrite: an early press beats the fade-in
    // the star gathers itself
    .to(star, { scale: 1.14, rotation: -14, duration: 0.45, ease: "power2.inOut" }, 0.5)
    // a seam of light opens the page from the middle
    .to(".intro-seam", { width: "100%", duration: 0.55, ease: "expo.inOut" }, 0.75)
    .to(".intro-half.top", { yPercent: -100, duration: 1.15, ease: "expo.inOut" }, 1.12)
    .to(".intro-half.bot", { yPercent: 100, duration: 1.15, ease: "expo.inOut" }, 1.12)
    .to(".intro-seam", { opacity: 0, scaleY: 6, duration: 0.5, ease: "power2.out" }, 1.25)
    // and the star flies up into the pill's logo, spinning home
    .to(star, { x: pr.left + pr.width / 2 - (sr.left + sr.width / 2), y: pr.top + pr.height / 2 - (sr.top + sr.height / 2), scale: pr.height / sr.height, rotation: 360, duration: 1.2, ease: "expo.inOut" }, 1.0)
    .fromTo(pill, { y: -90 }, { y: 0, duration: 1, ease: "expo.out", immediateRender: false }, 1.35)
    .add(() => tick(900, 0.12, 0.05), 2.15);
  const hero = heroIn();
  if (hero) tl.add(hero, 1.2);
}
if (intro) {
  if (!moving) open(true);
  else {
    lenis?.stop();
    gsap.fromTo("#metal", { scale: 0.6, opacity: 0 }, { scale: 1, opacity: 1, duration: 1.6, ease: "expo.out" });
    gsap.fromTo(".intro-ui", { opacity: 0, y: 14 }, { opacity: 1, y: 0, duration: 1, delay: 0.5, ease: "expo.out" });
    gsap.fromTo(".intro kbd", { opacity: 0, y: 8 }, { opacity: 1, y: 0, duration: 0.6, delay: 0.8, stagger: 0.09, ease: "back.out(2)" });
    window.addEventListener("keydown", onKey);
    $(".intro-go").addEventListener("click", () => open(false));
    $(".intro-skip").addEventListener("click", () => open(true));
    intro.addEventListener("wheel", () => open(false), { passive: true, once: true });
    intro.addEventListener("touchmove", () => open(false), { passive: true, once: true });
    $(".intro-go").focus({ preventScroll: true });
  }
}

/* ---------- the hero panel leans toward the pointer ---------- */
if (moving && matchMedia("(pointer: fine)").matches) {
  const p = $(".hero-panel .shot");
  const rx = gsap.quickTo(p, "rotationX", { duration: 0.8, ease: "power3" }), ry = gsap.quickTo(p, "rotationY", { duration: 0.8, ease: "power3" });
  $(".hero").addEventListener("pointermove", (e) => { const r = p.getBoundingClientRect(); ry(((e.clientX - r.left) / r.width - 0.5) * 6); rx(-((e.clientY - r.top) / r.height - 0.5) * 6); });
  $(".hero").addEventListener("pointerleave", () => { rx(0); ry(0); });
}

let owner = null; // the pinned chapter narrating right now, if any
const own = (name, on, line) => { if (on) { owner = name; if (line) setSay(line); } else if (owner === name) owner = null; };

/* ---------- the long pill's items, for the dial ---------- */
const ITEMS = [
  ["clock", "Clock", () => new Date().toLocaleTimeString([], { hour: "numeric", minute: "2-digit" })],
  ["date", "Date", () => new Date().toLocaleDateString([], { weekday: "short", day: "numeric", month: "short" })],
  ["weather", "Weather", () => "21°"], ["note", "Music", () => "Afterglow"], ["lyrics", "Lyrics", () => "sings along"],
  ["cover", "Cover", () => "now playing"], ["bars", "Visualizer", () => "now playing"], ["timer", "Timer", () => "24:12"],
  ["volume", "Volume", () => "70%"], ["wifi", "Wi-Fi", () => "online"], ["cpu", "CPU", () => "14%"], ["gpu", "GPU", () => "6%"],
  ["ram", "RAM", () => "61%"], ["lan", "Network", () => "↓2K ↑0K"], ["disk", "Disk", () => "70%"], ["battery", "Battery", () => "84%"],
  ["lang", "Language", () => "EN"], ["window", "Window", () => "Notes"], ["trend", "Markets", () => "S&P +0.4%"], ["trophy", "Score", () => "2–1  67′"],
];
const dialList = $(".dial-list");
if (dialList) dialList.innerHTML = ITEMS.map(([, n]) => `<li>${n}</li>`).join("");

/* ---------- reveals that work at every size ---------- */
if (moving) {
  $$("h2.big").forEach((h) => gsap.fromTo($$(".line > span", h), { yPercent: 110 }, { yPercent: 0, duration: 1.2, stagger: 0.08, ease: "expo.out", scrollTrigger: { trigger: h, start: "top 85%" } }));
  gsap.fromTo(".get .mega .line > span", { yPercent: 110 }, { yPercent: 0, duration: 1.2, ease: "expo.out", scrollTrigger: { trigger: ".get", start: "top 70%" } });
  // the narrator: whatever crosses the middle of the screen speaks, unless a pinned chapter is speaking for itself
  const sayers = $$("main > section[data-say]");
  ScrollTrigger.create({ start: 0, end: "max", onUpdate: () => {
    if (owner) return;
    const mid = innerHeight / 2;
    const s = sayers.find((x) => { const r = x.getBoundingClientRect(); return r.top <= mid && r.bottom >= mid; });
    if (s) setSay(s.dataset.say);
  } });
  $$(".chapters a").forEach((a) => { const sec = $(a.getAttribute("href")); if (sec) ScrollTrigger.create({ trigger: sec, start: "top 50%", end: "bottom 50%", onToggle: (st) => a.classList.toggle("on", st.isActive) }); });
  $$(".fig .num").forEach((n) => {
    const to = parseFloat(n.dataset.to), dec = +n.dataset.dec, o = { v: 0 };
    n.textContent = (0).toFixed(dec);
    gsap.to(o, { v: to, duration: 2.2, ease: "expo.out", scrollTrigger: { trigger: n, start: "top 85%" }, onUpdate: () => (n.textContent = o.v.toFixed(dec)) });
  });
  gsap.fromTo(".eng .ring", { scale: 0.6, opacity: 0 }, { scale: 1, opacity: 1, ease: "none", scrollTrigger: { trigger: ".eng", start: "top bottom", end: "center center", scrub: true } });
  gsap.fromTo(".isl-real .shot", { y: 60, opacity: 0 }, { y: 0, opacity: 1, duration: 1.2, stagger: 0.12, ease: "expo.out", scrollTrigger: { trigger: ".isl-real", start: "top 85%" } });

  // the pinned chapters: wide screens only
  gsap.matchMedia().add("(min-width: 900px)", () => {
    root.classList.add("js-pin");
    const kill = [storyPin(), dialPin(), lyricsPin(), looksPin(), wallScroll()];
    ScrollTrigger.sort(); // the triggers made before the pins must measure after them, in page order
    ScrollTrigger.refresh();
    return () => { root.classList.remove("js-pin"); kill.forEach((k) => k && k()); };
  });
}

function storyPin() {
  const steps = $$(".st-steps li"), N = steps.length;
  const imgs = Object.fromEntries($$(".st-panel img").map((i) => [i.dataset.k, i]));
  const cards = $$(".st-cards img");
  const title = $(".st-title span"), text = $(".st-text"), how = $(".st-how"), num = $(".st-num"), inLabel = $(".st-in");
  const dots = $(".st-dots");
  dots.innerHTML = steps.map(() => "<li></li>").join("");
  const dotEls = $$("li", dots);
  const fan = [[-31, -34, -7], [31, -36, 6], [-37, 30, 5], [37, 32, -6], [0, 52, 0]]; // where the panels behind home land: x%, y%, tilt
  let cur = -1, shown = "home";
  gsap.set(cards, { xPercent: -50, yPercent: -50 });
  let titleTl = null;
  function show(i) {
    if (i === cur) return;
    const s = steps[i], k = s.dataset.k, pk = k === "behind" ? "home" : k, was = cur, inside = s.dataset.in;
    cur = i;
    dotEls.forEach((d, j) => d.classList.toggle("on", j === i));
    num.textContent = String(i + 1).padStart(2, "0");
    inLabel.textContent = inside ? `  ·  inside ${inside}` : ""; // the panels behind home, and settings behind tools
    const dir = was < i ? 1 : -1, name = s.querySelector("h3").textContent;
    titleTl?.kill(); // a quick scroll mustn't let an older title land after this one
    titleTl = gsap.timeline()
      .to(title, { yPercent: -105 * dir, duration: 0.18, ease: "power2.in" })
      .add(() => { title.textContent = name; })
      .fromTo(title, { yPercent: 105 * dir }, { yPercent: 0, duration: 0.55, ease: "expo.out" });
    text.textContent = s.querySelector("p").textContent;
    how.textContent = s.querySelector(".how")?.textContent || "";
    gsap.fromTo([text, how], { opacity: 0, y: 10 }, { opacity: 1, y: 0, duration: 0.5, ease: "expo.out", stagger: 0.04, overwrite: true });
    if (pk !== shown) {
      shown = pk;
      Object.entries(imgs).forEach(([key, img]) => key === pk
        ? gsap.fromTo(img, { opacity: 0, scale: 1.03, filter: "blur(8px)" }, { opacity: 1, scale: 1, filter: "blur(0px)", duration: 0.7, ease: "expo.out", overwrite: true })
        : gsap.to(img, { opacity: 0, duration: 0.35, ease: "power2.out", overwrite: true }));
    }
    const behind = k === "behind";
    gsap.to(".st-panel", { scale: behind ? 0.9 : 1, opacity: behind ? 0.3 : 1, duration: 0.7, ease: "expo.out", overwrite: true });
    cards.forEach((c, j) => {
      const [x, y, r] = fan[j];
      gsap.to(c, behind
        ? { opacity: 1, xPercent: -50 + x * 2.2, yPercent: -50 + y * 2.2, rotation: r, scale: 1, duration: 0.8, delay: j * 0.05, ease: "expo.out", overwrite: true }
        : { opacity: 0, xPercent: -50, yPercent: -50, rotation: 0, scale: 0.8, duration: 0.4, ease: "power2.in", overwrite: true });
    });
    if (owner === "story") { setSay(s.dataset.say); tick(dir > 0 ? 1400 : 1150); }
  }
  const st = ScrollTrigger.create({
    trigger: ".story", pin: ".st-pin", start: "top top", end: () => "+=" + (N - 1) * window.innerHeight * 0.55,
    onUpdate: (self) => show(Math.min(N - 1, Math.floor(self.progress * N))),
    onLeave: () => show(N - 1), onEnterBack: (self) => show(Math.min(N - 1, Math.floor(self.progress * N))), onLeaveBack: () => show(0),
    onToggle: (self) => own("story", self.isActive, steps[Math.max(cur, 0)].dataset.say),
  });
  const tilt = gsap.fromTo(".st-stage", { rotationX: 24, y: 80 }, { rotationX: 0, y: 0, ease: "none", transformPerspective: 1800, scrollTrigger: { trigger: ".story", start: "top bottom", end: "top top", scrub: true } });
  show(0);
  return () => { st.kill(); tilt.scrollTrigger?.kill(); tilt.kill(); };
}

function dialPin() {
  const svg = $(".dial-svg");
  if (!svg) return null;
  const C = 750, R = 720, STEP = 360 / ITEMS.length;
  let marks = "";
  for (let a = 0; a < 360; a += 2) {
    const major = a % STEP === 0, r0 = R - (major ? 30 : 14), rad = (a - 90) * Math.PI / 180;
    marks += `<line class="tick${major ? " major" : ""}" x1="${C + r0 * Math.cos(rad)}" y1="${C + r0 * Math.sin(rad)}" x2="${C + R * Math.cos(rad)}" y2="${C + R * Math.sin(rad)}"/>`;
  }
  const labels = ITEMS.map(([, n], i) => {
    const a = i * STEP, rad = (a - 90) * Math.PI / 180, r = R - 58, x = C + r * Math.cos(rad), y = C + r * Math.sin(rad);
    return `<g class="lab"><text x="${x}" y="${y}" text-anchor="middle" dominant-baseline="middle" transform="rotate(${a} ${x} ${y})">${n}</text></g>`;
  }).join("");
  svg.setAttribute("viewBox", "0 0 1500 1500");
  svg.innerHTML = `<circle class="face" cx="${C}" cy="${C}" r="${R}"/><g class="wheel">${marks}${labels}</g>`;
  const wheel = $(".wheel", svg), labs = $$(".lab", svg);
  const ico = $(".dn-ico"), name = $(".dn-name"), val = $(".dn-val");
  let cur = -1;
  function pick(i) {
    if (i === cur) return;
    cur = i;
    const [id, n, v] = ITEMS[i];
    ico.innerHTML = svgUse(id); name.textContent = n; val.textContent = v();
    labs.forEach((l, j) => l.classList.toggle("on", j === i));
    if (owner === "dial") { setSay(`${svgUse(id)}${n} · ${v()}`); tick(1700, 0.025, 0.04); } // only while the dial is on screen
  }
  pick(0);
  const DIAL_PER_ITEM = 0.17; // screens of scrolling per item: a notch at a time, not a spin
  gsap.set(wheel, { svgOrigin: `${C} ${C}` });
  const turn = gsap.quickTo(wheel, "rotation", { duration: 0.6, ease: "power3" });
  const st = ScrollTrigger.create({
    trigger: ".dial-pin", pin: true, start: "top top+=60", end: () => "+=" + window.innerHeight * DIAL_PER_ITEM * (ITEMS.length - 1),
    snap: { snapTo: 1 / (ITEMS.length - 1), duration: { min: 0.2, max: 0.5 }, delay: 0.12, ease: "power2.inOut" },
    onToggle: (self) => { const [id, n, v] = ITEMS[Math.max(cur, 0)]; own("dial", self.isActive, `${svgUse(id)}${n} · ${v()}`); },
    onUpdate: (self) => {
      const rot = self.progress * (ITEMS.length - 1) * STEP;
      turn(-rot);
      pick(Math.round(rot / STEP));
    },
  });
  return () => st.kill();
}

function splitWords(li) {
  if (li.dataset.split) return;
  li.dataset.split = "1";
  li.innerHTML = li.textContent.split(/(\s+)/).map((w) => (/^\s+$/.test(w) ? w : `<span class="w">${w}</span>`)).join("");
}
function lyricsPin() {
  const frame = $(".ly-frame"), lines = $$(".sing li"), sing = $(".sing"), stage = $(".ly-stage"), fill = $(".ly-bar b"), clock = $(".ly-t0");
  const SONG = 221, LINE = 0.5, START = 1.25; // a 3:41 song; each line holds half a beat of the timeline
  lines.forEach(splitWords);
  const w0 = Math.min(window.innerWidth * 0.46, 760);
  const centre = (l) => -(l.offsetTop + l.offsetHeight / 2); // lifts the list so this line sits on the singing height
  const tl = gsap.timeline({ scrollTrigger: { trigger: ".lyrics", pin: ".ly-pin", start: "top top", end: () => "+=" + window.innerHeight * 2.6, scrub: 0.7, invalidateOnRefresh: true,
    onUpdate: (self) => { const sec = Math.round(self.progress * SONG * 0.42); clock.textContent = `${Math.floor(sec / 60)}:${String(sec % 60).padStart(2, "0")}`; } } });
  tl.fromTo(frame, { width: w0, height: w0 * 767 / 1365, borderRadius: 22 }, { width: () => window.innerWidth, height: () => window.innerHeight, borderRadius: 0, duration: 1, ease: "power2.inOut" })
    // the player melts into a field of its own colours, so the words have a room to sing in
    .fromTo(".ly-frame img", { filter: "blur(0px) saturate(1) brightness(1)", scale: 1 }, { filter: "blur(46px) saturate(1.6) brightness(.62)", scale: 1.25, duration: 0.45, ease: "power1.inOut" }, 0.85)
    .fromTo(frame, { "--shade-in": 0 }, { "--shade-in": 1, duration: 0.4 }, 0.9)
    .fromTo(stage, { opacity: 0 }, { opacity: 1, duration: 0.25 }, 1.05)
    .fromTo(".ly-np", { y: 16, opacity: 0 }, { y: 0, opacity: 1, duration: 0.25 }, 1.05)
    .fromTo(fill, { scaleX: 0 }, { scaleX: 0.42, duration: START + lines.length * LINE - 1.05, ease: "none" }, 1.05)
    .set(sing, { y: () => centre(lines[0]) }, 0);
  lines.forEach((l, i) => {
    const at = START + i * LINE;
    tl.to(sing, { y: () => centre(l), duration: LINE * 0.45, ease: "power2.inOut" }, at - LINE * 0.2)
      .to(l, { opacity: 1, filter: "blur(0px)", scale: 1, duration: LINE * 0.3 }, at - LINE * 0.15)
      .to($$(".w", l), { opacity: 1, duration: LINE * 0.12, stagger: (LINE * 0.6) / Math.max(1, $$(".w", l).length) }, at);
    if (i < lines.length - 1) tl.to(l, { opacity: 0.26, filter: "blur(2.5px)", scale: 0.94, duration: LINE * 0.3 }, at + LINE * 0.8);
  });
  tl.to({}, { duration: 0.3 }); // the last line holds a moment before the pin lets go
  return () => { tl.scrollTrigger?.kill(); tl.kill(); gsap.set([frame, sing, stage, fill, ".ly-np", ...lines, ...$$(".sing .w"), ".ly-frame img"], { clearProps: "all" }); };
}

function looksPin() {
  const track = $(".lk-track"), cards = $$(".lk-card");
  const dist = () => track.scrollWidth - window.innerWidth + 80;
  let last = -1;
  const tw = gsap.to(track, {
    x: () => -dist(), ease: "none",
    scrollTrigger: {
      trigger: ".looks", pin: ".lk-pin", start: "top top", end: () => "+=" + dist(), scrub: 0.8, invalidateOnRefresh: true,
      onToggle: (self) => own("looks", self.isActive, cards[Math.max(last, 0)].dataset.say),
      onUpdate: (self) => {
        const i = Math.min(cards.length - 1, Math.round(self.progress * (cards.length - 1)));
        if (i !== last) { last = i; if (owner === "looks") { setSay(cards[i].dataset.say); tick(1300); } }
      },
    },
  });
  return () => { tw.scrollTrigger?.kill(); tw.kill(); gsap.set(track, { clearProps: "all" }); };
}

function wallScroll() {
  const tw = gsap.fromTo(".wall-grid", { yPercent: 0, rotationX: 34 }, { yPercent: -38, rotationX: 16, ease: "none", scrollTrigger: { trigger: ".wall", start: "top bottom", end: "bottom top", scrub: true } });
  return () => { tw.scrollTrigger?.kill(); tw.kill(); };
}

/* ---------- the screenshot viewer ---------- */
(() => {
  const dlg = $("#viewer");
  if (!dlg || typeof dlg.showModal !== "function") return;
  const img = $("img", dlg), cap = $(".cap", dlg), count = $(".count", dlg);
  const all = $$(".shot button");
  const seenSrc = new Set();
  const shots = all.filter((x) => { const s = $("img", x).getAttribute("src"); return !seenSrc.has(s) && seenSrc.add(s); });
  let at = 0, opener = null;
  const show = (i) => {
    at = (i + shots.length) % shots.length;
    const src = $("img", shots[at]);
    img.src = src.currentSrc || src.src; img.alt = src.alt;
    cap.textContent = "";
    const b = document.createElement("b"); b.textContent = shots[at].dataset.title || "";
    cap.append(b, " ", shots[at].dataset.cap || "");
    count.textContent = `${at + 1} / ${shots.length}`;
  };
  all.forEach((btn) => btn.addEventListener("click", () => {
    opener = btn;
    const s = $("img", btn).getAttribute("src");
    show(Math.max(0, shots.findIndex((x) => $("img", x).getAttribute("src") === s)));
    lenis?.stop(); dlg.showModal(); tick(1000);
  }));
  $(".prev", dlg).addEventListener("click", () => show(at - 1));
  $(".next", dlg).addEventListener("click", () => show(at + 1));
  $(".close", dlg).addEventListener("click", () => dlg.close());
  dlg.addEventListener("close", () => { img.removeAttribute("src"); lenis?.start(); opener?.focus({ preventScroll: true }); });
  dlg.addEventListener("keydown", (e) => {
    if (e.key === "ArrowRight") { e.preventDefault(); show(at + 1); }
    else if (e.key === "ArrowLeft") { e.preventDefault(); show(at - 1); }
  });
  $(".vstage", dlg).addEventListener("click", (e) => { if (e.target !== img) dlg.close(); });
  const SWIPE = 48;
  let x0 = null, y0 = 0;
  dlg.addEventListener("pointerdown", (e) => { if (e.pointerType !== "mouse") { x0 = e.clientX; y0 = e.clientY; } });
  dlg.addEventListener("pointerup", (e) => {
    if (x0 === null) return;
    const dx = e.clientX - x0, dy = e.clientY - y0;
    x0 = null;
    if (Math.abs(dx) > SWIPE && Math.abs(dx) > Math.abs(dy) * 1.5) show(at + (dx < 0 ? 1 : -1));
  });
})();

/* ---------- the cursor: a star that leads, a ring that follows ---------- */
if (moving && matchMedia("(pointer: fine) and (hover: hover)").matches) {
  const cur = $(".cursor"), star = $(".c-star"), ring = $(".c-ring"), label = $(".c-label");
  root.classList.add("has-cursor");
  const sx = gsap.quickTo(star, "x", { duration: 0.12, ease: "power3" }), sy = gsap.quickTo(star, "y", { duration: 0.12, ease: "power3" });
  const rx = gsap.quickTo(ring, "x", { duration: 0.5, ease: "power3" }), ry = gsap.quickTo(ring, "y", { duration: 0.5, ease: "power3" });
  const spin = gsap.quickTo(star, "rotation", { duration: 0.6, ease: "power3" });
  let lx = 0, turn = 0;
  window.addEventListener("pointermove", (e) => {
    sx(e.clientX); sy(e.clientY); rx(e.clientX); ry(e.clientY);
    turn += (e.clientX - lx) * 0.6; lx = e.clientX; spin(turn); // the star rolls with the motion
    cur.classList.remove("gone");
    const t = e.target.closest?.(".shot button, a, button, [role=tab], kbd");
    const view = !!t?.matches(".shot button");
    cur.classList.toggle("view", view);
    cur.classList.toggle("link", !!t && !view);
    if (view) label.textContent = "View";
  }, { passive: true });
  window.addEventListener("pointerdown", () => cur.classList.add("down"));
  window.addEventListener("pointerup", () => cur.classList.remove("down"));
  document.addEventListener("pointerleave", () => cur.classList.add("gone"));
  document.documentElement.addEventListener("mouseleave", () => cur.classList.add("gone"));
}
