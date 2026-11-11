// Docs chrome: collapse the page menu on small screens and add copy buttons to code blocks.
const menu = document.querySelector('.docs-side');
if (menu && window.matchMedia('(max-width: 900px)').matches) menu.removeAttribute('open');

for (const figure of document.querySelectorAll('figure.code')) {
  const caption = figure.querySelector('figcaption');
  const code = figure.querySelector('pre');
  if (!caption || !code) continue;
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'copy';
  button.textContent = 'Copy';
  button.addEventListener('click', async () => {
    try {
      await navigator.clipboard.writeText(code.textContent);
      button.textContent = 'Copied';
    } catch {
      button.textContent = 'Select to copy';
    }
    setTimeout(() => { button.textContent = 'Copy'; }, 2000);
  });
  caption.append(button);
}
