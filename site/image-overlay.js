const imageLink = document.querySelector('[data-image-overlay]');
const imageDialog = document.querySelector('#tinyblok-cloud-dialog');

if (imageLink && imageDialog && typeof imageDialog.showModal === 'function') {
  imageLink.addEventListener('click', (event) => {
    event.preventDefault();
    imageDialog.showModal();
  });

  imageDialog.querySelector('button').addEventListener('click', () => imageDialog.close());
  imageDialog.addEventListener('click', (event) => {
    if (event.target !== imageDialog) return;
    const bounds = imageDialog.getBoundingClientRect();
    if (event.clientX < bounds.left || event.clientX > bounds.right ||
        event.clientY < bounds.top || event.clientY > bounds.bottom) {
      imageDialog.close();
    }
  });
}
