// WS 断连诊断：加载面板页，抓取浏览器控制台与 WebSocket 生命周期事件
import { chromium } from 'playwright';

const URL = process.argv[2] || 'http://127.0.0.1:18080/';

const browser = await chromium.launch();
const page = await browser.newPage();

page.on('console', (msg) => {
    console.log(`[console.${msg.type()}] ${msg.text()}`);
});
page.on('websocket', (ws) => {
    console.log(`[ws open] ${ws.url()}`);
    ws.on('close', () => console.log(`[ws close] ${ws.url()}`));
    ws.on('socketerror', (err) => console.log(`[ws error] ${err}`));
    ws.on('framereceived', (f) => {
        const payload = typeof f.payload === 'string'
            ? f.payload.slice(0, 80)
            : `binary ${f.payload.length}B`;
        if (Math.random() < 0.05 || typeof f.payload === 'string')
            console.log(`[ws recv] ${payload}`);
    });
});
page.on('pageerror', (err) => console.log(`[pageerror] ${err.message}`));

await page.goto(URL, { waitUntil: 'load', timeout: 10000 });
await page.waitForTimeout(5000);

const badge = await page.textContent('#conn-badge');
const wsrtt = await page.textContent('#m-ws_rtt');
const frameInfo = await page.textContent('#frame-info');
const p50 = await page.textContent('#m-lat_p50');
console.log(`\n[ui] badge=${badge} ws_rtt=${wsrtt} frame=${frameInfo} e2e_p50=${p50}`);

await browser.close();
