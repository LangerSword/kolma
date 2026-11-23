/* Deterministic 30 s kinetic timeline. Every visible value is f(t). */
(function () {
  const clamp = v => Math.max(0, Math.min(1, v));
  const easeOut = p => 1 - Math.pow(1 - clamp(p), 3);
  const rise = (t, a, d) => easeOut((t - a) / d);
  const scenes = Array.from(document.querySelectorAll('.scene'));
  const $ = id => document.getElementById(id);
  const q = (sel, root) => Array.from((root || document).querySelectorAll(sel));

  const s1 = $('s1'), s2 = $('s2'), s3 = $('s3'), s4 = $('s4'),
        s5 = $('s5'), s6 = $('s6'), s7 = $('s7');
  const s1lines = q('.line', s1), s1rule = q('.rule', s1)[0];
  const chips = q('.chip', s2);
  const shot = q('.shot', s3)[0], s3cap = q('.caption', s3)[0];
  const s4big = q('.big', s4)[0], s4rows = q('.rows .row', s4), s4foot = q('.foot', s4)[0];
  const s5big = q('.big', s5)[0], s5speed = q('.speed', s5)[0],
        s5rows = q('.cmp .row', s5), s5bars = q('.cmp .bar i', s5),
        s5tag = q('.tag', s5)[0], s5cap = q('.caption', s5)[0];
  const panels = q('.panel', s6);
  const lock = q('.lock', s7)[0], url = q('.url', s7)[0], tag7 = q('.tag', s7)[0];
  const fill = $('fill'), beatno = $('beatno');
  const bounds = [3.4, 7.0, 11.2, 16.6, 21.2, 25.6, 30.001];

  function render(t) {
    t = Math.max(0, Math.min(30, +t || 0));
    window.__t = t;

    scenes.forEach((el, i) => {
      const a = +el.dataset.a, b = +el.dataset.b;
      const pin = clamp((t - a) / 0.38);
      const pout = i === scenes.length - 1 ? 1 : clamp((b - t) / 0.38);
      el.style.opacity = String(Math.min(pin, pout));
    });

    // 1 — problem
    const p1 = rise(t, 0.04, 0.62);
    s1lines[0].style.transform = `translateY(${(1 - p1) * 58}px)`;
    s1lines[1].style.transform = `translateY(${(1 - p1) * 58 + (1 - p1) * 14}px)`;
    s1rule.style.transform = `scaleX(${rise(t, 0.5, 0.85)})`;

    // 2 — pain: eight real registry names
    chips.forEach((el, i) => {
      const p = rise(t, 3.6 + i * 0.085, 0.42);
      el.style.opacity = String(p);
      el.style.transform = `translateY(${(1 - p) * 32}px) scale(${0.965 + 0.035 * p})`;
    });

    // 3 — product: real benchmark screenshot
    const p3 = rise(t, 7.04, 1.1);
    shot.style.opacity = String(p3);
    shot.style.transform = `scale(${0.9 + 0.1 * p3}) rotateY(${(1 - p3) * -7}deg) translateY(${(1 - p3) * 26}px)`;
    s3cap.style.opacity = String(clamp((t - 8.4) / 0.5));

    // 4 — proof: real CLI readout
    const p4 = rise(t, 11.24, 0.8);
    s4big.style.transform = `scale(${0.9 + 0.1 * p4})`;
    s4rows.forEach((el, i) => {
      const p = rise(t, 11.5 + i * 0.11, 0.44);
      el.style.opacity = String(p);
      el.style.transform = `translateX(${(1 - p) * 26}px)`;
    });
    s4foot.style.opacity = String(clamp((t - 12.3) / 0.55));

    // 5 — new algorithm, measured numbers
    const p5 = rise(t, 16.64, 0.78);
    s5big.style.transform = `scale(${0.9 + 0.1 * p5})`;
    s5speed.style.transform = `translateX(${(1 - p5) * 34}px)`;
    s5speed.style.opacity = String(p5);
    s5rows.forEach((el, i) => {
      const p = rise(t, 17.05 + i * 0.13, 0.45);
      el.style.opacity = String(p);
      el.style.transform = `translateX(${(1 - p) * 22}px)`;
      s5bars[i].style.width = (parseFloat(s5bars[i].dataset.w) * rise(t, 17.15 + i * 0.13, 0.8)).toFixed(2) + '%';
    });
    s5tag.style.opacity = String(clamp((t - 17.6) / 0.6));
    s5cap.style.opacity = String(clamp((t - 18.0) / 0.6));

    // 6 — three real surfaces
    panels.forEach((el, i) => {
      const p = rise(t, 21.34 + i * 0.16, 0.6);
      el.style.opacity = String(p);
      el.style.transform = `translateY(${(1 - p) * 40}px) scale(${0.975 + 0.025 * p})`;
    });

    // 7 — lockup
    const p7 = rise(t, 25.64, 0.85);
    lock.style.transform = `scale(${0.92 + 0.08 * p7}) translateY(${(1 - p7) * 30}px)`;
    url.style.opacity = String(clamp((t - 26.15) / 0.55));
    tag7.style.opacity = String(clamp((t - 26.7) / 0.55));

    // hud
    fill.style.width = ((t / 30) * 100).toFixed(3) + '%';
    let idx = bounds.findIndex(b => t < b);
    if (idx < 0) idx = 6;
    beatno.textContent = String(idx + 1).padStart(2, '0') + ' / 07';
  }

  let raf = 0, startMs = 0, running = false;
  function loop(now) {
    if (!running) return;
    const t = (now - startMs) / 1000;
    if (t >= 30.02) {
      render(30);
      running = false;
      window.__filmDone = true;
      return;
    }
    render(t);
    raf = requestAnimationFrame(loop);
  }

  window.__render = render;
  window.__startFilm = function () {
    cancelAnimationFrame(raf);
    running = true;
    window.__filmDone = false;
    startMs = performance.now();
    render(0);
    raf = requestAnimationFrame(loop);
  };
  render(0);
  window.__filmReady = true;
})();
