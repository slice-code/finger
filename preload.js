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
    branches: () => ipcRenderer.invoke('api:branches'),
    employees: (kodeCabang) => ipcRenderer.invoke('api:employees', kodeCabang),
    getConfig: () => ipcRenderer.invoke('api:getConfig'),
    setConfig: (cfg) => ipcRenderer.invoke('api:setConfig', cfg),
  },
  onEvent: (cb) => {
    const h = (_e, obj) => cb(obj);
    ipcRenderer.on('bridge-event', h);
    return () => ipcRenderer.removeListener('bridge-event', h);
  },
});