const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('overlayAPI', {
  onStart(handler) {
    ipcRenderer.on('overlay:start', (_event, point) => handler(point));
  },
  onUpdate(handler) {
    ipcRenderer.on('overlay:update', (_event, point) => handler(point));
  },
  onClear(handler) {
    ipcRenderer.on('overlay:clear', () => handler());
  },
  show(point) {
    return ipcRenderer.invoke('overlay:show', point);
  },
  update(point) {
    return ipcRenderer.invoke('overlay:update', point);
  },
  hide() {
    return ipcRenderer.invoke('overlay:hide');
  }
});
