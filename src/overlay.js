const canvas = document.getElementById('overlay');
const context = canvas.getContext('2d');

let startPoint = null;
let currentPoint = null;

function resizeCanvas() {
  canvas.width = window.innerWidth;
  canvas.height = window.innerHeight;
  draw();
}

function clearCanvas() {
  context.clearRect(0, 0, canvas.width, canvas.height);
}

function draw() {
  clearCanvas();

  if (!startPoint || !currentPoint) {
    return;
  }

  context.strokeStyle = 'rgba(255, 255, 255, 1)';
  context.lineWidth = 2;
  context.lineCap = 'round';
  context.beginPath();
  context.moveTo(startPoint.x, startPoint.y);
  context.lineTo(currentPoint.x, currentPoint.y);
  context.stroke();
}

window.addEventListener('resize', resizeCanvas);

window.overlayAPI.onStart((point) => {
  startPoint = point;
  currentPoint = point;
  draw();
});

window.overlayAPI.onUpdate((point) => {
  if (!startPoint) {
    startPoint = point;
  }

  currentPoint = point;
  draw();
});

window.overlayAPI.onClear(() => {
  startPoint = null;
  currentPoint = null;
  clearCanvas();
});

resizeCanvas();
