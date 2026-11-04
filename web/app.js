'use strict';
/* MeshCore Hub web app - core: state, API, WebSocket, shell, conversations and chat. */

// ------------------------------------------------------------------ utils
const $ = (s, el = document) => el.querySelector(s);
function h(tag, props, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (v == null || v === false) continue;
    if (k === 'class') el.className = v;
    else if (k === 'html') el.innerHTML = v;
    else if (k === 'text') el.textContent = v;
    else if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
    else if (k === 'style' && typeof v === 'object') Object.assign(el.style, v);
    else if (k in el && typeof v !== 'string') el[k] = v;
    else el.setAttribute(k, v === true ? '' : v);
  }
  for (const k of kids.flat(9)) {
    if (k == null || k === false) continue;
    el.append(k instanceof Node ? k : document.createTextNode(String(k)));
  }
  return el;
}
const I = (name, cls) => h('span', { html: icon(name, cls), style: { display: 'contents' } });
const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const utf8 = new TextEncoder();
const bytes = s => utf8.encode(s).length;
const hexToBytes = hx => new Uint8Array((hx.match(/../g) || []).map(b => parseInt(b, 16)));
const bytesToHex = b => Array.from(b, x => x.toString(16).padStart(2, '0')).join('');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const store = {
  get(k, d) { try { const v = localStorage.getItem('mch.' + k); return v == null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem('mch.' + k, JSON.stringify(v)); } catch { /* private mode */ } },
};

// ------------------------------------------------------------------ i18n
const LANGS = { fr: 'Français', en: 'English' };
let DICT = {}, LANG = 'fr';
async function loadLang(l) {
  if (!LANGS[l]) l = 'fr';
  // translations are embedded in index.html by tools/build_web.py (one request for the whole app)
  if (window.I18N?.[l]) DICT = window.I18N[l];
  else try { DICT = await (await fetch(`i18n/${l}.json`)).json(); } catch { DICT = {}; }
  LANG = l;
  document.documentElement.lang = l;
}
function t(k, v) {
  let s = DICT[k] ?? k;
  if (v) for (const [a, b] of Object.entries(v)) s = s.split(`{${a}}`).join(b);
  return s;
}

// ------------------------------------------------------------------ formatting
const fmtTime = ts => new Date(ts * 1000).toLocaleTimeString(LANG, { hour: '2-digit', minute: '2-digit' });
function fmtListTime(ts) {
  if (!ts) return '';
  const d = new Date(ts * 1000), now = new Date();
  if (d.toDateString() === now.toDateString()) return fmtTime(ts);
  if (now - d < 6 * 864e5) return d.toLocaleDateString(LANG, { weekday: 'short' });
  return d.toLocaleDateString(LANG, { day: 'numeric', month: 'short' });
}
function fmtDay(ts) {
  const d = new Date(ts * 1000), now = new Date();
  const y = new Date(now); y.setDate(now.getDate() - 1);
  if (d.toDateString() === now.toDateString()) return t('today');
  if (d.toDateString() === y.toDateString()) return t('yesterday');
  return d.toLocaleDateString(LANG, { weekday: 'long', day: 'numeric', month: 'long', year: d.getFullYear() !== now.getFullYear() ? 'numeric' : undefined });
}
function ago(ts) {
  if (!ts) return t('never');
  const s = Math.max(0, Date.now() / 1000 - ts);
  const rtf = new Intl.RelativeTimeFormat(LANG, { numeric: 'auto' });
  if (s < 60) return t('just_now');
  if (s < 3600) return rtf.format(-Math.round(s / 60), 'minute');
  if (s < 86400) return rtf.format(-Math.round(s / 3600), 'hour');
  return rtf.format(-Math.round(s / 86400), 'day');
}
function fmtDuration(sec) {
  const d = Math.floor(sec / 86400), hh = Math.floor(sec / 3600) % 24, m = Math.floor(sec / 60) % 60;
  return d ? `${d} j ${hh} h` : hh ? `${hh} h ${m} min` : `${m} min`;
}
function distKm(a, b) {
  if (!a || !b || (!a.lat && !a.lon) || (!b.lat && !b.lon)) return null;
  const R = 6371, r = x => x * Math.PI / 180;
  const dLat = r(b.lat - a.lat), dLon = r(b.lon - a.lon);
  const x = Math.sin(dLat / 2) ** 2 + Math.cos(r(a.lat)) * Math.cos(r(b.lat)) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.sqrt(x));
}
const fmtKm = d => d == null ? '' : d < 1 ? `${Math.round(d * 1000)} m` : `${d.toFixed(d < 10 ? 1 : 0)} km`;
function snrClass(s) { return s >= 5 ? 'snr-good' : s >= -5 ? 'snr-mid' : 'snr-bad'; }

// avatar color derived from the public key (or name)
function hue(str) { let x = 0; for (const c of String(str)) x = (x * 31 + c.codePointAt(0)) >>> 0; return x % 360; }
function initials(name) {
  const w = String(name || '?').replace(/[#@]/g, '').trim().split(/\s+/).filter(Boolean);
  const pick = s => Array.from(s)[0] || '';
  return ((w[0] ? pick(w[0]) : '?') + (w[1] ? pick(w[1]) : '')).toUpperCase();
}
const KIND_ICON = { 0: 'user', 1: 'hash', 2: 'antenna', 3: 'home', 4: 'thermo' };
const TYPE_KIND = { 1: 0, 2: 2, 3: 3, 4: 4 };
function avatar(name, seed, kind, cls) {
  const el = h('div', { class: 'avatar' + (cls ? ' ' + cls : ''), style: { background: `hsl(${hue(seed || name)} 52% 47%)` }, 'aria-hidden': 'true' });
  if (kind === 1) el.innerHTML = icon('hash');
  else el.textContent = initials(name);
  if (kind && kind !== 1) el.append(h('span', { class: 'kind', html: icon(KIND_ICON[kind]) }));
  return el;
}

// ------------------------------------------------------------------ state
const S = {
  status: {}, contacts: [], channels: [], maxChannels: 8, convs: [], msgs: {}, maxLen: {}, more: {},
  view: 'chats', sel: null, logins: {}, rstatus: {}, telemetry: {}, drafts: {}, logs: [],
  showArchived: false, contactFilter: 'all', search: '', ws: null, wsOk: false, authed: false,
};

// ------------------------------------------------------------------ API
const waiters = new Map();   // rid -> resolve
const evWaiters = [];        // {pred, resolve}
// The Hub may answer 503 "busy" (RAM nearly full) or drop a request: retry a few times.
async function apiGet(path, tries = 4) {
  for (let i = 0; ; i++) {
    try {
      const r = await fetch(path, { credentials: 'same-origin', cache: 'no-store' });
      if (r.status === 401) { showLogin(); throw new Error('auth'); }
      if (r.status === 503) throw new Error('busy');
      return await r.json();  // a cut-off stream (e.g. long contact list) fails here and is retried
    } catch (e) {
      if (e.message === 'auth' || i + 1 >= tries) throw e;
      await sleep(1500);
    }
  }
}
async function apiDo(op, args = {}, quiet) {
  let r;
  try {
    r = await fetch('/api/do', { method: 'POST', credentials: 'same-origin', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ op, ...args }) });
  } catch (e) { if (!quiet) toast(t('err.network'), 'err'); throw e; }
  if (r.status === 401) { showLogin(); throw new Error('auth'); }
  const j = await r.json().catch(() => ({ ok: false, err: 'bad_json' }));
  if (!j.ok && !quiet) toast(t('err.' + j.err) !== 'err.' + j.err ? t('err.' + j.err) : t('err.generic', { e: j.err }), 'err');
  return j;
}
// waits for the {t:"result", rid} event of a mesh command
function waitRid(rid, ms = 15000) {
  return new Promise(res => {
    const to = setTimeout(() => { waiters.delete(rid); res({ ok: false, err: 'timeout' }); }, ms);
    waiters.set(rid, d => { clearTimeout(to); res(d); });
  });
}
function waitEvent(pred, ms = 20000) {
  return new Promise(res => {
    const w = { pred, res };
    evWaiters.push(w);
    setTimeout(() => { const i = evWaiters.indexOf(w); if (i >= 0) { evWaiters.splice(i, 1); res(null); } }, ms);
  });
}
async function meshCmd(op, args, okMsg) {
  const j = await apiDo(op, args);
  if (!j.ok) return null;
  if (!j.rid) { if (okMsg) toast(okMsg, 'ok'); return j; }
  const r = await waitRid(j.rid);
  if (r.ok) { if (okMsg) toast(okMsg, 'ok'); } else toast(t('err.mesh.' + r.err) !== 'err.mesh.' + r.err ? t('err.mesh.' + r.err) : t('err.generic', { e: r.err }), 'err');
  return r.ok ? r : null;
}

