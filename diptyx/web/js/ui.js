// Page logic for the Diptyx web flasher: wires the buttons to the flows in flow.js and keeps the user informed.
import { parseManifest, explainError, STOCK_APP } from './core.js';
import { checkDevice, installApp, installFromUrl, restoreStock, backupApp } from './flow.js';

const $ = (id) => document.getElementById(id);
const supported = typeof navigator !== 'undefined' && 'serial' in navigator;
let driver = null;
let busy = false;
let manifest = { version: 'dev', firmware: null };
// Loaded early so the click handler that opens the USB picker still has the browser's user-activation window.
const driverModule = supported ? import('./esp-driver.js') : Promise.resolve({});
driverModule.catch((e) => console.error('Could not load the flasher driver', e));

const PHASES = {
  download: 'Download the firmware',
  check: 'Check your Diptyx',
  'verify-file': 'Check the firmware file',
  write: 'Write the firmware',
  'verify-write': 'Verify what was written',
  reset: 'Restart the Diptyx',
  read: 'Read your current firmware',
};

function log(line) {
  const el = $('log');
  el.textContent += String(line).replace(/\s+$/, '') + '\n';
  el.scrollTop = el.scrollHeight;
}

function setBusy(on) {
  busy = on;
  $('connect').disabled = on || !supported;
  $('install').disabled = on || !manifest.firmware;
  for (const id of ['backup', 'restore', 'own']) $(id).disabled = on;
}

function showProgress(title, phaseIds) {
  $('result').hidden = true;
  $('progress').hidden = false;
  $('progress-title').textContent = title;
  const ol = $('phases');
  ol.replaceChildren(...phaseIds.map((id) => { const li = document.createElement('li'); li.dataset.id = id; li.textContent = PHASES[id]; return li; }));
  setBar(0);
}

function setPhase(id) {
  let seen = false;
  for (const li of $('phases').children) {
    if (li.dataset.id === id) { li.className = 'active'; seen = true; setBar(0); }
    else li.className = seen ? '' : 'done';
  }
}

function setBar(f) {
  const pct = Math.max(0, Math.min(100, Math.round(f * 100)));
  $('bar').style.width = pct + '%';
  $('bar').parentElement.setAttribute('aria-valuenow', String(pct));
}

function finishPhases() { for (const li of $('phases').children) li.className = 'done'; setBar(1); }

function showResult(kind, title, lines = []) {
  $('progress').hidden = true;
  const box = $('result');
  box.className = 'card ' + kind;
  box.hidden = false;
  const h = document.createElement('h2'); h.textContent = title;
  box.replaceChildren(h, ...lines.filter(Boolean).map((t) => { const p = document.createElement('p'); p.textContent = t; return p; }));
  box.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
}

function fail(err) {
  const { title, hint } = explainError(err);
  log('ERROR: ' + (err && err.stack ? err.stack : err));
  showResult('fail', title, [hint, 'Nothing else on your device was changed outside the app slot. The technical log below has details.']);
}

async function closeDriver() {
  if (driver) { try { await driver.disconnect(); } catch { /* ignore */ } driver = null; }
  $('actions').hidden = true;
}

function setChecks(items) {
  const ul = $('checks');
  ul.hidden = false;
  ul.replaceChildren(...items.map(([cls, text]) => { const li = document.createElement('li'); li.className = cls; li.textContent = text; return li; }));
}

async function onConnect() {
  if (busy) return;
  setBusy(true);
  $('result').hidden = true;
  await closeDriver();
  setChecks([['', 'Waiting for you to choose the device...']]);
  try {
    // __driverFactory is a test seam: a page test can supply a fake device instead of the real serial driver.
    driver = globalThis.__driverFactory ? await globalThis.__driverFactory(log) : new (await driverModule).EspDriver({ log });
    await driver.connect();
    setChecks([['ok', 'Connected'], ['', 'Checking the device...']]);
    const info = await checkDevice(driver, { log });
    setChecks([
      ['ok', `Chip: ${info.chip}`],
      ['ok', `Flash: ${info.flash / 1048576} MB`],
      ['ok', 'Partition table: stock Diptyx layout'],
    ]);
    $('actions').hidden = false;
    $('actions').scrollIntoView({ behavior: 'smooth', block: 'nearest' });
  } catch (e) {
    const { title, hint } = explainError(e);
    setChecks([['fail', title], ['', hint]]);
    log('ERROR: ' + (e && e.stack ? e.stack : e));
    await closeDriver();
  } finally {
    setBusy(false);
  }
}

const hooks = () => ({ log, onStep: (id) => setPhase(id), onProgress: (f) => setBar(f) });

