// the long pill's editor, on the page: tiles in, pill out. sample values; the clock and date are the visitor's own
(() => {
  const root = document.getElementById("build");
  if (!root) return;
  const lp = root.querySelector("#lp"), stage = root.querySelector(".b-stage"), where = root.querySelector(".b-where");
  const zones = { l: lp.querySelector(".lp-l"), m: lp.querySelector(".lp-m"), r: lp.querySelector(".lp-r") };
  const tilesEl = root.querySelector(".tiles"), hint = root.querySelector(".b-hint");
  const still = matchMedia("(prefers-reduced-motion: reduce)").matches;
  const HINT = "Click a tile to put it in the pill, or take it out.";

  let cpu = 14, net = 2, timer = 25 * 60 - 48, li = 0, singing = false;
  const time = () => new Date().toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
  const date = () => new Date().toLocaleDateString([], { weekday: "short", day: "numeric", month: "short" });
  const mmss = (s) => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
  // what each item is, what it shows now, and the hint under the grid (the app's own wording where it has one)
  const ITEMS = [
    { k: "clock", name: "Clock", icon: "clock", val: time, hint: "The time, in the middle unless you move it." },
    { k: "date", name: "Date", icon: "date", val: date, hint: "Today's date, short." },
    { k: "weather", name: "Weather", icon: "weather", val: () => "21°", hint: "The temperature where you are, or any city you type in Settings." },
    { k: "music", name: "Music", icon: "note", val: () => "Afterglow", hint: "The song playing, from any app that talks to Windows' media controls." },
    { k: "lyrics", name: "Lyrics", icon: "lyrics", val: () => "Sings along", hint: "While a line is sung it takes the middle's place. Between lines the middle comes back." },
    { k: "cover", name: "Cover", icon: "cover", val: () => "Now playing", hint: "The album art, small." },
    { k: "bars", name: "Visualizer", icon: "bars", val: () => "Now playing", hint: "Four bars that move with the music." },
    { k: "timer", name: "Timer", icon: "timer", val: () => mmss(timer), hint: "A running timer, stopwatch or focus session, counting." },
    { k: "volume", name: "Volume", icon: "volume", val: () => "70%", hint: "The output volume." },
    { k: "wifi", name: "Wi-Fi", icon: "wifi", val: () => "Online", hint: "Whether you're connected." },
    { k: "cpu", name: "CPU", icon: "cpu", val: () => cpu + "%", hint: "How busy the processor is." },
    { k: "gpu", name: "GPU", icon: "gpu", val: () => "6%", hint: "How busy the graphics are." },
    { k: "ram", name: "RAM", icon: "ram", val: () => "61%", hint: "Memory in use." },
    { k: "net", name: "Network", icon: "lan", val: () => `↓${net}K ↑0K`, hint: "Download and upload, right now." },
    { k: "disk", name: "Disk", icon: "disk", val: () => "70%", hint: "How full the system drive is." },
    { k: "battery", name: "Battery", icon: "battery", val: () => "84%", hint: "Charge left, on a laptop." },
    { k: "lang", name: "Language", icon: "lang", val: () => "EN", hint: "The keyboard language." },
    { k: "window", name: "Window", icon: "window", val: () => "Notes", hint: "The app in front." },
    { k: "markets", name: "Markets", icon: "trend", val: () => "S&P +0.4%", hint: "The S&P 500, while the market is open." },
    { k: "score", name: "Score", icon: "trophy", val: () => "2–1  67′", hint: "A live game you follow, with its clock." },
  ];
  const byKey = Object.fromEntries(ITEMS.map((i) => [i.k, i]));
  const LINES = ["Words light up as they're sung", "the middle steps aside for them", "and comes back between lines"];
  const DEFAULTS = {
    desk: { size: "long", items: ["music", "bars", "clock", "lyrics", "cpu", "net"] },
    apps: { size: "regular", items: ["music", "bars", "clock", "battery"] },
  };
  const ACCENTS = [["Violet", "#9f8cff"], ["Red", "#ff3b3b"], ["Pink", "#ff6fae"], ["Blue", "#4f9dff"], ["Mint", "#3fd6a8"], ["Amber", "#ffb547"], ["White", "#ececf1"]];
  const clone = (o) => JSON.parse(JSON.stringify(o));
  let state = { place: "desk", dock: "floating", body: "accent", sets: clone(DEFAULTS) };
  const set = () => state.sets[state.place];

  const svg = (id) => `<svg aria-hidden="true"><use href="#i-${id}"/></svg>`;
  const piece = (k) => {
    if (k === "cover") return `<span class="lp-i lp-cover" data-k="cover"></span>`;
    if (k === "bars") return `<span class="lp-i viz" data-k="bars"><i></i><i></i><i></i><i></i></span>`;
    const it = byKey[k];
    return `<span class="lp-i" data-k="${k}">${svg(it.icon)}<b>${it.val()}</b></span>`;
  };

  // the app's layout: the clock holds the middle if it's in, otherwise the middle item does; lyrics borrow the middle while a line is sung
  function render(animate) {
    const w0 = lp.offsetWidth;
    const s = set();
    lp.dataset.size = s.size; lp.dataset.dock = state.dock; lp.dataset.body = state.body;
    stage.dataset.place = state.place;
    where.textContent = state.place === "desk" ? "On the desktop" : "Over your apps";
    if (s.size === "regular") {
      zones.l.innerHTML = `<span class="lp-i lp-cover"></span><span class="lp-song"><b>Afterglow</b><small>Starfall Sessions</small></span>`;
      zones.m.innerHTML = ""; zones.r.innerHTML = `<span class="lp-i viz"><i></i><i></i><i></i><i></i></span>`;
    } else {
      const shown = s.items.filter((k) => k !== "lyrics");
      const mid = shown.includes("clock") ? shown.indexOf("clock") : Math.floor((shown.length - 1) / 2);
      zones.l.innerHTML = shown.slice(0, Math.max(0, mid)).map(piece).join("");
      zones.r.innerHTML = shown.slice(mid + 1).map(piece).join("");
      const sung = s.items.includes("lyrics") && singing;
      zones.m.innerHTML = sung ? `<span class="lp-i lp-line" data-k="line"><b>${LINES[li]}</b></span>` : shown.length ? piece(shown[mid]) : `<span class="lp-empty">Empty</span>`;
    }
    lp.setAttribute("aria-label", s.size === "regular" ? "A preview of the regular pill" : `A preview of the long pill showing ${s.items.map((k) => byKey[k].name).join(", ") || "nothing"}`);
    tilesEl.querySelectorAll(".tile").forEach((t) => {
      const on = s.items.includes(t.dataset.k);
      t.setAttribute("aria-pressed", on);
      t.querySelector("small").textContent = byKey[t.dataset.k].val();
    });
    root.querySelectorAll(".seg").forEach((g) => {
      const v = g.dataset.key === "size" ? s.size : state[g.dataset.key];
      g.querySelectorAll("button").forEach((b) => b.setAttribute("aria-pressed", b.dataset.v === v));
    });
    tilesEl.classList.toggle("off", s.size === "regular");
    const w1 = lp.offsetWidth;
    if (animate && !still && w0 && w0 !== w1) lp.animate([{ width: w0 + "px" }, { width: w1 + "px" }], { duration: 420, easing: "cubic-bezier(.16,1,.3,1)" });
  }

  // tiles: built once, kept in sync
  tilesEl.innerHTML = ITEMS.map((i) => `<button type="button" class="tile" data-k="${i.k}" aria-pressed="false">${svg(i.icon)}<span>${i.name}</span><small></small></button>`).join("");
  tilesEl.addEventListener("click", (e) => {
    const t = e.target.closest(".tile"); if (!t || set().size === "regular") return;
    const items = set().items, k = t.dataset.k, at = items.indexOf(k);
    if (at >= 0) items.splice(at, 1); else items.push(k);
    render(true);
  });
  const showHint = (e) => { const t = e.target.closest?.(".tile"); hint.textContent = t ? byKey[t.dataset.k].hint : HINT; };
  tilesEl.addEventListener("pointerover", showHint);
  tilesEl.addEventListener("focusin", showHint);
  tilesEl.addEventListener("pointerleave", () => (hint.textContent = HINT));

  root.querySelectorAll(".seg").forEach((g) => g.addEventListener("click", (e) => {
    const b = e.target.closest("button"); if (!b) return;
    if (g.dataset.key === "size") set().size = b.dataset.v; else state[g.dataset.key] = b.dataset.v;
    render(true);
  }));
  root.querySelector(".b-reset").addEventListener("click", () => { state.sets[state.place] = clone(DEFAULTS[state.place]); render(true); });

  // accent: the whole page takes it, like the app's custom look
  const sw = root.querySelector(".swatches");
  const luma = (hex) => { const n = parseInt(hex.slice(1), 16); return (0.299 * (n >> 16) + 0.587 * ((n >> 8) & 255) + 0.114 * (n & 255)) / 255; };
  const paint = (hex) => {
    document.documentElement.style.setProperty("--accent", hex);
    document.documentElement.style.setProperty("--on-accent", luma(hex) > 0.6 ? "#0b0b0e" : "#fff");
    sw.querySelectorAll("button").forEach((b) => b.setAttribute("aria-pressed", b.dataset.c === hex));
    try { localStorage.setItem("astraeus-accent", hex); } catch {}
  };
  sw.innerHTML = ACCENTS.map(([n, c]) => `<button type="button" data-c="${c}" style="--c:${c}" aria-label="${n}" aria-pressed="false"></button>`).join("");
  sw.addEventListener("click", (e) => { const b = e.target.closest("button"); if (b) paint(b.dataset.c); });
  let saved = null;
  try { saved = localStorage.getItem("astraeus-accent"); } catch {}
  paint(ACCENTS.some(([, c]) => c === saved) ? saved : ACCENTS[0][1]);

  render(false);

  // the live parts: the minute, a restless cpu, the timer, and a line sung now and then.
  // only the words change each second; the pill is rebuilt when a line comes or goes, so the bars keep moving
  const TICK = 1000, LINE_ON = 3, LINE_OFF = 2; // seconds a line is sung, then rests
  let n = 0;
  setInterval(() => {
    n++;
    timer = Math.max(0, timer - 1);
    if (n % 2 === 0) {
      cpu = Math.max(4, Math.min(38, cpu + Math.round((Math.random() - 0.5) * 8)));
      net = Math.max(0, Math.min(9, net + Math.round((Math.random() - 0.5) * 4)));
    }
    const beat = n % (LINE_ON + LINE_OFF);
    const was = singing;
    if (beat === 0) { singing = true; li = (li + 1) % LINES.length; }
    else if (beat === LINE_ON) singing = false;
    if (was !== singing || beat === 0) { render(true); return; } // the pill eases to its new length, like the app
    lp.querySelectorAll(".lp-i[data-k] b").forEach((b) => { const it = byKey[b.parentElement.dataset.k]; if (it) b.textContent = it.val(); });
    tilesEl.querySelectorAll(".tile").forEach((t) => (t.querySelector("small").textContent = byKey[t.dataset.k].val()));
  }, TICK);
})();