// ------------------------------------------------------------------ toasts, dialogs, menus
function toast(msg, kind) {
  let box = $('.toasts');
  if (!box) document.body.append(box = h('div', { class: 'toasts', role: 'status', 'aria-live': 'polite' }));
  const el = h('div', { class: 'toast' + (kind ? ' ' + kind : '') }, msg);
  box.append(el);
  setTimeout(() => el.remove(), kind === 'err' ? 5000 : 3000);
}
// In-app bubble for an incoming message (browser notifications need HTTPS, which the Hub lacks).
function msgBubble(title, body, conv, kind) {
  let box = $('.toasts');
  if (!box) document.body.append(box = h('div', { class: 'toasts', role: 'status', 'aria-live': 'polite' }));
  const el = h('button', { class: 'toast bubble', onclick: () => { el.remove(); go(`#/chats/${conv}`); } },
    avatar(title, contactByConv(conv)?.key || conv, kind, 'sm'),
    h('span', { class: 'bubble-txt' }, h('b', {}, title), h('span', {}, body.length > 90 ? body.slice(0, 90) + '…' : body)));
  box.append(el);
  while (box.querySelectorAll('.bubble').length > 3) box.querySelector('.bubble').remove();
  setTimeout(() => el.remove(), 7000);
}
function dialog(title, body, actions) {
  return new Promise(res => {
    const close = v => { ov.remove(); document.removeEventListener('keydown', key); res(v); };
    const key = e => { if (e.key === 'Escape') close(null); };
    const ov = h('div', { class: 'overlay', onclick: e => { if (e.target === ov) close(null); } },
      h('div', { class: 'dialog', role: 'dialog', 'aria-modal': 'true', 'aria-label': title },
        h('h2', {}, title), body,
        h('div', { class: 'btns', style: { justifyContent: 'flex-end' } },
          (actions || [{ label: t('ok'), value: true, pri: true }]).map(a =>
            h('button', { class: 'btn' + (a.pri ? ' pri' : '') + (a.danger ? ' danger' : ''), onclick: () => close(typeof a.value === 'function' ? a.value() : a.value) }, a.label)))));
    document.body.append(ov);
    document.addEventListener('keydown', key);
    setTimeout(() => (ov.querySelector('input,textarea,select') || ov.querySelector('.btn.pri'))?.focus(), 30);
  });
}
const confirmDlg = (title, text, label, danger) =>
  dialog(title, h('p', {}, text), [{ label: t('cancel'), value: false }, { label: label || t('ok'), value: true, pri: true, danger }]);
function menu(anchor, items) {
  document.querySelectorAll('.menu').forEach(m => m.remove());
  const m = h('div', { class: 'menu', role: 'menu' }, items.filter(Boolean).map(it =>
    h('button', { role: 'menuitem', class: it.danger ? 'danger' : '', onclick: () => { m.remove(); it.run(); } }, I(it.icon), it.label)));
  document.body.append(m);
  const r = anchor.getBoundingClientRect();
  const w = m.offsetWidth, hgt = m.offsetHeight;
  m.style.left = Math.max(8, Math.min(innerWidth - w - 8, r.right - w)) + 'px';
  m.style.top = (r.bottom + hgt + 8 > innerHeight ? r.top - hgt - 4 : r.bottom + 4) + 'px';
  setTimeout(() => document.addEventListener('click', function off(e) { if (!m.contains(e.target)) { m.remove(); document.removeEventListener('click', off); } }), 0);
  m.querySelector('button')?.focus();
}
function copy(text) {
  (navigator.clipboard?.writeText(text) || Promise.reject()).then(() => toast(t('copied'), 'ok'), () => {
    const ta = h('textarea', { value: text, style: { position: 'fixed', opacity: 0 } });
    document.body.append(ta); ta.select(); document.execCommand('copy'); ta.remove(); toast(t('copied'), 'ok');
  });
}
function qrSvg(text) {
  const q = qrcode(0, 'M');
  q.addData(unescape(encodeURIComponent(text)));  // UTF-8 bytes
  q.make();
  return h('div', { class: 'qr', html: q.createSvgTag({ cellSize: 4, margin: 0, scalable: true }) });
}

// ------------------------------------------------------------------ theme
function applyTheme() {
  const st = S.status;
  const theme = store.get('theme', st.theme || 'auto');
  if (theme === 'auto') document.documentElement.removeAttribute('data-theme');
  else document.documentElement.setAttribute('data-theme', theme);
  const acc = st.accent || '#2f6fed';
  document.documentElement.style.setProperty('--accent', acc);
  $('meta[name=theme-color]')?.setAttribute('content', acc);
}

