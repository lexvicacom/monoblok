const installer = document.querySelector('esp-web-install-button');
const status = document.getElementById('installer-status');
try {
  await import('./assets/esp32/vendor/esp-web-tools/install-button.js');
  await customElements.whenDefined('esp-web-install-button');
  installer.hidden = false;
  status.hidden = true;
} catch (error) {
  status.textContent = 'The installer could not load. Reload the page to try again.';
  console.error('Unable to load ESP32 installer', error);
}
