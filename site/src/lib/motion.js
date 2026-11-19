import { gsap } from 'gsap';

// One load director for the hero; IntersectionObserver-driven reveals for the
// sections (reliable under fast or programmatic scroll), animated with GSAP.
// Motion collapses to static under prefers-reduced-motion, and a safety net
// guarantees no section is ever left hidden.
export function initMotion(root) {
  if (!root || typeof window === 'undefined') return () => {};
  const reduce = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  if (reduce) return () => {};

  let safety;
  let io;
  const ctx = gsap.context(() => {
    // Hero load sequence (not scroll-gated, so first paint always resolves).
    gsap.from('.hero-copy > *', {
      autoAlpha: 0, y: 26, duration: 0.8, stagger: 0.09, ease: 'power3.out', clearProps: 'transform'
    });
    gsap.from('.hero-visual', {
      autoAlpha: 0, y: 30, scale: 0.985, duration: 1, delay: 0.2, ease: 'power3.out', clearProps: 'transform'
    });

    const items = gsap.utils.toArray('.reveal');
    if (items.length) {
      gsap.set(items, { autoAlpha: 0, y: 28 });
      io = new IntersectionObserver((entries) => {
        const visible = entries.filter((e) => e.isIntersecting).map((e) => e.target);
        if (visible.length) {
          gsap.to(visible, { autoAlpha: 1, y: 0, duration: 0.7, stagger: 0.06, ease: 'power3.out', overwrite: true });
          visible.forEach((el) => io.unobserve(el));
        }
      }, { threshold: 0.1, rootMargin: '0px 0px -6% 0px' });
      items.forEach((el) => io.observe(el));
      // Safety net: anything still hidden after the page settles is revealed.
      safety = window.setTimeout(() => {
        gsap.to(items, { autoAlpha: 1, y: 0, duration: 0.4, overwrite: 'auto' });
      }, 2500);
    }
  }, root);

  return () => {
    if (safety) window.clearTimeout(safety);
    if (io) io.disconnect();
    ctx.revert();
  };
}