// ------------------------------------------------------------------ WebSocket
let wsRetry = 1000;
function connectWs() {
  if (S.ws) try { S.ws.close(); } catch { /* ignore */ }
  const ws = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`);
  S.ws = ws;
  ws.onopen = () => { S.wsOk = true; wsRetry = 1000; renderStatus(); };
  ws.onclose = () => {
    S.wsOk = false; renderStatus();
    setTimeout(() => { if (S.ws === ws) connectWs(); }, wsRetry);
    wsRetry = Math.min(wsRetry * 2, 15000);
  };
  ws.onmessage = e => { try { onEvent(JSON.parse(e.data)); } catch (err) { console.error(err); } };
}
// one ws event -> state update + minimal re-render
function onEvent(d) {
  for (const w of [...evWaiters]) if (w.pred(d)) { evWaiters.splice(evWaiters.indexOf(w), 1); w.res(d); }
  switch (d.t) {
    case 'hello': case 'status':
      S.status = { ...S.status, ...d.status }; renderStatus(); renderNav(); if (S.view === 'settings' || S.view === 'node') refreshSolo(); if (S.view === 'settings') V.settings.live?.(); break;
    case 'link': {
      const wasReady = S.status.link?.state === 'ready';
      S.status.link = { ...(S.status.link || {}), state: d.state, phase: d.phase, error: d.error };
      renderStatus();
      if (d.state === 'ready' && !wasReady) { loadMeshData(); refreshIfPlaceholder(); }
      if (typeof wizOnLink === 'function') wizOnLink(d);
      break;
    }
    case 'self': S.status.node = d.node; renderStatus(); refreshIfPlaceholder(); break;
    case 'contacts': loadContacts(); break;
    case 'contact': {
      const i = S.contacts.findIndex(c => c.key === d.c.key);
      if (i >= 0) S.contacts[i] = d.c; else S.contacts.push(d.c);
      if (S.view === 'contacts') renderList();
      if (d.isNew) toast(t('new_contact', { n: d.c.name }));
      break;
    }
    case 'contact_removed': S.contacts = S.contacts.filter(c => c.key !== d.key); if (S.view === 'contacts') renderMain(); break;
    case 'channels': loadChannels(); break;
    case 'conv_removed':
      S.convs = S.convs.filter(c => c.id !== d.conv); delete S.msgs[d.conv]; renderNav();
      if (S.view === 'chats') { if (S.sel === d.conv) go('#/chats'); else renderList(); }
      break;
    case 'msg': onMsg(d); break;
    case 'st': onStatusUpdate(d); break;
    case 'battery': S.status.battery = { mv: d.mv, pct: d.pct, usedKb: d.usedKb, totalKb: d.totalKb }; renderStatus(); break;
    case 'login':
      S.logins[d.conv] = { ok: d.ok, admin: d.admin, at: Date.now() };
      toast(d.ok ? t('login_ok', { n: convName(d.conv) }) : t('login_fail', { n: convName(d.conv) }), d.ok ? 'ok' : 'err');
      if (S.sel === d.conv || S.view === 'contacts') renderDetail();
      break;
    case 'rstatus': S.rstatus[d.conv] = { ...d.s, at: Date.now() }; if (S.sel === d.conv || S.view === 'contacts') renderDetail(); break;
    case 'telemetry': S.telemetry[d.conv] = { lpp: d.lpp, at: Date.now() }; if (S.sel === d.conv || S.view === 'contacts') renderDetail(); break;
    case 'result': { const w = waiters.get(d.rid); if (w) { waiters.delete(d.rid); w(d); } break; }
    case 'contacts_full': toast(t('contacts_full'), 'err'); break;
    case 'log':
      S.logs.push(d.line); if (S.logs.length > 300) S.logs.shift();
      { const box = $('#logbox'); if (box) { box.append(h('div', { class: d.l }, d.line)); box.scrollTop = box.scrollHeight; } }
      break;
    case 'tg_claimed':  // a conversation was just linked: show it in the settings form
      if (S.view === 'settings' && !$('.wiz')) V.settings.claimed(d);
      if (typeof wizOnEvent === 'function') wizOnEvent(d);
      break;
    default:
      if (typeof wizOnEvent === 'function') wizOnEvent(d);
      if (typeof pageOnEvent === 'function') pageOnEvent(d);
  }
}

// ------------------------------------------------------------------ data loading
async function loadContacts() {
  try { S.contacts = await apiGet('/api/contacts'); } catch (e) { if (e.message !== 'auth') toast(t('err.contacts_load'), 'err'); return; }
  if (['contacts', 'map', 'chats', 'tools'].includes(S.view)) renderMain();
}
async function loadChannels() {
  try { const j = await apiGet('/api/channels'); S.channels = j.channels; S.maxChannels = j.max || 8; } catch { return; }
  if (['channels', 'chats'].includes(S.view)) renderMain();
}
async function loadConvs() {
  try { S.convs = await apiGet('/api/conversations'); } catch { return; }
  renderNav();
  if (S.view === 'chats') renderList();
}
// One request after another: every parallel connection costs RAM on the ESP32.
async function loadMeshData() { await loadConvs(); await loadChannels(); await loadContacts(); }

// ------------------------------------------------------------------ conversations helpers
function convKind(id) {
  if (id.startsWith('ch_')) return 1;
  const c = contactByConv(id);
  if (c) return TYPE_KIND[c.type] ?? 0;
  return S.convs.find(x => x.id === id)?.kind ?? 0;
}
const contactByConv = id => S.contacts.find(c => c.conv === id);
const contactByKey = k => S.contacts.find(c => c.key === k);
function convName(id) {
  if (id.startsWith('ch_')) return S.channels.find(c => c.conv === id)?.name || S.convs.find(c => c.id === id)?.name || id;
  return contactByConv(id)?.name || S.convs.find(c => c.id === id)?.name || id.slice(2);
}
function convList() {
  const map = new Map(S.convs.map(c => [c.id, { ...c }]));
  for (const ch of S.channels) if (!map.has(ch.conv)) map.set(ch.conv, { id: ch.conv, name: ch.name, kind: 1, lastTs: 0, unread: 0, preview: '' });
  for (const c of map.values()) c.name = convName(c.id) || c.name;
  return [...map.values()];
}
const unreadTotal = () => S.convs.reduce((n, c) => n + (c.muted ? 0 : c.unread || 0), 0);

function onMsg(d) {
  let c = S.convs.find(x => x.id === d.conv);
  if (!c) S.convs.push(c = { id: d.conv, name: d.name, kind: d.kind, unread: 0 });
  const m = d.m;
  c.lastTs = m.t; c.lastId = m.i; c.preview = m.x; c.lastAuthor = m.a; c.lastOut = !!m.d;
  const open = S.view === 'chats' && S.sel === d.conv;
  const visible = open && !document.hidden;
  if (!m.d && m.y !== 1) {
    if (!visible) c.unread = (c.unread || 0) + 1; else apiDo('read', { conv: d.conv }, true);
    if (c.archived) c.archived = false;
    if ((document.hidden || !open) && !c.muted) {
      const title = d.name || convName(d.conv), body = (m.a && d.kind !== 0 ? m.a + ' : ' : '') + m.x;
      if (!open) msgBubble(title, body, d.conv, d.kind);
      notify(title, body, d.conv);
    }
  }
  const list = S.msgs[d.conv];
  if (list && !list.some(x => x.i === m.i)) {
    list.push(m);
    if (open) appendBubble(m, list);
  }
  if (open && m.y === 1) renderDetail();
  if (S.view === 'chats') renderList();
  renderNav();
}
function onStatusUpdate(d) {
  const m = S.msgs[d.conv]?.find(x => x.i === d.id);
  if (m) { m.k = d.k; if (d.r) m.r = d.r; }
  const el = document.querySelector(`.msg[data-id="${d.id}"]`);
  if (el && m) el.replaceWith(bubble(m, d.conv, S.msgs[d.conv]));
}

// browser notifications (only available in a secure context: localhost / HTTPS)
function notify(title, body, conv) {
  if (!('Notification' in window) || Notification.permission !== 'granted' || !store.get('notif', true)) return;
  try {
    const n = new Notification(title, { body, tag: conv, icon: 'icon.svg' });
    n.onclick = () => { window.focus(); go(`#/chats/${conv}`); n.close(); };
  } catch { /* ignore */ }
}

// ------------------------------------------------------------------ shell
const NAV = [
  { id: 'chats', icon: 'chat', label: 'nav.chats' },
  { id: 'contacts', icon: 'users', label: 'nav.contacts' },
  { id: 'channels', icon: 'hash', label: 'nav.channels' },
  { id: 'map', icon: 'map', label: 'nav.map' },
  { id: 'tools', icon: 'tool', label: 'nav.tools' },
  { id: 'node', icon: 'radio', label: 'nav.node' },
  { id: 'settings', icon: 'settings', label: 'nav.settings' },
];
const MOBILE_TABS = ['chats', 'contacts', 'map', 'more'];

