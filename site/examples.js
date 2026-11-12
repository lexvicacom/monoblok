const examples = patchbayExamples;
const tabs = [...document.querySelectorAll('[data-example]')];
const formatSelect = document.getElementById('example-format');
let activeExample = 'clean';
function showExample(key) {
  activeExample = key;
  const example = examples[key];
  const liveLink = document.getElementById('demo-live-link');
  liveLink.href = `demo.html?example=${key}`;
  document.getElementById('demo-live').hidden = example.live === false;
  tabs.forEach(tab => {
    const selected = tab.dataset.example === key;
    tab.setAttribute('aria-selected', String(selected));
    tab.tabIndex = selected ? 0 : -1;
  });
  document.getElementById('example-panel').setAttribute('aria-labelledby', `tab-${key}`);
  const yaml = formatSelect.value === 'yaml';
  document.getElementById('example-thread-operator').textContent = yaml ? 'pipeline' : '->';
  document.getElementById('example-filename').textContent = yaml ? 'patchbay.yml' : 'patchbay.edn';
  renderPatchbayCode(document.getElementById('example-code'), yaml ? example.yaml : example.code);
  document.getElementById('example-title').textContent = example.title;
  document.getElementById('example-description').textContent = example.description;
  document.getElementById('input-label').textContent = example.inputLabel;
  document.getElementById('output-label').textContent = example.outputLabel;
  for (const type of ['input', 'output']) {
    const container = document.getElementById(`example-${type}`);
    container.replaceChildren(...example[type].map(value => {
      const span = document.createElement('span');
      span.textContent = value;
      return span;
    }));
  }
}
tabs.forEach((tab, index) => {
  tab.addEventListener('click', () => showExample(tab.dataset.example));
  tab.addEventListener('keydown', event => {
    let next;
    if (event.key === 'ArrowRight') next = (index + 1) % tabs.length;
    if (event.key === 'ArrowLeft') next = (index + tabs.length - 1) % tabs.length;
    if (event.key === 'Home') next = 0;
    if (event.key === 'End') next = tabs.length - 1;
    if (next !== undefined) {
      event.preventDefault();
      tabs[next].focus();
      showExample(tabs[next].dataset.example);
    }
  });
});
formatSelect.addEventListener('change', () => showExample(activeExample));
showExample(activeExample);
document.querySelectorAll('[data-copy-command]').forEach(copyButton => {
let copyResetTimer;
copyButton.addEventListener('click', async event => {
  const button = event.currentTarget;
  const command = document.getElementById(button.dataset.copyCommand).textContent;
  clearTimeout(copyResetTimer);
  try {
    if (navigator.clipboard && window.isSecureContext) await navigator.clipboard.writeText(command);
    else {
      const area = document.createElement('textarea');
      area.value = command;
      area.style.position = 'fixed';
      area.style.opacity = '0';
      document.body.append(area);
      area.select();
      const copied = document.execCommand('copy');
      area.remove();
      button.focus();
      if (!copied) throw new Error('Copy failed');
    }
    button.dataset.copyState = 'copied';
    button.setAttribute('aria-label', 'Command copied');
    button.title = 'Command copied';
    document.getElementById('copy-status').textContent = 'Command copied';
  } catch {
    delete button.dataset.copyState;
    button.setAttribute('aria-label', 'Copy unavailable. Select the command');
    button.title = 'Copy unavailable. Select the command';
    document.getElementById('copy-status').textContent = 'Copy unavailable. Select the command to copy it.';
  }
  copyResetTimer = setTimeout(() => {
    delete button.dataset.copyState;
    button.setAttribute('aria-label', 'Copy command');
    button.title = 'Copy command';
  }, 2000);
});
});
