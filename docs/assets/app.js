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
  $('meta[name="theme-color"]').content = dark ? "#070708" : "#f3f3f0";
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
let said = "";
function setSay(html) {
  if (!say || !html || html === said) return;
  said = html;
  const old = say.querySelector(".p-line:last-child"), next = document.createElement("span");
  next.className = "p-line"; next.innerHTML = html;
  if (!moving) { say.replaceChildren(next); return; }
  say.style.position = "relative";
  next.style.cssText = "position:absolute;left:0;right:0";
  say.append(next);
  gsap.fromTo(next, { yPercent: 90, opacity: 0, filter: "blur(4px)" }, { yPercent: 0, opacity: 1, filter: "blur(0px)", duration: 0.55, ease: "expo.out", onComplete: () => { next.style.cssText = ""; } });
  if (old) gsap.to(old, { yPercent: -90, opacity: 0, filter: "blur(4px)", duration: 0.35, ease: "power2.in", onComplete: () => old.remove() });
}

/* ---------- liquid chrome: the logo's star ---------- */
function metal(el, scale) {
  if (!el) return;
  const img = new Image();
  img.onload = () => {
    try {
      new ShaderMount(el, liquidMetalFragmentShader, {
        u_image: img, u_isImage: true,
        u_colorBack: color("rgba(0,0,0,0)"), u_colorTint: color("#ffffff"),
        u_repetition: 2, u_softness: 0.1, u_shiftRed: 0.3, u_shiftBlue: 0.3, u_distortion: 0.07, u_contour: 0.4, u_angle: 70, u_shape: 0,
        u_fit: 1, u_scale: scale, u_rotation: 0, u_offsetX: 0, u_offsetY: 0, u_originX: 0.5, u_originY: 0.5, u_worldWidth: 0, u_worldHeight: 0,
      }, { premultipliedAlpha: false }, still ? 0 : 1, 0, 2, undefined, ["u_image"]);
    } catch { el.classList.add("no-gl"); } // no webgl: the static star from css instead
  };
  img.onerror = () => el.classList.add("no-gl");
  img.src = "assets/img/star-metal.png";
}
metal($("#metal"), 0.9);
metal($("#metal2"), 0.9);

/* ---------- smooth scroll ---------- */
let lenis = null;
if (moving && Lenis) {
  lenis = new Lenis({ lerp: 0.09, smoothWheel: true });
  lenis.on("scroll", ScrollTrigger.update);
  gsap.ticker.add((t) => lenis.raf(t * 1000));
  gsap.ticker.lagSmoothing(0);
}
$$('a[href^="#"]').forEach((a) => a.addEventListener("click", (e) => {
  const id = a.getAttribute("href"); if (id.length < 2) return;
  const el = $(id); if (!el) return;
  e.preventDefault();
  if (lenis) lenis.scrollTo(id === "#top" ? 0 : el, { duration: 1.4 }); else el.scrollIntoView();
  history.replaceState(null, "", id);
}));

