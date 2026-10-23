const { chromium } = require('playwright');
const path = require('path');
(async () => {
  const jobs = JSON.parse(process.argv[2]); const dir = process.argv[3];
  const channel = process.env.SZU_PREVIEW_BROWSER_CHANNEL;
  const browser = await chromium.launch(channel ? { channel } : {});
  for (const [name, w, h] of jobs) {
    const page = await browser.newPage({ viewport: { width: w, height: h || 900 }, deviceScaleFactor: 2 });
    await page.goto('file://' + path.join(dir, 'html', name + '.html'));
    await page.screenshot({ path: path.join(dir, name + '.png'), fullPage: h === 0 });
    await page.close();
  }
  await browser.close();
})().catch(e => { console.error(e); process.exit(1); });