function shell() {
  const app = $('#app');
  app.innerHTML = '';
  app.append(
    h('div', { class: 'statusbar', id: 'statusbar', role: 'status' }),
    h('div', { class: 'main', id: 'main' }, h('nav', { class: 'nav', id: 'nav', 'aria-label': t('nav.label') }), h('div', { id: 'view', style: { display: 'contents' } })),
    h('nav', { class: 'tabbar', id: 'tabbar', 'aria-label': t('nav.label') }));
  renderStatus(); renderNav(); renderMain();
}

function linkDot() {
  const l = S.status.link || {};
  if (!S.wsOk) return ['err', t('st.offline')];
  if (l.state === 'ready') return ['ok', S.status.node?.name || t('st.ready')];
  if (l.state === 'connecting' || l.state === 'handshake') return ['warn pulse', t('phase.' + (l.phase || 'connecting'))];
  if (l.state === 'released') return ['', t('st.released')];
  if (l.state === 'disabled' || !l.type) return ['', t('st.no_module')];
  return ['err', l.error ? t('linkerr.' + l.error) : t('st.waiting')];
}
function renderStatus() {
  const bar = $('#statusbar');
  if (!bar) return;
  const st = S.status, [dc, label] = linkDot(), b = st.battery || {}, w = st.wifi || {}, tg = st.telegram || {};
  bar.innerHTML = '';
  bar.append(...[
    h('span', { class: 'pill', title: t('st.module') }, h('span', { class: 'dot ' + dc }), h('b', {}, label)),
    b.mv ? h('span', { class: 'pill', title: t('st.battery') }, I('battery'), `${b.pct}% · ${(b.mv / 1000).toFixed(2)} V`) : null,
    h('span', { class: 'pill hide-m', title: 'Wi-Fi' }, I('wifi'), w.connected ? `${w.ssid} (${w.rssi} dBm)` : (w.apSsid ? t('st.ap_mode', { s: w.apSsid }) : t('st.no_wifi'))),
    // 4G modem: only once it is enabled (most Hubs have none)
    st.cell?.state && !['off', 'unsupported'].includes(st.cell.state) ? h('span', { class: 'pill hide-m', title: t('cell_title'), style: { cursor: 'pointer' }, onclick: () => go('#/settings') },
      h('span', { class: 'dot ' + (st.cell.online ? 'ok' : st.cell.state === 'error' ? 'err' : st.cell.state === 'waiting_wifi' ? '' : 'warn') }), I('signal'),
      st.cell.online ? `4G${st.cell.operator ? ' · ' + st.cell.operator : ''}` : st.cell.state === 'error' ? t('st.cell_err') : t('st.cell') + ' · ' + t('cell_st.' + st.cell.state).replace(/[.…]+$/, '')) : null,
    // always shown (a click opens the settings): "no Telegram" must be visible too, not just absent
    h('span', { class: 'pill hide-m', title: 'Telegram', style: { cursor: 'pointer' }, onclick: () => go('#/settings') },
      h('span', { class: 'dot ' + (!tg.enabled ? '' : tg.online && tg.chats !== 0 ? 'ok' : 'err') }), I('telegram'),
      !tg.enabled ? t('st.tg_disabled') : tg.online ? (tg.user ? '@' + tg.user : 'Telegram') : t('st.tg_off'), tg.queued ? ` (${tg.queued})` : ''),
    h('span', { class: 'sp' }),
    st.lastSync ? h('span', { class: 'pill hide-m', title: t('st.last_sync') }, I('refresh'), fmtTime(st.lastSync)) : null,
  ].filter(Boolean));  // Element.append(null) would print "null"
}
function renderNav() {
  const nav = $('#nav'), tab = $('#tabbar');
  if (!nav) return;
  const un = unreadTotal();
  document.title = (un ? `(${un}) ` : '') + 'MeshCore Hub';
  nav.innerHTML = '';
  nav.append(h('div', { html: LOGO, style: { display: 'contents' } }));
  for (const n of NAV) {
    nav.append(h('button', { class: S.view === n.id ? 'on' : '', 'aria-current': S.view === n.id ? 'page' : null, onclick: () => go('#/' + n.id) },
      I(n.icon), t(n.label), n.id === 'chats' && un ? h('span', { class: 'badge' }, un > 99 ? '99+' : un) : null));
    if (n.id === 'channels') nav.append(h('div', { class: 'sp' }));
  }
  tab.innerHTML = '';
  for (const id of MOBILE_TABS) {
    const n = NAV.find(x => x.id === id) || { id: 'more', icon: 'more', label: 'nav.more' };
    const on = id === 'more' ? !['chats', 'contacts', 'map'].includes(S.view) : S.view === id;
    tab.append(h('button', { class: on ? 'on' : '', onclick: e => id === 'more' ? menu(e.currentTarget, NAV.filter(x => !MOBILE_TABS.includes(x.id)).map(x => ({ icon: x.icon, label: t(x.label), run: () => go('#/' + x.id) }))) : go('#/' + id) },
      I(n.icon), t(n.label), id === 'chats' && un ? h('span', { class: 'badge' }, un) : null));
  }
}

