const path = require('node:path');
const fs = require('node:fs');
const { app, BrowserWindow, ipcMain, screen, systemPreferences } = require('electron');

let overlayWindow = null;
let straightDrag = null;
let appConfig = null;
let pendingDragStart = null;

function loadAppConfig() {
  const configPath = path.join(__dirname, 'config', 'default.json');
  const rawConfig = fs.readFileSync(configPath, 'utf8');
  const parsed = JSON.parse(rawConfig);
  return parsed[process.platform] || parsed.default;
}

function resolveNativeAddonPath() {
  if (app.isPackaged) {
    return path.join(process.resourcesPath, 'app.asar.unpacked', 'build', 'Release', 'straight_drag.node');
  }

  return path.join(__dirname, 'build', 'Release', 'straight_drag.node');
}

function toOverlayPoint(point) {
  if (!overlayWindow || overlayWindow.isDestroyed()) {
    return point;
  }

  const bounds = overlayWindow.getBounds();
  return {
    x: point.x - bounds.x,
    y: point.y - bounds.y
  };
}

function getOverlayBounds() {
  const displays = screen.getAllDisplays();
  const minX = Math.min(...displays.map((display) => display.bounds.x));
  const minY = Math.min(...displays.map((display) => display.bounds.y));
  const maxX = Math.max(...displays.map((display) => display.bounds.x + display.bounds.width));
  const maxY = Math.max(...displays.map((display) => display.bounds.y + display.bounds.height));

  return {
    x: minX,
    y: minY,
    width: maxX - minX,
    height: maxY - minY
  };
}

function createOverlayWindow() {
  const { x, y, width, height } = getOverlayBounds();

  overlayWindow = new BrowserWindow({
    x,
    y,
    width,
    height,
    show: false,
    transparent: true,
    frame: false,
    alwaysOnTop: true,
    fullscreen: false,
    resizable: false,
    focusable: false,
    skipTaskbar: true,
    hasShadow: false,
    fullscreenable: false,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      backgroundThrottling: false
    }
  });

  overlayWindow.setAlwaysOnTop(true, 'screen-saver');
  overlayWindow.setVisibleOnAllWorkspaces(true, { visibleOnFullScreen: true });
  overlayWindow.setIgnoreMouseEvents(true, { forward: true });
  const loadPromise = overlayWindow.loadFile(path.join(__dirname, 'src', 'overlay.html'));

  overlayWindow.on('closed', () => {
    overlayWindow = null;
  });

  return loadPromise;
}

function sendToOverlay(channel, payload) {
  if (!overlayWindow || overlayWindow.isDestroyed()) {
    return;
  }

  overlayWindow.webContents.send(channel, payload);
}

function showOverlay(point) {
  if (!overlayWindow || overlayWindow.isDestroyed()) {
    return;
  }

  overlayWindow.setBounds(getOverlayBounds());
  if (!overlayWindow.isVisible()) {
    overlayWindow.showInactive();
  }
  overlayWindow.moveTop();

  sendToOverlay('overlay:start', toOverlayPoint(point));
}

function updateOverlay(point) {
  if (!overlayWindow || overlayWindow.isDestroyed()) {
    return;
  }

  sendToOverlay('overlay:update', toOverlayPoint(point));
}

function hideOverlay() {
  if (!overlayWindow || overlayWindow.isDestroyed()) {
    return;
  }

  sendToOverlay('overlay:clear');
  overlayWindow.hide();
}

function registerOverlayIpc() {
  ipcMain.handle('overlay:show', async (_event, point) => {
    showOverlay(point);
  });

  ipcMain.handle('overlay:update', async (_event, point) => {
    updateOverlay(point);
  });

  ipcMain.handle('overlay:hide', async () => {
    hideOverlay();
  });
}

function loadNativeAddon() {
  const addonPath = resolveNativeAddonPath();
  straightDrag = require(addonPath);
  straightDrag.setConfig(appConfig);

  straightDrag.setCallbacks({
    hotkeyStart(point) {
      pendingDragStart = point;
      showOverlay(point);
    },
    hotkeyMove(point) {
      updateOverlay(point);
    },
    hotkeyEnd(point) {
      hideOverlay();
      const startPoint = pendingDragStart;
      pendingDragStart = null;

      if (!startPoint || !point) {
        return;
      }

      setTimeout(() => {
        straightDrag.performDrag(startPoint, point);
      }, 45);
    }
  });
}

app.whenReady().then(async () => {
  if (process.platform === 'darwin') {
    if (typeof app.setActivationPolicy === 'function') {
      app.setActivationPolicy('accessory');
    }
    if (app.dock && typeof app.dock.hide === 'function') {
      app.dock.hide();
    }
    systemPreferences.isTrustedAccessibilityClient(true);
  }

  appConfig = loadAppConfig();
  await createOverlayWindow();
  registerOverlayIpc();
  loadNativeAddon();
  const listeningStarted = straightDrag.startListening();
  if (!listeningStarted) {
    throw new Error('Global input hooks failed to start. On macOS, grant Accessibility and Input Monitoring permissions, then restart the app.');
  }

  app.on('activate', () => {
    if (!overlayWindow) {
      createOverlayWindow();
    }
  });
});

app.on('before-quit', () => {
  if (straightDrag && typeof straightDrag.stopListening === 'function') {
    straightDrag.stopListening();
  }
});

app.on('window-all-closed', (event) => {
  event.preventDefault();
});
