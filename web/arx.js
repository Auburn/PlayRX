// AnalogRX shared helpers (app, OBS pages, overlays).
export const $ = (sel, root = document) => root.querySelector(sel);
export const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];

export function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c]));
}

export async function get(path) {
  const r = await fetch(path, {cache: 'no-store'});
  if (!r.ok) throw new Error(`${path}: ${r.status}`);
  return r.json();
}

export async function post(path, body = {}) {
  const r = await fetch(path, {method: 'POST', body: JSON.stringify(body)});
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw Object.assign(new Error(`${path}: ${r.status}`), {status: r.status, body: j});
  return j;
}

// 12.345 -> "12.35"; 0 -> "–"
export const secs = s => (s > 0 ? s.toFixed(2) : '–');
// 75.3 -> "1:15.3"
export function clock(s) {
  s = Math.max(0, s || 0);
  const m = Math.floor(s / 60), r = s - m * 60;
  return `${m}:${r.toFixed(1).padStart(4, '0')}`;
}


// A race clock that runs smoothly between updates from the engine (they come every few hundred ms): given the
// engine's elapsed time, it counts on by itself every screen refresh. `key` names the clock (the element may be
// redrawn); small differences from the next update are absorbed so it never jumps back. fmt(seconds) -> text.
const liveClocks = new Map();  // key -> {el, base, at, run, fmt}
let liveTick = 0;
export function liveClock(key, el, elapsed_s, running, fmt = clock) {
  if (!el) return;
  const now = performance.now(), prev = liveClocks.get(key);
  let base = Math.max(0, elapsed_s || 0);
  if (prev && prev.run && running) {
    const shown = prev.base + (now - prev.at) / 1000;
    if (Math.abs(shown - base) < 0.3) base = Math.max(base, shown);  // jitter from the network: keep counting forward
  }
  liveClocks.set(key, {el, base, at: now, run: !!running, fmt});
  el.textContent = fmt(base);
  if (!liveTick) {
    const tick = () => {
      const t = performance.now();
      for (const [k, c] of liveClocks) {
        if (!c.el.isConnected) { liveClocks.delete(k); continue; }
        if (!c.run) continue;
        const txt = c.fmt(c.base + (t - c.at) / 1000);
        if (c.el.textContent !== txt) c.el.textContent = txt;
      }
      liveTick = liveClocks.size ? requestAnimationFrame(tick) : 0;
    };
    liveTick = requestAnimationFrame(tick);
  }
}


// Live events from the engine (/ws/events): fn({type, ...}) for each; reconnects on its own. Types: hello, flow,
// pass, result, heat, league. Returns {close()}.
export function events(fn) {
  let ws = null, closed = false;
  function open() {
    if (closed) return;
    ws = new WebSocket(`ws://${location.host}/ws/events`);
    ws.onmessage = e => { try { fn(JSON.parse(e.data)); } catch { /* not JSON */ } };
    ws.onclose = () => { if (!closed) setTimeout(open, 1000); };
  }
  open();
  return {close() { closed = true; ws?.close(); }};
}

let toastTimer;
export function toast(msg) {
  let t = $('.toast');
  if (!t) {
    t = document.createElement('div');
    t.className = 'toast';
    document.body.append(t);
  }
  t.textContent = msg;
  t.classList.add('on');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.remove('on'), 1800);
}

// A yes / no question in the page (the app window's own confirm() can be switched off by the browser engine):
// resolves true on the OK button, false on Cancel / Esc / a click outside. o: {title, text, ok, cancel, danger}
export function ask(o) {
  return new Promise(resolve => {
    const bg = document.createElement('div');
    bg.className = 'ask-bg';
    bg.innerHTML = `<div class="ask" role="dialog" aria-modal="true"><h3>${esc(o.title || 'Are you sure?')}</h3>
      ${o.text ? `<p>${o.text}</p>` : ''}<div class="row" style="justify-content:flex-end">
      <button class="btn btn-ghost" data-a="no">${esc(o.cancel || 'Cancel')}</button>
      <button class="btn ${o.danger ? 'btn-danger' : 'btn-primary'}" data-a="yes">${esc(o.ok || 'OK')}</button></div></div>`;
    const done = v => { bg.remove(); document.removeEventListener('keydown', key, true); resolve(v); };
    const key = e => { if (e.key === 'Escape') { e.stopPropagation(); done(false); } };
    bg.addEventListener('click', e => { if (e.target === bg) done(false); const a = e.target.closest('[data-a]'); if (a) done(a.dataset.a === 'yes'); });
    document.addEventListener('keydown', key, true);
    document.body.append(bg);
    bg.querySelector('[data-a=yes]').focus();
  });
}