// views registry: list() / detail() for two-column views, solo() for full pages
const V = {};
function renderMain() {
  const view = $('#view');
  if (!view) return;
  const v = V[S.view] || V.chats;
  view.innerHTML = '';
  $('#main').classList.toggle('detail', !!S.sel);
  if (v.solo) {
    view.append(h('div', { class: 'col-main solo', id: 'solo' }, v.solo()));
    if (v.mounted) v.mounted();
  } else {
    view.append(h('section', { class: 'col-list', id: 'list', 'aria-label': t('nav.' + S.view) }), h('section', { class: 'col-main', id: 'detail' }));
    renderList(); renderDetail();
  }
}
function renderList() {
  const el = $('#list'); if (!el || !V[S.view]?.list) return;
  const st = el.querySelector('.scroll')?.scrollTop;
  // The search field being typed in is moved into the new list instead of being rebuilt: keys
  // typed while a long list (hundreds of contacts) is redrawn would land in a removed field.
  const typing = el.querySelector('.search input');
  const keep = typing && document.activeElement === typing ? typing : null;
  el.innerHTML = ''; el.append(...[V[S.view].list()].flat());
  if (keep) { el.querySelector('.search input')?.replaceWith(keep); keep.focus(); }
  const sc = el.querySelector('.scroll'); if (sc && st && !keep) sc.scrollTop = st;
}
// filter once typing pauses, not on every key
let searchTimer = 0;
function searchSoon() { clearTimeout(searchTimer); searchTimer = setTimeout(renderList, 150); }
function renderDetail() { const el = $('#detail'); if (!el || !V[S.view]?.detail) return; el.innerHTML = ''; el.append(...[V[S.view].detail()].flat()); V[S.view].detailMounted?.(); }
// Pages that keep form input (noAutoRefresh) are still redrawn while they only show the
// "connect the module" placeholder, so they fill in as soon as the module is ready.
function refreshIfPlaceholder() {
  const el = $('#solo');
  if (!el || !V[S.view]?.solo || !el.querySelector('.notice') || el.querySelector('input,select')) return;
  el.innerHTML = '';
  el.append(V[S.view].solo());
  V[S.view].mounted?.();
}
function refreshSolo() { const el = $('#solo'); if (!el || !V[S.view]?.solo || V[S.view].noAutoRefresh) return; const st = el.scrollTop; el.innerHTML = ''; el.append(V[S.view].solo()); el.scrollTop = st; V[S.view].mounted?.(); }
function go(hash) { if (location.hash === hash) route(); else location.hash = hash; }
function route() {
  const [, view, ...rest] = (location.hash || '#/chats').split('/');
  const prev = S.view;
  S.view = V[view] ? view : 'chats';
  S.sel = rest.length ? decodeURIComponent(rest.join('/')) : null;
  if (prev === 'map' && S.view !== 'map') V.map.unmount?.();
  renderNav(); renderMain();
}
function head(title, sub, actions, back) {
  return h('div', { class: 'head' },
    back ? h('button', { class: 'ibtn back', 'aria-label': t('back'), onclick: () => go(back), html: icon('back') }) : null,
    h('div', { class: 'titles' }, h('h2', {}, title), sub ? h('div', { class: 'sub' }, sub) : null),
    actions || null);
}
function emptyState(text, art) {
  return h('div', { class: 'empty' }, h('div', { html: art || ART.chat }), h('div', {}, text));
}
const ART = {
  chat: `<svg class="art" viewBox="0 0 140 110" fill="none"><rect x="14" y="18" width="78" height="50" rx="14" fill="var(--panel-2)" stroke="var(--line)" stroke-width="2"/><path d="M30 68v16l16-16" fill="var(--panel-2)" stroke="var(--line)" stroke-width="2"/><rect x="58" y="44" width="68" height="40" rx="12" fill="var(--accent)" opacity=".85"/><circle cx="78" cy="64" r="4" fill="#fff"/><circle cx="92" cy="64" r="4" fill="#fff"/><circle cx="106" cy="64" r="4" fill="#fff"/><path d="M30 36h44M30 48h30" stroke="var(--ink-3)" stroke-width="4" stroke-linecap="round"/></svg>`,
  mesh: `<svg class="art" viewBox="0 0 140 110" fill="none" stroke="var(--line)" stroke-width="3"><path d="M30 70 70 30l40 40M30 70h80M70 30v60"/><circle cx="30" cy="70" r="10" fill="var(--panel-2)"/><circle cx="70" cy="30" r="12" fill="var(--accent)" stroke="none"/><circle cx="110" cy="70" r="10" fill="var(--panel-2)"/><circle cx="70" cy="90" r="9" fill="var(--panel-2)"/></svg>`,
};

// ------------------------------------------------------------------ conversations view
V.chats = {
  list() {
    const all = convList();
    const q = S.search.toLowerCase();
    const archived = all.filter(c => c.archived);
    const items = all.filter(c => !!c.archived === S.showArchived)
      .filter(c => !q || c.name.toLowerCase().includes(q) || (c.preview || '').toLowerCase().includes(q))
      .sort((a, b) => (b.pinned - a.pinned) || (b.lastTs || 0) - (a.lastTs || 0) || a.name.localeCompare(b.name));
    return [
      head(S.showArchived ? t('archived') : t('nav.chats'), null, h('div', { class: 'row' },
        S.showArchived ? h('button', { class: 'ibtn', 'aria-label': t('back'), html: icon('back'), onclick: () => { S.showArchived = false; renderList(); } }) : null,
        h('button', { class: 'ibtn', title: t('new_chat'), 'aria-label': t('new_chat'), html: icon('plus'), onclick: newChatDialog }))),
      h('div', { class: 'search' }, h('input', { type: 'search', placeholder: t('search'), 'aria-label': t('search'), value: S.search, oninput: e => { S.search = e.target.value; searchSoon(); } })),
      h('div', { class: 'scroll', role: 'list' },
        !S.showArchived && archived.length ? h('div', { class: 'item', onclick: () => { S.showArchived = true; renderList(); } }, h('div', { class: 'avatar sm', style: { background: 'var(--panel-2)', color: 'var(--ink-2)' }, html: icon('archive') }), h('div', { class: 'body' }, t('archived_n', { n: archived.length }))) : null,
        items.length ? items.map(convItem) : h('div', { class: 'empty' }, S.contacts.length ? t('no_conv') : h('div', {}, h('div', { html: ART.mesh }), t('no_conv_no_module')))),
    ];
  },
  detail() {
    if (!S.sel) return emptyState(t('pick_conv'));
    return chatView(S.sel);
  },
  detailMounted() {
    const box = $('#msgs');
    if (box) box.scrollTop = box.scrollHeight;
    if (S.sel && matchMedia('(min-width: 761px)').matches) $('#composer')?.focus();
  },
};

function convItem(c) {
  const contact = contactByConv(c.id);
  const prev = (c.lastOut ? t('you') + ': ' : c.lastAuthor && c.kind !== 0 ? c.lastAuthor + ': ' : '') + (c.preview || '');
  return h('div', { class: 'item' + (S.sel === c.id ? ' on' : ''), role: 'listitem', tabindex: 0, onclick: () => go(`#/chats/${c.id}`), onkeydown: e => { if (e.key === 'Enter') go(`#/chats/${c.id}`); } },
    avatar(c.name, contact?.key || c.id, c.kind),
    h('div', { class: 'body' },
      h('div', { class: 'top' }, h('span', { class: 'name' }, c.name, c.pinned ? I('pin') : null), h('span', { class: 'time' }, fmtListTime(c.lastTs))),
      h('div', { class: 'bottom' }, h('span', { class: 'prev' }, prev || (c.kind === 1 ? t('channel') : '')),
        h('span', { class: 'flags' }, c.muted ? I('mute') : null),
        c.unread ? h('span', { class: 'badge' + (c.muted ? ' muted' : '') }, c.unread) : null)),
    h('button', { class: 'ibtn more', 'aria-label': t('options'), html: icon('more'), onclick: e => { e.stopPropagation(); convMenu(e.currentTarget, c); } }));
}
function convMenu(el, c) {
  const set = async (k, v) => { await apiDo('flags', { conv: c.id, [k]: v }); const x = S.convs.find(y => y.id === c.id); if (x) x[k] = v; else S.convs.push({ ...c, [k]: v }); renderList(); };
  menu(el, [
    { icon: 'pin', label: c.pinned ? t('unpin') : t('pin'), run: () => set('pinned', !c.pinned) },
    { icon: c.muted ? 'bell' : 'mute', label: c.muted ? t('unmute') : t('mute'), run: () => set('muted', !c.muted) },
    { icon: 'archive', label: c.archived ? t('unarchive') : t('archive'), run: () => set('archived', !c.archived) },
    c.unread ? { icon: 'check', label: t('mark_read'), run: async () => { await apiDo('read', { conv: c.id }); c.unread = 0; const x = S.convs.find(y => y.id === c.id); if (x) x.unread = 0; renderList(); renderNav(); } } : null,
    { icon: 'trash', label: t('delete_history'), danger: true, run: async () => { if (await confirmDlg(t('delete_history'), t('delete_history_q', { n: c.name }), t('delete'), true)) { await apiDo('delete_conv', { conv: c.id }); S.convs = S.convs.filter(x => x.id !== c.id); delete S.msgs[c.id]; if (S.sel === c.id) go('#/chats'); else renderList(); } } },
  ]);
}
async function newChatDialog() {
  const list = h('div', { class: 'netlist' });
  const fill = q => {
    list.innerHTML = '';
    const items = [...S.channels.map(ch => ({ id: ch.conv, name: ch.name, kind: 1, seed: ch.conv })),
      ...S.contacts.filter(c => c.type === 1 || c.type === 3).map(c => ({ id: c.conv, name: c.name, kind: TYPE_KIND[c.type], seed: c.key }))]
      .filter(x => x.name.toLowerCase().includes(q.toLowerCase()));
    for (const it of items) list.append(h('div', { class: 'item', onclick: () => { $('.overlay')?.remove(); go(`#/chats/${it.id}`); } }, avatar(it.name, it.seed, it.kind, 'sm'), h('div', { class: 'body' }, h('span', { class: 'name' }, it.name))));
    if (!items.length) list.append(h('div', { class: 'empty' }, t('no_contacts')));
  };
  fill('');
  dialog(t('new_chat'), h('div', {}, h('input', { type: 'search', placeholder: t('search'), oninput: e => fill(e.target.value) }), list), [{ label: t('close'), value: null }]);
}