async function run(title, phaseIds, job, doneTitle, doneLines) {
  if (busy || !driver) return;
  setBusy(true);
  showProgress(title, phaseIds);
  try {
    await job();
    finishPhases();
    await closeDriver();
    showResult('ok', doneTitle, doneLines);
  } catch (e) {
    fail(e);
    await closeDriver();
  } finally {
    setBusy(false);
  }
}

const AFTER = [
  'The Diptyx restarts by itself and shows the CrossPoint home screen within about 20 seconds.',
];
const AFTER_OWN = ['The Diptyx restarts by itself within about 20 seconds.'];
const AFTER_STOCK = [
  'The Diptyx restarts by itself into the stock firmware within about 20 seconds. Your settings and books are untouched. If you switch it off completely later, tap the power button briefly to start it (wait 20 seconds and tap again if it does not start; holding it for 3 seconds makes the stock firmware shut down).',
];

function onInstall() {
  const fw = manifest.firmware;
  if (!fw) return;
  run('Installing CrossPoint ' + manifest.version, ['download', 'check', 'verify-file', 'write', 'verify-write', 'reset'],
    () => installFromUrl(driver, { url: new URL(fw.path, location.href).href, sha256: fw.sha256, size: fw.size }, hooks()),
    'CrossPoint is installed.', AFTER);
}

function onRestore() {
  if (!confirm(`Replace the app with the stock Diptyx firmware ${STOCK_APP.version}? Your settings and books are not touched.`)) return;
  run('Restoring the stock firmware', ['download', 'check', 'verify-file', 'write', 'verify-write', 'reset'],
    () => restoreStock(driver, hooks()), 'The stock firmware is back.', AFTER_STOCK);
}

async function onOwnFile(ev) {
  const file = ev.target.files && ev.target.files[0];
  ev.target.value = '';
  if (!file || busy || !driver) return;
  if (!confirm(`Install "${file.name}" (${file.size.toLocaleString()} bytes) into the app slot?`)) return;
  run('Installing ' + file.name, ['check', 'verify-file', 'write', 'verify-write', 'reset'],
    async () => installApp(driver, new Uint8Array(await file.arrayBuffer()), hooks()), 'Your firmware is installed.', AFTER_OWN);
}

async function onBackup() {
  if (busy || !driver) return;
  setBusy(true);
  showProgress('Saving a copy of your current firmware', ['check', 'read']);
  try {
    const data = await backupApp(driver, hooks());
    finishPhases();
    const d = new Date().toISOString().slice(0, 10);
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([data], { type: 'application/octet-stream' }));
    a.download = `diptyx-app-backup-${d}.bin`;
    document.body.append(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 60000);
    showResult('ok', 'Backup saved.', [`Saved as ${a.download} (${data.length.toLocaleString()} bytes). It is the app slot only, so you can restore it with "Install my own firmware file". Keep it private.`, 'You can now install CrossPoint below.']);
  } catch (e) { fail(e); await closeDriver(); } finally { setBusy(false); }
}

async function loadManifest() {
  try {
    const res = await fetch('manifest.json', { cache: 'no-store' });
    manifest = parseManifest(await res.json());
  } catch (e) { log('Could not read manifest.json: ' + e.message); manifest = { version: 'dev', firmware: null }; }
  $('v').textContent = manifest.version || 'dev';
  const fw = manifest.firmware;
  $('version-line').textContent = fw
    ? `CrossPoint for Diptyx ${manifest.version}${manifest.released ? ' (' + manifest.released + ')' : ''}, ${(fw.size / 1048576).toFixed(1)} MB.`
    : 'No firmware is bundled with this copy of the page (a development copy). Use "Install my own firmware file" under Other options.';
  $('install').disabled = !fw;
}

function init() {
  if (!window.isSecureContext) $('insecure').hidden = false;
  if (!supported) {
    $('unsupported').hidden = false;
    $('connect').disabled = true;
  }
  $('connect').addEventListener('click', onConnect);
  $('install').addEventListener('click', onInstall);
  $('restore').addEventListener('click', onRestore);
  $('backup').addEventListener('click', onBackup);
  $('own').addEventListener('change', onOwnFile);
  $('copylog').addEventListener('click', async () => {
    try { await navigator.clipboard.writeText($('log').textContent); $('copylog').textContent = 'Copied'; setTimeout(() => ($('copylog').textContent = 'Copy log'), 1500); } catch { /* clipboard blocked */ }
  });
  window.addEventListener('beforeunload', (e) => { if (busy) { e.preventDefault(); e.returnValue = ''; } });
  if (supported) {
    navigator.serial.addEventListener('disconnect', (ev) => {
      // Ignore other serial devices being unplugged.
      if (!driver || (ev.target && driver.port && ev.target !== driver.port)) return;
      if (busy) { log('The USB device was disconnected.'); }
      else { closeDriver(); setChecks([['fail', 'The Diptyx was unplugged.']]); }
    });
  }
  loadManifest();
}

init();
