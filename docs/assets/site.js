// the viewer, the rail story, the sung lines and the looks. each only follows the scroll or a click; nothing runs on its own
const root = document.documentElement;
root.classList.remove("no-js");
root.classList.add("js");

(() => { // screenshot viewer
  const dlg = document.getElementById("viewer");
  if (!dlg || typeof dlg.showModal !== "function") return; // no dialog support: the shots stay where they are
  const img = dlg.querySelector("img"), cap = dlg.querySelector(".cap"), count = dlg.querySelector(".count");
  const all = [...document.querySelectorAll(".shot button")];
  let shots = all, at = 0, opener = null;

  const show = (i) => {
    at = (i + shots.length) % shots.length;
    const src = shots[at].querySelector("img");
    img.src = src.currentSrc || src.src;
    img.alt = src.alt;
    cap.textContent = "";
    const t = shots[at].dataset.title;
    if (t) { const b = document.createElement("b"); b.textContent = t; cap.append(b, " "); }
    cap.append(document.createTextNode(shots[at].dataset.cap || ""));
    count.textContent = `${at + 1} / ${shots.length}`;
  };
  const seen = new Set(); // each screenshot once, even where the layout keeps a second copy
  shots = all.filter((x) => { const s = x.querySelector("img").src; return !seen.has(s) && seen.add(s); });
  const open = (b, from) => {
    opener = from || b;
    const s = b.querySelector("img").src;
    show(Math.max(0, shots.findIndex((x) => x.querySelector("img").src === s)));
    dlg.showModal();
  };
  all.forEach((b) => b.addEventListener("click", () => open(b)));
  const screen = document.querySelector("button.frames"); // the rail story's screen opens whichever page it shows
  screen?.addEventListener("click", () => {
    const k = screen.querySelector(".frame.on")?.dataset.k || "home";
    const b = document.querySelector(`.step[data-k="${k}"] .shot button`);
    if (b) open(b, screen);
  });
  dlg.querySelector(".prev").addEventListener("click", () => show(at - 1));
  dlg.querySelector(".next").addEventListener("click", () => show(at + 1));
  dlg.querySelector(".close").addEventListener("click", () => dlg.close());
  dlg.addEventListener("close", () => { img.removeAttribute("src"); opener?.focus(); });
  dlg.addEventListener("keydown", (e) => {
    if (e.key === "ArrowRight") { e.preventDefault(); show(at + 1); }
    else if (e.key === "ArrowLeft") { e.preventDefault(); show(at - 1); }
  });
  dlg.querySelector(".vstage").addEventListener("click", (e) => { if (e.target !== img) dlg.close(); });

  const SWIPE = 48; // px sideways before a swipe counts
  let x0 = null, y0 = 0;
  dlg.addEventListener("pointerdown", (e) => { if (e.pointerType !== "mouse") { x0 = e.clientX; y0 = e.clientY; } });
  dlg.addEventListener("pointerup", (e) => {
    if (x0 === null) return;
    const dx = e.clientX - x0, dy = e.clientY - y0;
    x0 = null;
    if (Math.abs(dx) > SWIPE && Math.abs(dx) > Math.abs(dy) * 1.5) show(at + (dx < 0 ? 1 : -1));
  });
})();

// whichever item crosses the middle band of the screen is the lit one
const lightUp = (items, onPick) => {
  if (!items.length || !("IntersectionObserver" in window)) return;
  const io = new IntersectionObserver((es) => {
    es.forEach((e) => { if (e.isIntersecting) onPick(e.target); });
  }, { rootMargin: "-45% 0px -45% 0px" });
  items.forEach((i) => io.observe(i));
  onPick(items[0]);
};

(() => { // the rail story: the screen shows the page being read, the rail lights its icon
  const steps = [...document.querySelectorAll(".step")];
  const frames = [...document.querySelectorAll(".frames .frame")];
  const icons = [...document.querySelectorAll(".mini-rail li")];
  lightUp(steps, (s) => {
    const k = s.dataset.k, r = s.dataset.r || k; // a panel behind home keeps home lit on the rail
    steps.forEach((x) => x.classList.toggle("on", x === s));
    frames.forEach((f) => f.classList.toggle("on", f.dataset.k === k));
    icons.forEach((i) => i.classList.toggle("on", i.dataset.k === r));
  });
})();

(() => { // the sung lines: one in focus, the rest soft, like the fullscreen player
  const lines = [...document.querySelectorAll(".sing li")];
  lightUp(lines, (l) => lines.forEach((x) => x.classList.toggle("on", x === l)));
})();

(() => { // looks: the tabs switch the shot and the section's own look
  const list = document.querySelector(".tabs");
  if (!list) return;
  const sec = list.closest(".looks");
  const tabs = [...list.querySelectorAll("[role=tab]")];
  const pick = (t, focus) => {
    tabs.forEach((x) => {
      const on = x === t;
      x.setAttribute("aria-selected", on);
      x.tabIndex = on ? 0 : -1;
      document.getElementById(x.getAttribute("aria-controls")).hidden = !on;
    });
    sec.dataset.look = t.dataset.look;
    if (focus) t.focus();
  };
  tabs.forEach((t, i) => {
    t.addEventListener("click", () => pick(t));
    t.addEventListener("keydown", (e) => {
      const step = e.key === "ArrowRight" ? 1 : e.key === "ArrowLeft" ? -1 : 0;
      if (step) { e.preventDefault(); pick(tabs[(i + step + tabs.length) % tabs.length], true); }
      else if (e.key === "Home") { e.preventDefault(); pick(tabs[0], true); }
      else if (e.key === "End") { e.preventDefault(); pick(tabs[tabs.length - 1], true); }
    });
  });
})();