// ------------------------------------------------------------------ chat view
const msgLoads = {};  // conv -> in-flight first-page load (re-renders must not request it again)
function loadMessages(conv, before) {
  if (before) return fetchMessages(conv, before);
  return msgLoads[conv] || (msgLoads[conv] = fetchMessages(conv).finally(() => delete msgLoads[conv]));
}
async function fetchMessages(conv, before) {
  const j = await apiGet(`/api/messages?conv=${encodeURIComponent(conv)}&limit=40${before ? '&before=' + before : ''}`);
  S.maxLen[conv] = j.max;
  S.more[conv] = j.messages.length >= 40;
  const cur = S.msgs[conv] || [];
  S.msgs[conv] = before ? [...j.messages, ...cur.filter(m => !j.messages.some(x => x.i === m.i))] : j.messages;
}

function chatView(conv) {
  const kind = convKind(conv), name = convName(conv), contact = contactByConv(conv);
  const c = S.convs.find(x => x.id === conv);
  let sub = kind === 1 ? t('channel') : contact ? `${t('type.' + contact.type)} · ${contact.hops < 0 ? t('route.flood') : contact.hops === 0 ? t('route.direct') : t('route.hops', { n: contact.hops })} · ${t('seen', { t: ago(contact.lastAdvert) })}` : t('unknown_contact');
  const actions = h('div', { class: 'row' },
    contact ? h('button', { class: 'ibtn', title: t('contact_info'), 'aria-label': t('contact_info'), html: icon('info'), onclick: () => go(`#/contacts/${contact.key}`) }) : null,
    kind === 1 ? h('button', { class: 'ibtn', title: t('channel_info'), 'aria-label': t('channel_info'), html: icon('info'), onclick: () => go(`#/channels/${conv.slice(3)}`) }) : null,
    h('button', { class: 'ibtn', 'aria-label': t('options'), html: icon('more'), onclick: e => convMenu(e.currentTarget, c || { id: conv, name, kind }) }));
  const out = [head(name, sub, actions, '#/chats')];

  if (kind === 2) out.push(repeaterPanel(conv, contact));
  if (kind === 3 && !S.logins[conv]?.ok) out.push(roomBanner(conv));

  const box = h('div', { class: 'msgs', id: 'msgs', role: 'log', 'aria-live': 'polite', onscroll: e => { if (e.target.scrollTop < 60 && S.more[conv] && !S.loadingMore) loadOlder(conv); } });
  if (!S.msgs[conv]) {
    box.append(h('div', { class: 'typing-skel', style: { width: '40%' } }), h('div', { class: 'typing-skel', style: { width: '55%', alignSelf: 'flex-end' } }), h('div', { class: 'typing-skel', style: { width: '30%' } }));
    loadMessages(conv).then(() => { if (S.sel === conv) { fillMsgs(box, conv); box.scrollTop = box.scrollHeight; } }).catch(() => {});
  } else fillMsgs(box, conv);
  out.push(box);
  if (kind !== 2) out.push(composer(conv, kind));
  if (c?.unread) { apiDo('read', { conv }, true); c.unread = 0; renderNav(); setTimeout(renderList, 0); }
  return out;
}
async function loadOlder(conv) {
  const list = S.msgs[conv];
  if (!list?.length) return;
  S.loadingMore = true;
  const box = $('#msgs'), h0 = box.scrollHeight;
  try { await loadMessages(conv, list[0].i); } finally { S.loadingMore = false; }
  if (S.sel !== conv) return;
  fillMsgs(box, conv);
  box.scrollTop = box.scrollHeight - h0;
}
function fillMsgs(box, conv) {
  box.innerHTML = '';
  const list = (S.msgs[conv] || []).filter(m => convKind(conv) !== 2 || m.y !== 1);
  if (S.more[conv]) box.append(h('div', { class: 'loadmore' }, t('scroll_more')));
  if (!list.length) box.append(emptyState(convKind(conv) === 1 ? t('no_msgs_channel') : t('no_msgs')));
  list.forEach((m, i) => {
    if (i === 0 || new Date(m.t * 1000).toDateString() !== new Date(list[i - 1].t * 1000).toDateString()) box.append(h('div', { class: 'daysep' }, fmtDay(m.t)));
    box.append(bubble(m, conv, list, i));
  });
}
function appendBubble(m, list) {
  const box = $('#msgs');
  if (!box) return;
  if (convKind(S.sel) === 2 && m.y === 1) return;
  box.querySelector('.empty')?.remove();
  const near = box.scrollHeight - box.scrollTop - box.clientHeight < 160;
  const prev = list[list.length - 2];
  if (!prev || new Date(prev.t * 1000).toDateString() !== new Date(m.t * 1000).toDateString()) box.append(h('div', { class: 'daysep' }, fmtDay(m.t)));
  box.append(bubble(m, S.sel, list, list.length - 1));
  if (near || m.d) box.scrollTop = box.scrollHeight;
}
const STATUS_ICON = ['clock', 'check', 'checks', 'alert'];
function renderText(txt) {
  // @[Name] mentions like the official app
  const frag = document.createDocumentFragment();
  let last = 0;
  for (const mt of txt.matchAll(/@\[([^\]]{1,32})\]/g)) {
    frag.append(txt.slice(last, mt.index), h('span', { class: 'mention' }, '@' + mt[1]));
    last = mt.index + mt[0].length;
  }
  frag.append(txt.slice(last));
  return frag;
}
function bubble(m, conv, list, idx) {
  const kind = convKind(conv);
  if (idx == null) idx = list.indexOf(m);
  const prev = list[idx - 1];
  const showAuthor = !m.d && (kind === 1 || kind === 3) && m.a && !(prev && !prev.d && prev.a === m.a && m.t - prev.t < 300);
  const meta = h('div', { class: 'meta' });
  if (!m.d && (m.s != null || m.h != null)) {
    const parts = [];
    if (m.s != null) parts.push(`SNR ${m.s.toFixed(1)}`);
    if (m.h != null) parts.push(m.h === -1 ? t('route.direct') : t('route.hops', { n: m.h }));
    meta.append(h('span', { title: t('radio_info') }, parts.join(' · ')));
  }
  meta.append(h('span', {}, fmtTime(m.t)));
  if (m.d) {
    const k = m.k ?? 0;
    const title = kind === 1 && k === 1 ? t('st.sent_channel') : t('msgst.' + k) + (m.r ? ` · ${(m.r / 1000).toFixed(1)} s` : '');
    meta.append(h('span', { title, class: k === 3 ? 'st-failed' : '', html: icon(STATUS_ICON[k] || 'clock') }));
    if (k === 2 && m.r) meta.append(h('span', {}, `${(m.r / 1000).toFixed(1)} s`));
  }
  const color = `hsl(${hue(m.ak || m.a)} 55% ${document.documentElement.dataset.theme === 'dark' || matchMedia('(prefers-color-scheme: dark)').matches ? 65 : 38}%)`;
  return h('div', { class: 'msg' + (m.d ? ' out' : '') + (m.y === 1 ? ' cli' : ''), 'data-id': m.i },
    showAuthor ? h('div', { class: 'author', style: { color } }, m.a) : null,
    h('div', { class: 'bubble' }, renderText(m.x), meta),
    m.d && m.k === 3 ? h('div', { class: 'failed-note' }, I('alert'), t('not_delivered'), h('button', { onclick: () => apiDo('resend', { conv, id: m.i }) }, t('resend'))) : null,
    h('div', { class: 'acts' },
      kind !== 0 && !m.d && m.a ? h('button', { onclick: () => insertReply(conv, m.a) }, I('reply'), t('reply')) : null,
      h('button', { onclick: () => copy(m.x) }, I('copy'), t('copy'))));
}
function insertReply(conv, name) {
  const ta = $('#composer');
  if (!ta) return;
  ta.value = `@[${name}] ` + ta.value.replace(/^@\[[^\]]*\]\s*/, '');
  ta.dispatchEvent(new Event('input'));
  ta.focus();
}
const EMOJIS = '😀 😂 😊 😍 🤔 😎 😢 😮 😴 🙏 👍 👎 👌 👋 🙌 💪 🤝 ✌️ 🎉 🔥 ❤️ ⭐ ✅ ❌ ⚠️ 📡 📶 🔋 🛰️ 🗺️ 📍 🏠 🚗 🚲 🏔️ 🌲 ☀️ 🌧️ ❄️ ⚡ 🆘 ☕ 🍺 🍕 ⏰ 📢 💬 👀 🤖'.split(' ');
function composer(conv, kind) {
  const max = () => S.maxLen[conv] || (kind === 1 ? 140 : 160);
  const count = h('div', { class: 'count' });
  const btn = h('button', { class: 'send', 'aria-label': t('send'), html: icon('send'), disabled: true });
  const ta = h('textarea', { id: 'composer', rows: 1, placeholder: S.status.link?.state === 'ready' ? t('type_message') : t('type_message_offline'), 'aria-label': t('type_message') });
  ta.value = S.drafts[conv] || '';
  const update = () => {
    const n = bytes(ta.value), m = max();
    count.textContent = `${n} / ${m} ${t('bytes')}`;
    count.className = 'count' + (n > m ? ' over' : '');
    btn.disabled = !ta.value.trim() || n > m;
    ta.style.height = 'auto';
    ta.style.height = Math.min(140, ta.scrollHeight + 2) + 'px';
    S.drafts[conv] = ta.value;
  };
  const send = async () => {
    const text = ta.value.trim();
    if (!text || bytes(text) > max()) return;
    btn.disabled = true;
    const j = await apiDo('send', { conv, text });
    if (j.ok) { ta.value = ''; S.drafts[conv] = ''; update(); }
    else btn.disabled = false;
    ta.focus();
  };
  ta.addEventListener('input', update);
  ta.addEventListener('keydown', e => { if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) { e.preventDefault(); send(); } });
  btn.addEventListener('click', send);
  const emo = h('button', { class: 'ibtn', 'aria-label': t('emoji'), html: icon('smile'), onclick: e => {
    document.querySelectorAll('.menu').forEach(m => m.remove());
    const m = h('div', { class: 'menu' }, h('div', { class: 'emojis' }, EMOJIS.map(x => h('button', { 'aria-label': x, onclick: () => { const p = ta.selectionStart ?? ta.value.length; ta.value = ta.value.slice(0, p) + x + ta.value.slice(p); update(); ta.focus(); } }, x))));
    document.body.append(m);
    const r = e.currentTarget.getBoundingClientRect();
    m.style.left = Math.max(8, r.left) + 'px'; m.style.top = (r.top - m.offsetHeight - 6) + 'px';
    setTimeout(() => document.addEventListener('click', function off(ev) { if (!m.contains(ev.target)) { m.remove(); document.removeEventListener('click', off); } }), 0);
  } });
  setTimeout(update, 0);
  return h('div', { class: 'composer' }, h('div', { class: 'bar' }, emo, ta, btn), count);
}
function roomBanner(conv) {
  const pw = h('input', { type: 'password', placeholder: t('password'), 'aria-label': t('room_password') });
  const remember = h('input', { type: 'checkbox', checked: true });
  const go_ = async () => { const r = await apiDo('login', { conv, password: pw.value, remember: remember.checked }); if (r.ok) toast(t('login_sent')); };
  pw.addEventListener('keydown', e => { if (e.key === 'Enter') go_(); });
  return h('div', { class: 'banner' }, I('lock'), h('span', { class: 'grow' }, t('room_login_hint')), pw,
    h('label', { class: 'row', style: { gap: '4px', fontSize: '13px' } }, remember, t('remember')),
    h('button', { class: 'btn pri sm', onclick: go_ }, t('login')));
}

