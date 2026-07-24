'use strict';

const { app, BrowserWindow } = require('electron');
const path = require('path');

console.log('test-demos: starting');

// Re-use the IPC handlers and lifecycle setup from main.js.
require('./main.js');

console.log('test-demos: main.js loaded');

async function sleep(ms) {
  return new Promise(r => setTimeout(r, ms));
}

async function runTests() {
  console.log('test-demos: awaiting app ready');
  await app.whenReady();
  console.log('test-demos: app ready');

  // Create a hidden test window that loads the same demo page.
  const win = new BrowserWindow({
    width: 1280,
    height: 800,
    show: false,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false
    }
  });

  console.log('test-demos: loading demo page');
  await win.loadFile(path.join(__dirname, '..', 'index.html'));
  console.log('test-demos: demo page loaded');
  await sleep(1000);
  console.log('test-demos: running evaluations');

  const results = await win.webContents.executeJavaScript(`
    (async () => {
      const slidesWithCode = Array.from(document.querySelectorAll('.demo-panel'));
      const out = [];
      for (let i = 0; i < slidesWithCode.length; i++) {
        const panel = slidesWithCode[i];
        const btn = panel.querySelector('.run-btn');
        const term = panel.querySelector('.terminal');
        if (!btn) continue;

        // Clear the terminal, click Run, and wait for output.
        term.innerHTML = '';
        btn.click();
        await new Promise(r => setTimeout(r, 800));

        // If there is an in-flight async evaluation, wait a bit more.
        await new Promise(r => setTimeout(r, 400));

        out.push({
          panel: i,
          slide: panel.closest('.slide')?.dataset.slide,
          terminal: term.innerText
        });
      }
      return out;
    })()
  `);

  console.log(JSON.stringify(results, null, 2));

  win.close();
  app.quit();
}

app.whenReady().then(runTests).catch(err => {
  console.error(err);
  app.quit(1);
});