export async function copy(text) {
  try {
    await navigator.clipboard.writeText(text);
    toast('Copied ' + text);
  } catch {
    prompt('Copy this:', text);
  }
}

// Fit a canvas's backing store to its CSS size; returns the device pixel ratio.
export function fit(canvas) {
  const r = canvas.getBoundingClientRect(), d = devicePixelRatio || 1;
  const w = Math.max(1, Math.round(r.width * d)), h = Math.max(1, Math.round(r.height * d));
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
  }
  return d;
}

export const cssVar = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();

// Live video over one WebSocket for any number of pilots: each message is [slot byte][JPEG]. Draws each
// picture into the canvas `canvasFor(slot)` returns (CSS object-fit: contain sizes it). Reconnects on its own.
// Returns {close()}.
export function video(slots, canvasFor) {
  let ws = null, closed = false;
  const busy = new Set();  // slots with a decode in flight: newer pictures replace, never queue
  function open() {
    if (closed) return;
    ws = new WebSocket(`ws://${location.host}/ws/feed?slots=${slots.join(',')}`);
    ws.binaryType = 'arraybuffer';
    ws.onmessage = async e => {
      const slot = new Uint8Array(e.data, 0, 1)[0];
      if (busy.has(slot)) return;
      const c = canvasFor(slot);
      if (!c) return;
      busy.add(slot);
      try {
        const bmp = await createImageBitmap(new Blob([new Uint8Array(e.data, 1)], {type: 'image/jpeg'}));
        // Analog video is 4:3: the decoder's 720 x 480 (or 576) has non-square pixels. Draw it 4:3, at about the
        // size it is shown (480 lines for small tiles, up to 1080 for full screen / an OBS 1080p source), with
        // high-quality scaling, so the picture is the right shape and the browser's stretch doesn't soften it.
        const want = Math.min(1080, Math.max(bmp.height, Math.round((c.clientHeight || bmp.height) * (devicePixelRatio || 1) / 60) * 60));
        const h = Math.abs(want - c.height) > 0.1 * want || !c.height ? want : c.height, w = Math.round(h * 4 / 3);
        if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
        const g = c.getContext('2d');
        g.imageSmoothingEnabled = true;
        g.imageSmoothingQuality = 'high';
        g.drawImage(bmp, 0, 0, w, h);
        c.dataset.at = performance.now();  // when a picture last arrived (the Live page: "weak", not "no", signal)
        bmp.close();
      } catch { /* a torn frame */ }
      busy.delete(slot);
    };
    ws.onclose = () => { if (!closed) setTimeout(open, 1000); };
  }
  open();
  return {close() { closed = true; ws?.close(); }};
}

// Multi-select dropdown (design system .dd): a button that opens a list of checkboxes.
//   multiSelect(host, {options: [{value, label, hint}], selected: [...], min: 1, summary(sel) -> text,
//                      onChange(sel)})
// Keeps at least `min` options selected. Returns {set(selected)}.
export function multiSelect(host, o) {
  let sel = new Set(o.selected);
  host.classList.add('dd');
  host.innerHTML = `<button type="button" class="dd-btn"></button><div class="dd-menu" role="listbox">
    ${o.options.map(x => `<label class="dd-item"><input type="checkbox" class="check" value="${esc(x.value)}">
      <span class="dd-label">${esc(x.label)}</span>${x.hint ? `<span class="dd-hint">${esc(x.hint)}</span>` : ''}</label>`).join('')}
    <div class="dd-foot"><button type="button" class="btn btn-sm btn-ghost" data-all>Select all</button>
      <button type="button" class="btn btn-sm btn-ghost" data-none>Clear</button></div></div>`;
  const btn = $('.dd-btn', host), boxes = $$('input', host);
  const label = () => {
    const names = o.options.filter(x => sel.has(x.value)).map(x => x.label);
    return o.summary ? o.summary([...sel], names) : names.join(', ') || 'None';
  };
  const render = () => {
    boxes.forEach(b => {
      b.checked = sel.has(b.value);
      b.disabled = b.checked && sel.size <= (o.min ?? 0);  // the last one can't be cleared
    });
    btn.textContent = label();
  };
  const commit = next => {
    if (next.size < (o.min ?? 0)) return render();
    sel = next;
    render();
    o.onChange?.([...sel]);
  };
  btn.onclick = e => { e.stopPropagation(); host.classList.toggle('open'); };
  host.addEventListener('change', e => {
    if (!e.target.matches('input')) return;
    const next = new Set(sel);
    e.target.checked ? next.add(e.target.value) : next.delete(e.target.value);
    commit(next);
  });
  $('[data-all]', host).onclick = () => commit(new Set(o.options.map(x => x.value)));
  $('[data-none]', host).onclick = () => commit(new Set([...sel].slice(0, o.min ?? 0)));  // keeps the minimum
  const close = e => { if (!host.contains(e.target)) host.classList.remove('open'); };
  document.addEventListener('click', close);
  addEventListener('keydown', e => { if (e.key === 'Escape') host.classList.remove('open'); });
  render();
  return {set(v) { sel = new Set(v); render(); }};
}