// ------------------------------------------------------------------ repeater admin panel
function repeaterPanel(conv, contact) {
  const st = S.rstatus[conv], lg = S.logins[conv], tel = S.telemetry[conv];
  const pw = h('input', { type: 'password', placeholder: t('admin_password') });
  const term = h('div', { class: 'term', id: 'term' });
  for (const m of (S.msgs[conv] || []).filter(x => x.y === 1 || x.y === 3)) term.append(h('div', { class: m.d ? 'in' : '' }, (m.d ? '> ' : '') + m.x));
  setTimeout(() => { term.scrollTop = term.scrollHeight; }, 0);
  const cmd = h('input', { type: 'text', placeholder: t('cli_placeholder'), class: 'mono', 'aria-label': t('cli') });
  cmd.addEventListener('keydown', async e => {
    if (e.key !== 'Enter' || !cmd.value.trim()) return;
    const v = cmd.value.trim(); cmd.value = '';
    await apiDo('cli', { conv, text: v });
  });
  return h('div', { class: 'scroll' },
    h('div', { class: 'card' }, h('h3', {}, I('lock'), t('admin_login')),
      lg?.ok ? h('div', { class: 'notice ok' }, I('check'), lg.admin ? t('logged_admin') : t('logged_guest')) : h('p', { class: 'hint' }, t('admin_login_hint')),
      h('div', { class: 'row' }, pw, h('button', { class: 'btn pri', onclick: () => apiDo('login', { conv, password: pw.value, remember: true }).then(r => r.ok && toast(t('login_sent'))) }, t('login')),
        lg?.ok ? h('button', { class: 'btn', onclick: () => apiDo('logout', { conv }).then(() => { delete S.logins[conv]; renderDetail(); }) }, t('logout')) : null)),
    h('div', { class: 'card' }, h('h3', {}, I('stats'), t('repeater_status')),
      st ? statusGrid(st) : h('p', { class: 'hint' }, t('status_hint')),
      h('div', { class: 'btns' }, h('button', { class: 'btn', onclick: () => meshCmd('status_req', { conv }, t('request_sent')) }, I('refresh'), t('get_status')),
        h('button', { class: 'btn', onclick: () => meshCmd('telemetry_req', { conv }, t('request_sent')) }, I('thermo'), t('get_telemetry'))),
      tel ? lppView(tel.lpp) : null),
    h('div', { class: 'card' }, h('h3', {}, I('terminal'), t('cli')), h('p', { class: 'hint' }, t('cli_hint')), term, h('div', { style: { marginTop: '8px' } }, cmd)));
}
function statusGrid(s) {
  const cell = (v, l) => h('div', {}, h('b', {}, v), h('span', {}, l));
  return h('div', { class: 'stat' },
    cell(s.battMv ? (s.battMv / 1000).toFixed(2) + ' V' : '–', t('battery')),
    cell(fmtDuration(s.uptime), t('uptime')),
    cell(s.noiseFloor + ' dBm', t('noise_floor')),
    cell(s.lastRssi + ' dBm', 'RSSI'),
    cell(s.lastSnr.toFixed(1) + ' dB', 'SNR'),
    cell(s.nRecv, t('pkts_recv')), cell(s.nSent, t('pkts_sent')),
    cell(`${s.recvFlood} / ${s.recvDirect}`, t('recv_flood_direct')),
    cell(`${s.sentFlood} / ${s.sentDirect}`, t('sent_flood_direct')),
    cell(s.airtime + ' s', t('tx_airtime')), cell((s.rxAirtime ?? 0) + ' s', t('rx_airtime')),
    cell(`${s.directDups} / ${s.floodDups}`, t('dups')), cell(s.txQueue, t('tx_queue')),
    s.recvErrors != null ? cell(s.recvErrors, t('recv_errors')) : null);
}
// Cayenne LPP (MeshCore telemetry)
function parseLpp(hex) {
  const b = hexToBytes(hex), out = [];
  const T = { 0: ['digital_in', 1, 1], 1: ['digital_out', 1, 1], 2: ['analog', 2, 100, 1], 3: ['analog_out', 2, 100, 1], 100: ['generic', 4, 1], 101: ['lux', 2, 1, 0, 'lx'],
    102: ['presence', 1, 1], 103: ['temperature', 2, 10, 1, '°C'], 104: ['humidity', 1, 2, 0, '%'], 115: ['pressure', 2, 10, 0, 'hPa'], 116: ['voltage', 2, 100, 0, 'V'],
    117: ['current', 2, 1000, 0, 'A'], 118: ['frequency', 4, 1, 0, 'Hz'], 120: ['percentage', 1, 1, 0, '%'], 121: ['altitude', 2, 1, 1, 'm'], 125: ['concentration', 2, 1, 0, 'ppm'],
    128: ['power', 2, 1, 0, 'W'], 130: ['distance', 4, 1000, 0, 'm'], 131: ['energy', 4, 1000, 0, 'kWh'], 132: ['direction', 2, 1, 0, '°'], 133: ['unixtime', 4, 1], 142: ['switch', 1, 1] };
  const rd = (o, n, signed) => { let v = 0; for (let k = 0; k < n; k++) v = v * 256 + b[o + k]; if (signed && v >= 2 ** (8 * n - 1)) v -= 2 ** (8 * n); return v; };
  let i = 0;
  while (i + 2 <= b.length) {
    const ch = b[i], ty = b[i + 1]; i += 2;
    if (ty === 136) { if (i + 9 > b.length) break; out.push({ ch, name: 'gps', value: `${(rd(i, 3, 1) / 1e4).toFixed(4)}, ${(rd(i + 3, 3, 1) / 1e4).toFixed(4)} · ${(rd(i + 6, 3, 1) / 100).toFixed(0)} m` }); i += 9; continue; }
    if (ty === 113 || ty === 134) { if (i + 6 > b.length) break; const m = ty === 113 ? 1000 : 100; out.push({ ch, name: ty === 113 ? 'accel' : 'gyro', value: [0, 2, 4].map(o => (rd(i + o, 2, 1) / m).toFixed(2)).join(' / ') }); i += 6; continue; }
    if (ty === 135) { out.push({ ch, name: 'colour', value: '#' + bytesToHex(b.slice(i, i + 3)) }); i += 3; continue; }
    const d = T[ty];
    if (!d || i + d[1] > b.length) break;
    out.push({ ch, name: d[0], value: +(rd(i, d[1], d[3]) / d[2]).toFixed(3) + (d[4] ? ' ' + d[4] : '') });
    i += d[1];
  }
  return out;
}
function lppView(hex) {
  const items = parseLpp(hex);
  if (!items.length) return h('p', { class: 'hint' }, t('no_telemetry'));
  return h('div', { class: 'stat', style: { marginTop: '12px' } }, items.map(x => h('div', {}, h('b', {}, String(x.value)), h('span', {}, `${t('lpp.' + x.name)} · ch${x.ch}`))));
}

