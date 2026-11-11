const formatTabs = [...document.querySelectorAll('.rule-tabs [role="tab"]')];
function selectFormat(selected) {
  for (const tab of formatTabs) {
    const active = tab === selected;
    tab.setAttribute('aria-selected', String(active));
    tab.tabIndex = active ? 0 : -1;
    document.getElementById(tab.getAttribute('aria-controls')).hidden = !active;
  }
}
for (const [index, tab] of formatTabs.entries()) {
  tab.addEventListener('click', () => selectFormat(tab));
  tab.addEventListener('keydown', event => {
    let next;
    if (event.key === 'ArrowRight') next = (index + 1) % formatTabs.length;
    if (event.key === 'ArrowLeft') next = (index + formatTabs.length - 1) % formatTabs.length;
    if (event.key === 'Home') next = 0;
    if (event.key === 'End') next = formatTabs.length - 1;
    if (next === undefined) return;
    event.preventDefault();
    selectFormat(formatTabs[next]);
    formatTabs[next].focus();
  });
}
if (formatTabs.length) {
  selectFormat(formatTabs[0]);
  document.querySelector('.rule-tabs').hidden = false;
}