// A repeating poll that survives errors and stops when `alive()` turns false.
export function every(ms, fn, alive = () => true) {
  let stopped = false;
  (async function loop() {
    if (stopped || !alive()) return;
    try { await fn(); } catch (e) { /* the app may be restarting */ }
    setTimeout(loop, ms);
  })();
  return () => { stopped = true; };
}

// ---- exports: CSV files and printable reports (no server round trip)
// rows: arrays of cells; the first row is the header. Excel-friendly: UTF-8 with a BOM, CRLF lines.
export function toCsv(rows) {
  const cell = v => {
    const s = v == null ? '' : String(v);
    return /[",\r\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
  };
  return '\ufeff' + rows.map(r => r.map(cell).join(',')).join('\r\n') + '\r\n';
}
export function download(name, text, type = 'text/csv;charset=utf-8') {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([text], {type}));
  a.download = name;
  document.body.append(a);
  a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
}
export const fileName = s => String(s).replace(/[\/:*?"<>|]+/g, '-').trim() || 'export';
// "1,2,3\n..." -> [["1","2","3"], ...] (quotes, commas and line breaks in quoted cells)
export function parseCsv(text) {
  const rows = [];
  let row = [], cur = '', q = false;
  text = text.replace(/^\ufeff/, '');
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (q) {
      if (c === '"' && text[i + 1] === '"') { cur += '"'; i++; } else if (c === '"') q = false; else cur += c;
    } else if (c === '"') q = true;
    else if (c === ',') { row.push(cur); cur = ''; }
    else if (c === '\n' || c === '\r') {
      if (c === '\r' && text[i + 1] === '\n') i++;
      row.push(cur); cur = '';
      if (row.some(x => x.trim())) rows.push(row);
      row = [];
    } else cur += c;
  }
  row.push(cur);
  if (row.some(x => x.trim())) rows.push(row);
  return rows;
}
// A printable report (black on white) in a hidden frame: the print dialog offers Save as PDF.
export function printReport(title, html) {
  const f = document.createElement('iframe');
  f.style.cssText = 'position:fixed;right:0;bottom:0;width:0;height:0;border:0';
  document.body.append(f);
  const d = f.contentDocument;
  d.open();
  d.write(`<!doctype html><html><head><meta charset="utf-8"><title>${esc(title)}</title><style>
    @page { margin: 14mm; }
    body { font: 10.5pt "Segoe UI", system-ui, sans-serif; color: #111; margin: 0; }
    h1 { font-size: 18pt; margin: 0 0 2pt; } h2 { font-size: 12.5pt; margin: 16pt 0 5pt; break-after: avoid; }
    .sub { color: #555; margin-bottom: 10pt; }
    table { width: 100%; border-collapse: collapse; margin-bottom: 6pt; break-inside: auto; }
    th, td { text-align: left; padding: 3pt 6pt; border-bottom: .5pt solid #ccc; }
    th { font-size: 8.5pt; color: #555; font-weight: 600; border-bottom: 1pt solid #888; }
    td.r, th.r { text-align: right; font-variant-numeric: tabular-nums; }
    tr { break-inside: avoid; }
    .foot { margin-top: 18pt; color: #888; font-size: 8pt; }
  </style></head><body>${html}<div class="foot">${esc(title)} · printed ${new Date().toLocaleString()}</div></body></html>`);
  d.close();
  setTimeout(() => { f.contentWindow.focus(); f.contentWindow.print(); setTimeout(() => f.remove(), 60000); }, 250);
}

