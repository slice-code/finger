'use strict';

const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('electron', {
  api: {
    list: () => ipcRenderer.invoke('api:list'),
    count: () => ipcRenderer.invoke('api:count'),
    status: () => ipcRenderer.invoke('api:status'),
    enroll: (body) => ipcRenderer.invoke('api:enroll', body),
    autoscanOn: () => ipcRenderer.invoke('api:autoscan/on'),
    autoscanOff: () => ipcRenderer.invoke('api:autoscan/off'),
    delete: (body) => ipcRenderer.invoke('api:delete', body),
    empty: () => ipcRenderer.invoke('api:empty'),
  },
  onEvent: (cb) => {
    const h = (_e, obj) => cb(obj);
    ipcRenderer.on('bridge-event', h);
    return () => ipcRenderer.removeListener('bridge-event', h);
  },
});