// ------------------------------------------------------------------ login
function showLogin() {
  if (S.loginShown) return;
  S.loginShown = true; S.authed = false;
  const app = $('#app');
  app.innerHTML = '';
  const pw = h('input', { type: 'password', id: 'pw', autocomplete: 'current-password', 'aria-label': t('password') });
  const err = h('div', { class: 'notice err', style: { display: 'none' } });
  const submit = async e => {
    e?.preventDefault();
    let j;
    try {
      const r = await fetch('/api/login', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ password: pw.value }) });
      j = await r.json();
    } catch {
      j = { err: 'network' };  // no answer or not JSON: never claim the password is wrong
    }
    if (j.ok) { S.loginShown = false; start(); }
    else { err.style.display = ''; err.textContent = t('err.' + (j.err || 'network')); pw.select(); }
  };
  app.append(h('div', { class: 'login' }, h('form', { class: 'dialog', onsubmit: submit },
    h('div', { class: 'row', style: { marginBottom: '10px' } }, h('div', { html: LOGO, style: { display: 'contents' } }), h('h2', { style: { margin: 0 } }, 'MeshCore Hub')),
    h('p', { class: 'hint' }, t('login_hint')), h('label', { class: 'f', for: 'pw' }, t('password')), pw, err,
    h('div', { class: 'btns' }, h('button', { class: 'btn pri', type: 'submit' }, I('lock'), t('login'))))));
  setTimeout(() => pw.focus(), 50);
}

// ------------------------------------------------------------------ start
async function start() {
  let st;
  try { st = await (await fetch('/api/status', { cache: 'no-store' })).json(); }
  catch { $('#app').innerHTML = `<div class="empty">${esc(t('err.network'))}</div>`; setTimeout(start, 3000); return; }
  await loadLang(store.get('lang', st.lang || 'fr'));
  S.status = st;
  applyTheme();
  if (!st.auth) return showLogin();
  S.authed = true;
  // no NTP (access point mode): give the hub the browser time
  if (!st.timeValid) apiDo('time', { unix: Math.floor(Date.now() / 1000) }, true);
  if (!st.setupDone) { connectWs(); return startWizard(); }
  shell();
  connectWs();
  loadMeshData();
  window.addEventListener('hashchange', route);
  route();
}
document.addEventListener('visibilitychange', () => {
  if (!document.hidden && S.view === 'chats' && S.sel) {
    const c = S.convs.find(x => x.id === S.sel);
    if (c?.unread) { apiDo('read', { conv: S.sel }, true); c.unread = 0; renderNav(); renderList(); }
  }
});
window.addEventListener('DOMContentLoaded', start);
