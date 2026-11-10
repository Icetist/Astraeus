// two small things: the screenshot viewer, and the looks switcher. nothing else moves
document.documentElement.classList.remove("no-js");

(() => {
  const dlg = document.getElementById("viewer");
  if (!dlg || typeof dlg.showModal !== "function") return; // no dialog support: the shots just stay where they are
  const img = dlg.querySelector("img"), cap = dlg.querySelector(".cap"), count = dlg.querySelector(".count");
  const shots = [...document.querySelectorAll(".shot button")];
  let at = 0, opener = null;

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

  shots.forEach((b, i) => b.addEventListener("click", () => { opener = b; show(i); dlg.showModal(); }));
  dlg.querySelector(".prev").addEventListener("click", () => show(at - 1));
  dlg.querySelector(".next").addEventListener("click", () => show(at + 1));
  dlg.querySelector(".close").addEventListener("click", () => dlg.close());
  dlg.addEventListener("close", () => { img.removeAttribute("src"); opener?.focus(); });
  dlg.addEventListener("keydown", (e) => {
    if (e.key === "ArrowRight") { e.preventDefault(); show(at + 1); }
    else if (e.key === "ArrowLeft") { e.preventDefault(); show(at - 1); }
  });
  dlg.querySelector(".stage").addEventListener("click", (e) => { if (e.target !== img) dlg.close(); }); // the dark around the picture closes it

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

(() => {
  const list = document.querySelector(".tabs");
  if (!list) return;
  const tabs = [...list.querySelectorAll("[role=tab]")];
  const pick = (t, focus) => {
    tabs.forEach((x) => {
      const on = x === t;
      x.setAttribute("aria-selected", on);
      x.tabIndex = on ? 0 : -1;
      document.getElementById(x.getAttribute("aria-controls")).hidden = !on;
    });
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
