/* Preview stills at beat boundaries — screenshots are taken from __render(t). */
const fs = require('fs');
const path = require('path');
const { chromium } = require('/home/lakshaya/node_modules/playwright');

const DIR = __dirname;
const OUT = path.join(DIR, 'previews');
const TIMES = [1, 5, 9, 13, 18, 23, 28];
const EXEC = '/home/lakshaya/.cache/ms-playwright/chromium-1255/chrome-linux64/chrome';

(async () => {
  fs.mkdirSync(OUT, { recursive: true });
  const browser = await chromium.launch({
    executablePath: EXEC,
    headless: true,
    args: ['--no-sandbox', '--disable-dev-shm-usage', '--disable-gpu', '--allow-file-access-from-files']
  });
  const context = await browser.newContext({ viewport: { width: 1920, height: 1080 }, deviceScaleFactor: 1 });
  const page = await context.newPage();
  await page.goto('file://' + path.join(DIR, 'index.html'), { waitUntil: 'load' });
  await page.waitForFunction(() => window.__filmReady === true);
  await page.evaluate(async () => {
    await Promise.all(Array.from(document.images).map(img => img.decode().catch(() => {})));
    await document.fonts.ready;
  });
  for (const t of TIMES) {
    await page.evaluate(v => window.__render(v), t);
    await page.screenshot({ path: path.join(OUT, `still_${String(t).padStart(2, '0')}.png`), animations: 'disabled' });
    console.log(`still t=${t}s`);
  }
  await browser.close();
})().catch(e => { console.error(e.stack || e.message); process.exit(1); });
