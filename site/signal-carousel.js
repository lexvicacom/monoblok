(() => {
  const carousel = document.querySelector('[aria-roledescription="carousel"]');
  if (!carousel) return;

  const slides = [...carousel.querySelectorAll('.signal-slide')];
  const titles = slides.map(slide => slide.getAttribute('aria-label').split(': ')[1]);
  const position = carousel.querySelector('.signal-position');
  let current = 0;

  function move(step) {
    current = (current + step + slides.length) % slides.length;
    slides.forEach((slide, index) => { slide.hidden = index !== current; });
    position.textContent = `${current + 1} / ${slides.length} · ${titles[current]}`;
  }

  carousel.querySelector('.signal-controls').hidden = false;
  carousel.querySelectorAll('[data-signal-step]').forEach(button => {
    button.addEventListener('click', () => move(Number(button.dataset.signalStep)));
  });
  carousel.addEventListener('keydown', event => {
    if (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight') return;
    event.preventDefault();
    move(event.key === 'ArrowRight' ? 1 : -1);
  });

  let touchStart;
  carousel.addEventListener('touchstart', event => {
    const touch = event.changedTouches[0];
    touchStart = { x: touch.clientX, y: touch.clientY };
  }, { passive: true });
  carousel.addEventListener('touchend', event => {
    if (!touchStart) return;
    const touch = event.changedTouches[0];
    const dx = touch.clientX - touchStart.x;
    const dy = touch.clientY - touchStart.y;
    if (Math.abs(dx) > 45 && Math.abs(dx) > Math.abs(dy)) move(dx < 0 ? 1 : -1);
    touchStart = undefined;
  }, { passive: true });
  carousel.addEventListener('touchcancel', () => { touchStart = undefined; });

  // Autoplay: advance every few seconds, but hold still while the visitor is
  // pointing at, focused inside, or not looking at the carousel.
  const AUTOPLAY_MS = 5000;
  if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) return;

  let timer;
  let hovering = false;
  let focused = false;

  function stop() {
    clearInterval(timer);
    timer = undefined;
    // Announce slide changes only when the visitor is the one changing them.
    position.setAttribute('aria-live', 'polite');
  }
  function start() {
    stop();
    if (hovering || focused || document.hidden) return;
    position.setAttribute('aria-live', 'off');
    timer = setInterval(() => move(1), AUTOPLAY_MS);
  }

  carousel.addEventListener('mouseenter', () => { hovering = true; stop(); });
  carousel.addEventListener('mouseleave', () => { hovering = false; start(); });
  carousel.addEventListener('focusin', () => { focused = true; stop(); });
  carousel.addEventListener('focusout', event => {
    if (carousel.contains(event.relatedTarget)) return;
    focused = false;
    start();
  });
  // A swipe should get a full pause before the next automatic advance.
  carousel.addEventListener('touchend', start, { passive: true });
  document.addEventListener('visibilitychange', start);
  start();
})();