/* ---------- the intro: press it open ---------- */
const intro = $("#intro");
let opened = false, seen = false;
try { seen = sessionStorage.getItem("astraeus-intro") === "1"; } catch {}
function heroIn() {
  if (!moving) return;
  gsap.timeline({ defaults: { ease: "expo.out" } })
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
  try { sessionStorage.setItem("astraeus-intro", "1"); } catch {}
  window.removeEventListener("keydown", onKey);
  if (!intro) return;
  if (fast || !moving) { intro.remove(); lenis?.start(); heroIn(); return; }
  tick(520, 0.25, 0.07);
  const star = $("#metal"), mark = $(".p-brand svg");
  const pr = mark.getBoundingClientRect(), sr = star.getBoundingClientRect();
  gsap.timeline({ onComplete: () => { intro.remove(); lenis?.start(); } })
    .to(".intro-ui, .intro-skip", { opacity: 0, y: 10, duration: 0.3 })
    // the star flies up into the pill's logo
    .to(star, { x: pr.left + pr.width / 2 - (sr.left + sr.width / 2), y: pr.top + pr.height / 2 - (sr.top + sr.height / 2), scale: pr.height / sr.height, duration: 1.05, ease: "expo.inOut" }, 0.1)
    .to(intro, { backgroundColor: "rgba(0,0,0,0)", duration: 0.6 }, 0.55)
    .add(heroIn, 0.65);
}
if (intro) {
  if (seen || !moving) open(true);
  else {
    lenis?.stop();
    gsap.fromTo("#metal", { scale: 0.6, opacity: 0 }, { scale: 1, opacity: 1, duration: 1.6, ease: "expo.out" });
    gsap.fromTo(".intro-ui", { opacity: 0, y: 14 }, { opacity: 1, y: 0, duration: 1, delay: 0.5, ease: "expo.out" });
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
  $$("main > section[data-say]").forEach((s) => ScrollTrigger.create({ trigger: s, start: "top 55%", end: "bottom 45%", onToggle: (st) => st.isActive && setSay(s.dataset.say) }));
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
    ScrollTrigger.refresh();
    return () => { root.classList.remove("js-pin"); kill.forEach((k) => k && k()); };
  });
}

