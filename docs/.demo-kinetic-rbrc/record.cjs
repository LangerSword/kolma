/* Record the deterministic film with Playwright at 1920x1080. */
const fs = require('fs');
const path = require('path');
const { chromium } = require('/home/lakshaya/node_modules/playwright');

const DIR = __dirname;
const RAW = path.join(DIR, 'raw');
const EXEC = '/home/lakshaya/.cache/ms-playwright/chromium-1255/chrome-linux64/chrome';

(async () => {
  fs.mkdirSync(RAW, { recursive: true });
  const browser = await chromium.launch({
    executablePath: EXEC,
    headless: true,
    args: ['--no-sandbox', '--disable-dev-shm-usage', '--disable-gpu', '--allow-file-access-from-files',
           '--autoplay-policy=no-user-gesture-required']
  });
  const context = await browser.newContext({
    viewport: { width: 1920, height: 1080 },
    deviceScaleFactor: 1,
    recordVideo: { dir: RAW, size: { width: 1920, height: 1080 } }
  });
  const page = await context.newPage();
  await page.goto('file://' + path.join(DIR, 'index.html'), { waitUntil: 'load' });
  await page.waitForFunction(() => window.__filmReady === true);
  await page.evaluate(async () => {
    await Promise.all(Array.from(document.images).map(img => img.decode().catch(() => {})));
    await document.fonts.ready;
  });
  await page.evaluate(() => window.__startFilm());
  await page.waitForTimeout(30300);
  const done = await page.evaluate(() => window.__filmDone === true);
  const video = page.video();
  await page.close();
  await context.close();
  await browser.close();
  const rawPath = await video.path();
  const info = { rawPath, done, recordedAt: new Date().toISOString() };
  fs.writeFileSync(path.join(DIR, 'record.json'), JSON.stringify(info, null, 2));
  console.log(JSON.stringify(info, null, 2));
})().catch(e => { console.error(e.stack || e.message); process.exit(1); });