function storyPin() {
  const steps = $$(".st-steps li"), N = steps.length;
  const imgs = Object.fromEntries($$(".st-panel img").map((i) => [i.dataset.k, i]));
  const cards = $$(".st-cards img");
  const title = $(".st-title span"), text = $(".st-text"), how = $(".st-how"), num = $(".st-num");
  const dots = $(".st-dots");
  dots.innerHTML = steps.map(() => "<li></li>").join("");
  const dotEls = $$("li", dots);
  const fan = [[-31, -34, -7], [31, -36, 6], [-37, 30, 5], [37, 32, -6], [0, 52, 0]]; // where the panels behind home land: x%, y%, tilt
  let cur = -1, shown = "home";
  gsap.set(cards, { xPercent: -50, yPercent: -50 });
  function show(i) {
    if (i === cur) return;
    const s = steps[i], k = s.dataset.k, pk = k === "behind" ? "home" : k, was = cur;
    cur = i;
    dotEls.forEach((d, j) => d.classList.toggle("on", j === i));
    num.textContent = String(i + 1).padStart(2, "0");
    const dir = was < i ? 1 : -1;
    gsap.timeline()
      .to(title, { yPercent: -105 * dir, duration: 0.22, ease: "power2.in" })
      .add(() => { title.textContent = s.querySelector("h3").textContent; })
      .fromTo(title, { yPercent: 105 * dir }, { yPercent: 0, duration: 0.6, ease: "expo.out" });
    text.textContent = s.querySelector("p").textContent;
    how.textContent = s.querySelector(".how")?.textContent || "";
    gsap.fromTo([text, how], { opacity: 0, y: 10 }, { opacity: 1, y: 0, duration: 0.6, ease: "expo.out", stagger: 0.05 });
    if (pk !== shown) {
      gsap.to(imgs[shown], { opacity: 0, duration: 0.45, ease: "power2.out" });
      gsap.fromTo(imgs[pk], { opacity: 0, scale: 1.03, filter: "blur(8px)" }, { opacity: 1, scale: 1, filter: "blur(0px)", duration: 0.8, ease: "expo.out" });
      shown = pk;
    }
    const behind = k === "behind";
    gsap.to(".st-panel", { scale: behind ? 0.9 : 1, opacity: behind ? 0.3 : 1, duration: 0.8, ease: "expo.out" });
    cards.forEach((c, j) => {
      const [x, y, r] = fan[j];
      gsap.to(c, behind
        ? { opacity: 1, xPercent: -50 + x * 2.2, yPercent: -50 + y * 2.2, rotation: r, scale: 1, duration: 0.9, delay: j * 0.06, ease: "expo.out" }
        : { opacity: 0, xPercent: -50, yPercent: -50, rotation: 0, scale: 0.8, duration: 0.45, ease: "power2.in" });
    });
    setSay(s.dataset.say);
    tick(dir > 0 ? 1400 : 1150);
  }
  const st = ScrollTrigger.create({
    trigger: ".story", pin: ".st-pin", start: "top top", end: () => "+=" + (N - 1) * window.innerHeight * 0.55,
    onUpdate: (self) => show(Math.min(N - 1, Math.floor(self.progress * N))),
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
    setSay(`${svgUse(id)}${n} · ${v()}`);
    tick(1700, 0.025, 0.04);
  }
  pick(0);
  const st = ScrollTrigger.create({
    trigger: ".dial-pin", pin: true, start: "top top+=60", end: () => "+=" + window.innerHeight * 1.3,
    onUpdate: (self) => {
      const rot = self.progress * (ITEMS.length - 1) * STEP;
      gsap.set(wheel, { rotation: -rot, svgOrigin: `${C} ${C}` });
      pick(Math.round(rot / STEP));
    },
  });
  return () => st.kill();
}

function lyricsPin() {
  const frame = $(".ly-frame"), lines = $$(".sing li"), sing = $(".sing");
  const w0 = Math.min(window.innerWidth * 0.46, 760);
  const tl = gsap.timeline({ scrollTrigger: { trigger: ".lyrics", pin: ".ly-pin", start: "top top", end: () => "+=" + window.innerHeight * 2, scrub: 0.6, invalidateOnRefresh: true } });
  tl.fromTo(frame, { width: w0, height: w0 * 767 / 1365, borderRadius: 22 }, { width: () => window.innerWidth, height: () => window.innerHeight, borderRadius: 0, duration: 1, ease: "power2.inOut" })
    .fromTo(sing, { opacity: 0 }, { opacity: 1, duration: 0.2 }, 0.85)
    .fromTo(".ly-frame img", { filter: "blur(0px) brightness(1)" }, { filter: "blur(18px) brightness(.55)", duration: 0.35 }, 0.8); // the player steps back so the words can sing over it
  lines.forEach((l, i) => {
    tl.to(l, { opacity: 1, filter: "blur(0px)", duration: 0.25 }, 1 + i * 0.35);
    if (i < lines.length - 1) tl.to(l, { opacity: 0.25, filter: "blur(3px)", duration: 0.25 }, 1 + i * 0.35 + 0.35);
  });
  return () => { tl.scrollTrigger?.kill(); tl.kill(); gsap.set([frame, sing, ...lines, ".ly-frame img"], { clearProps: "all" }); };
}

function looksPin() {
  const track = $(".lk-track"), cards = $$(".lk-card");
  const dist = () => track.scrollWidth - window.innerWidth + 80;
  const smear = { v: 0 };
  const setFilter = () => {
    const x = gsap.utils.clamp(-14, 14, smear.v);
    track.style.filter = Math.abs(x) < 0.3 ? "" : `drop-shadow(${x}px 0 0 rgb(255 40 110 / .45)) drop-shadow(${-x}px 0 0 rgb(0 170 255 / .45))`; // colours part as it slides, light through glass
  };
  let last = -1;
  const tw = gsap.to(track, {
    x: () => -dist(), ease: "none",
    scrollTrigger: {
      trigger: ".looks", pin: ".lk-pin", start: "top top", end: () => "+=" + dist(), scrub: 0.8, invalidateOnRefresh: true,
      onUpdate: (self) => {
        gsap.to(smear, { v: self.getVelocity() / 260, duration: 0.3, overwrite: true, onUpdate: setFilter });
        gsap.to(track, { skewX: gsap.utils.clamp(-5, 5, -self.getVelocity() / 650), duration: 0.4, overwrite: "auto" });
        const i = Math.min(cards.length - 1, Math.round(self.progress * (cards.length - 1)));
        if (i !== last) { last = i; setSay(cards[i].dataset.say); tick(1300); }
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
