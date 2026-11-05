'use strict';
/* MeshCore Hub web app - contacts, channels, map, tools, node and hub settings. */

// ------------------------------------------------------------------ contacts
const CONTACT_FILTERS = [['all', 'filter.all'], ['fav', 'filter.favorites'], [1, 'filter.people'], [2, 'filter.repeaters'], [3, 'filter.rooms'], [4, 'filter.sensors']];
V.contacts = {
  list() {
    const q = S.search.toLowerCase(), me = S.status.node;
    const inFilter = (c, v) => v === 'all' || (v === 'fav' ? (c.flags & 1) : c.type === v);
    const items = S.contacts.filter(c => inFilter(c, S.contactFilter) && (!q || c.name.toLowerCase().includes(q) || c.key.startsWith(q)))
      .sort((a, b) => (b.flags & 1) - (a.flags & 1) || b.lastAdvert - a.lastAdvert);
    return [
      head(t('nav.contacts'), t('n_contacts', { n: S.contacts.length }), h('button', { class: 'ibtn', title: t('import_contact'), 'aria-label': t('import_contact'), html: icon('plus'), onclick: importContactDialog })),
      h('div', { class: 'search' }, h('input', { type: 'search', placeholder: t('search'), value: S.search, oninput: e => { S.search = e.target.value; searchSoon(); } })),
      h('div', { class: 'chips', role: 'tablist' }, CONTACT_FILTERS.map(([v, l]) => h('button', { class: 'chip' + (S.contactFilter === v ? ' on' : ''), role: 'tab', 'aria-selected': S.contactFilter === v, onclick: () => { S.contactFilter = v; renderList(); } }, t(l), ' ', h('span', { style: { opacity: .7 } }, S.contacts.filter(c => inFilter(c, v)).length)))),
      h('div', { class: 'scroll', role: 'list' }, items.length ? items.map(c => {
        const d = distKm(me, c);
        return h('div', { class: 'item' + (S.sel === c.key ? ' on' : ''), role: 'listitem', tabindex: 0, onclick: () => go(`#/contacts/${c.key}`), onkeydown: e => { if (e.key === 'Enter') go(`#/contacts/${c.key}`); } },
          avatar(c.name, c.key, TYPE_KIND[c.type]),
          h('div', { class: 'body' },
            h('div', { class: 'top' }, h('span', { class: 'name' }, c.name, c.flags & 1 ? I('star') : null), h('span', { class: 'time' }, d != null ? fmtKm(d) : '')),
            h('div', { class: 'bottom' }, h('span', { class: 'prev' }, `${t('type.' + c.type)} · ${t('seen', { t: ago(c.lastAdvert) })}`),
              h('span', { class: 'time' }, c.hops < 0 ? '' : c.hops === 0 ? t('route.direct') : t('route.hops', { n: c.hops })))));
      }) : h('div', { class: 'empty' }, h('div', { html: ART.mesh }), S.contacts.length ? t('no_match') : t('no_contacts'))),
    ];
  },
  detail() {
    const c = S.sel && contactByKey(S.sel);
    if (!c) return emptyState(t('pick_contact'), ART.mesh);
    const conv = c.conv, kind = TYPE_KIND[c.type];
    const route = c.hops < 0 ? t('route.unknown') : c.hops === 0 ? t('route.direct') : `${t('route.hops', { n: c.hops })} · ${c.path.match(new RegExp(`.{${c.hashSize * 2}}`, 'g'))?.map(x => hashName(x)).join(' → ')}`;
    const d = distKm(S.status.node, c);
    const out = [head(c.name, t('type.' + c.type), null, '#/contacts'), h('div', { class: 'scroll' }, h('div', { class: 'cards' },
      h('div', { class: 'card' },
        h('div', { class: 'row', style: { gap: '16px', marginBottom: '12px' } }, avatar(c.name, c.key, kind, 'lg'), h('div', {}, h('h3', {}, c.name), h('div', { class: 'hint' }, t('seen', { t: ago(c.lastAdvert) })))),
        h('dl', { class: 'kv' },
          h('dt', {}, t('public_key')), h('dd', { class: 'mono' }, c.key.slice(0, 16) + '…', ' ', h('button', { class: 'btn sm', onclick: () => copy(c.key) }, I('copy'))),
          h('dt', {}, t('route')), h('dd', {}, route),
          c.lat || c.lon ? [h('dt', {}, t('position')), h('dd', {}, `${c.lat.toFixed(5)}, ${c.lon.toFixed(5)}${d != null ? ' · ' + fmtKm(d) : ''}`)] : null,
          h('dt', {}, t('last_advert')), h('dd', {}, c.lastAdvert ? new Date(c.lastAdvert * 1000).toLocaleString(LANG) : '–')),
        h('div', { class: 'btns' },
          kind !== 4 ? h('button', { class: 'btn pri', onclick: () => go(`#/chats/${conv}`) }, I(kind === 2 ? 'terminal' : 'chat'), kind === 2 ? t('admin') : t('open_chat')) : null,
          h('button', { class: 'btn', onclick: () => meshCmd('contact_fav', { key: c.key, fav: !(c.flags & 1) }) }, I('star'), c.flags & 1 ? t('unfavorite') : t('favorite')),
          h('button', { class: 'btn', onclick: () => meshCmd('contact_reset_path', { key: c.key }, t('path_reset')) }, I('route'), t('reset_path')),
          h('button', { class: 'btn', onclick: () => exportContact(c) }, I('qr'), t('share_qr')),
          h('button', { class: 'btn', onclick: () => meshCmd('contact_share', { key: c.key }, t('shared_zero_hop')) }, I('share'), t('share_mesh')),
          h('button', { class: 'btn danger', onclick: async () => { if (await confirmDlg(t('remove_contact'), t('remove_contact_q', { n: c.name }), t('delete'), true)) { if (await meshCmd('contact_remove', { key: c.key }, t('removed'))) go('#/contacts'); } } }, I('trash'), t('delete')))),
      kind === 3 ? roomCard(c) : null,
      kind === 2 || kind === 4 ? h('div', { class: 'card' }, h('h3', {}, I('thermo'), t('telemetry')),
        S.telemetry[conv] ? lppView(S.telemetry[conv].lpp) : h('p', { class: 'hint' }, t('telemetry_hint')),
        h('div', { class: 'btns' }, h('button', { class: 'btn', onclick: () => meshCmd('telemetry_req', { conv }, t('request_sent')) }, I('refresh'), t('get_telemetry')))) : null))];
    return out;
  },
};
function hashName(hex) {
  const m = S.contacts.filter(c => c.key.startsWith(hex));
  return m.length === 1 ? m[0].name : hex;
}
function roomCard(c) {
  const lg = S.logins[c.conv];
  const pw = h('input', { type: 'password', placeholder: t('password') });
  return h('div', { class: 'card' }, h('h3', {}, I('home'), t('room')),
    lg?.ok ? h('div', { class: 'notice ok' }, I('check'), t('logged_in')) : h('p', { class: 'hint' }, t('room_hint')),
    h('div', { class: 'row' }, pw, h('button', { class: 'btn pri', onclick: () => apiDo('login', { conv: c.conv, password: pw.value, remember: true }).then(r => r.ok && toast(t('login_sent'))) }, t('login')),
      h('button', { class: 'btn', onclick: () => apiDo('logout', { conv: c.conv }).then(() => { delete S.logins[c.conv]; renderDetail(); toast(t('logged_out')); }) }, t('logout'))));
}
async function exportContact(c) {
  const r = await meshCmd('contact_export', c ? { key: c.key } : {});
  if (!r?.data) return;
  const uri = 'meshcore://' + r.data.slice(2);  // drop RESP_CODE_EXPORT_CONTACT
  dialog(c ? c.name : t('my_card'), h('div', { style: { textAlign: 'center' } }, qrSvg(uri), h('p', { class: 'hint mono', style: { wordBreak: 'break-all', textAlign: 'left' } }, uri), h('button', { class: 'btn', onclick: () => copy(uri) }, I('copy'), t('copy_link'))));
}
async function importContactDialog() {
  const ta = h('textarea', { rows: 4, placeholder: 'meshcore://…', class: 'mono' });
  const v = await dialog(t('import_contact'), h('div', {}, h('p', { class: 'hint' }, t('import_contact_hint')), ta), [{ label: t('cancel'), value: null }, { label: t('import'), value: () => ta.value.trim(), pri: true }]);
  if (!v) return;
  if (v.startsWith('meshcore://contact/add')) {
    const p = new URL(v.replace('meshcore://', 'http://x/')).searchParams;
    await meshCmd('contact_add', { key: p.get('public_key'), name: p.get('name'), type: +(p.get('type') || 1) }, t('imported'));
  } else if (v.startsWith('meshcore://channel/add')) {
    const p = new URL(v.replace('meshcore://', 'http://x/')).searchParams;
    await meshCmd('channel_set', { name: p.get('name'), secret: p.get('secret') }, t('channel_added'));
  } else {
    await meshCmd('contact_import', { hex: v }, t('imported'));
  }
}

// ------------------------------------------------------------------ channels
V.channels = {
  list() {
    return [
      head(t('nav.channels'), t('n_of_m', { n: S.channels.length, m: S.maxChannels }), h('button', { class: 'ibtn', title: t('add_channel'), 'aria-label': t('add_channel'), html: icon('plus'), onclick: () => go('#/channels/new') })),
      h('div', { class: 'scroll' }, S.channels.length ? S.channels.map(ch => h('div', { class: 'item' + (S.sel === String(ch.idx) ? ' on' : ''), tabindex: 0, onclick: () => go(`#/channels/${ch.idx}`) },
        avatar(ch.name, ch.conv, 1), h('div', { class: 'body' }, h('div', { class: 'top' }, h('span', { class: 'name' }, ch.name), h('span', { class: 'time' }, '#' + ch.idx)),
          h('div', { class: 'bottom' }, h('span', { class: 'prev' }, ch.name.startsWith('#') ? t('hashtag_channel') : ch.idx === 0 ? t('public_channel') : t('private_channel')))))) : h('div', { class: 'empty' }, t('no_channels'))),
    ];
  },
  detail() {
    if (S.sel === 'new') return channelForm();
    const ch = S.channels.find(c => String(c.idx) === S.sel);
    if (!ch) return emptyState(t('pick_channel'), ART.mesh);
    const link = `meshcore://channel/add?name=${encodeURIComponent(ch.name)}&secret=${ch.secret}`;
    let shown = false;
    const sec = h('dd', { class: 'mono' }, '•'.repeat(16), ' ');
    const toggle = h('button', { class: 'btn sm', onclick: () => { shown = !shown; sec.firstChild.textContent = shown ? ch.secret : '•'.repeat(16); } }, I('eye'));
    sec.append(toggle);
    return [head(ch.name, t('slot', { n: ch.idx }), null, '#/channels'), h('div', { class: 'scroll' }, h('div', { class: 'cards' },
      h('div', { class: 'card' }, h('dl', { class: 'kv' }, h('dt', {}, t('name')), h('dd', {}, ch.name), h('dt', {}, t('secret_key')), sec),
        ch.name.startsWith('#') ? h('div', { class: 'notice' }, I('info'), t('hashtag_public_warning')) : null,
        h('div', { class: 'btns' }, h('button', { class: 'btn pri', onclick: () => go(`#/chats/${ch.conv}`) }, I('chat'), t('open_chat')),
          h('button', { class: 'btn', onclick: () => copy(link) }, I('copy'), t('copy_link')),
          ch.idx !== 0 ? h('button', { class: 'btn danger', onclick: async () => { if (await confirmDlg(t('delete_channel'), t('delete_channel_q', { n: ch.name }), t('delete'), true)) { if (await meshCmd('channel_delete', { idx: ch.idx }, t('removed'))) go('#/channels'); } } }, I('trash'), t('delete')) : null)),
      h('div', { class: 'card', style: { textAlign: 'center' } }, h('h3', { style: { justifyContent: 'center' } }, I('qr'), t('share_channel')), qrSvg(link), h('p', { class: 'hint' }, t('share_channel_hint')))))];
  },
};
function channelForm() {
  let mode = 'hashtag';
  const name = h('input', { type: 'text', maxlength: 31, placeholder: '#paris' });
  const key = h('input', { type: 'text', class: 'mono', maxlength: 32, placeholder: '32 hex' });
  const keyRow = h('div', { style: { display: 'none' } }, h('label', { class: 'f' }, t('secret_key')), h('div', { class: 'row' }, key,
    h('button', { class: 'btn', title: t('random_key'), onclick: () => { key.value = bytesToHex(crypto.getRandomValues(new Uint8Array(16))); } }, I('dice'), t('random_key'))));
  const choice = h('div', { class: 'choice' });
  const setMode = m => {
    mode = m;
    keyRow.style.display = m === 'private' ? '' : 'none';
    name.placeholder = m === 'hashtag' ? '#paris' : t('channel_name');
    choice.querySelectorAll('button').forEach(b => b.classList.toggle('on', b.dataset.m === m));
  };
  choice.append(
    h('button', { 'data-m': 'hashtag', class: 'on', onclick: () => setMode('hashtag') }, h('b', {}, I('hash'), t('hashtag_channel')), h('span', {}, t('hashtag_desc'))),
    h('button', { 'data-m': 'private', onclick: () => { setMode('private'); if (!key.value) key.value = bytesToHex(crypto.getRandomValues(new Uint8Array(16))); } }, h('b', {}, I('lock'), t('private_channel')), h('span', {}, t('private_desc'))));
  const save = async () => {
    const n = name.value.trim();
    if (!n) return toast(t('err.bad_name'), 'err');
    const args = mode === 'hashtag' ? { name: n, hashtag: true } : { name: n, secret: key.value.trim().toLowerCase() };
    if (mode === 'private' && !/^[0-9a-f]{32}$/.test(args.secret)) return toast(t('err.bad_secret'), 'err');
    if (await meshCmd('channel_set', args, t('channel_added'))) go('#/channels');
  };
  return [head(t('add_channel'), null, null, '#/channels'), h('div', { class: 'scroll' }, h('div', { class: 'cards' }, h('div', { class: 'card' },
    choice, h('label', { class: 'f' }, t('name')), name, keyRow,
    h('details', { class: 'help' }, h('summary', {}, t('channels_help_t')), h('p', {}, t('channels_help'))),
    h('div', { class: 'btns' }, h('button', { class: 'btn pri', onclick: save }, I('plus'), t('add')), h('button', { class: 'btn', onclick: importContactDialog }, I('link'), t('join_by_link'))))))];
}

// ------------------------------------------------------------------ map (Leaflet loaded on demand)
let mapObj = null;
function loadLeaflet() {
  if (window.L) return Promise.resolve();
  return new Promise((res, rej) => {
    document.head.append(h('link', { rel: 'stylesheet', href: 'vendor/leaflet/leaflet.css' }));
    const s = h('script', { src: 'vendor/leaflet/leaflet.js' });
    s.onload = res; s.onerror = rej;
    document.head.append(s);
  });
}
V.map = {
  solo() {
    const pts = S.contacts.filter(c => c.lat || c.lon);
    return h('div', { style: { display: 'flex', flexDirection: 'column', height: '100%' } },
      head(t('nav.map'), t('n_located', { n: pts.length })),
      h('div', { id: 'mapnote' }),
      h('div', { id: 'map' }));
  },
  async mounted() {
    const pts = S.contacts.filter(c => c.lat || c.lon), me = S.status.node;
    const note = $('#mapnote');
    try { await loadLeaflet(); } catch { note.append(h('div', { class: 'notice', style: { margin: '10px' } }, t('map_unavailable'))); $('#map').remove(); return; }
    if (!$('#map')) return;
    mapObj = L.map('map', { zoomControl: true });
    let tileErr = 0;
    const tl = L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', { maxZoom: 18, attribution: '© OpenStreetMap' }).addTo(mapObj);
    tl.on('tileerror', () => { if (++tileErr === 3) note.append(h('div', { class: 'notice', style: { margin: '10px' } }, I('wifi'), t('map_offline'))); });
    const color = { 1: '#2f6fed', 2: '#e0861b', 3: '#8b5cf6', 4: '#18a058' };
    const bounds = [];
    for (const c of pts) {
      const m = L.circleMarker([c.lat, c.lon], { radius: 8, color: '#fff', weight: 2, fillColor: color[c.type] || '#555', fillOpacity: .95 }).addTo(mapObj);
      m.bindPopup(`<b>${esc(c.name)}</b><br>${esc(t('type.' + c.type))} · ${esc(ago(c.lastAdvert))}<br><a href="#/contacts/${c.key}">${esc(t('details'))}</a>`);
      bounds.push([c.lat, c.lon]);
    }
    if (me && (me.lat || me.lon)) {
      L.circleMarker([me.lat, me.lon], { radius: 10, color: '#fff', weight: 3, fillColor: '#d64545', fillOpacity: 1 }).addTo(mapObj).bindPopup(`<b>${esc(me.name)}</b> (${esc(t('me'))})`);
      bounds.push([me.lat, me.lon]);
    }
    if (bounds.length) mapObj.fitBounds(bounds, { padding: [30, 30], maxZoom: 13 }); else mapObj.setView([46.6, 2.4], 5);
    setTimeout(() => mapObj?.invalidateSize(), 100);
  },
  unmount() { if (mapObj) { mapObj.remove(); mapObj = null; } },
  noAutoRefresh: true,
};

// ------------------------------------------------------------------ tools
const TOOLS = { trace: null, disc: null, stats: {} };
V.tools = {
  noAutoRefresh: true,
  solo() {
    const direct = S.contacts.filter(c => c.hops >= 0).sort((a, b) => a.name.localeCompare(b.name));
    const sel = (list, id) => h('select', { id }, list.map(c => h('option', { value: c.key }, `${c.name} (${c.hops === 0 ? t('route.direct') : t('route.hops', { n: c.hops })})`)));
    const all = [...S.contacts].sort((a, b) => a.name.localeCompare(b.name));
    return h('div', { class: 'cards' }, head(t('nav.tools')),
      h('div', { class: 'card' }, h('h3', {}, I('megaphone'), t('advert')), h('p', { class: 'hint' }, t('advert_hint')),
        h('div', { class: 'btns' }, h('button', { class: 'btn', onclick: () => meshCmd('advert', { flood: false }, t('advert_sent')) }, t('advert_zero')),
          h('button', { class: 'btn pri', onclick: () => meshCmd('advert', { flood: true }, t('advert_sent')) }, t('advert_flood')))),
      h('div', { class: 'card' }, h('h3', {}, I('route'), t('trace')), h('p', { class: 'hint' }, t('trace_hint')),
        direct.length ? h('div', { class: 'row' }, sel(direct, 'tracesel'), h('button', { class: 'btn pri', onclick: runTrace }, t('start'))) : h('div', { class: 'notice' }, t('trace_no_path')),
        h('div', { id: 'traceout' }, TOOLS.trace)),
      h('div', { class: 'card' }, h('h3', {}, I('search'), t('path_discovery')), h('p', { class: 'hint' }, t('path_discovery_hint')),
        h('div', { class: 'row' }, h('select', { id: 'discsel' }, all.map(c => h('option', { value: c.key }, c.name))), h('button', { class: 'btn pri', onclick: runDisc }, t('start'))),
        h('div', { id: 'discout' }, TOOLS.disc)),
      h('div', { class: 'card' }, h('h3', {}, I('stats'), t('radio_stats')),
        h('div', { class: 'btns' }, [[0, 'stats.core'], [1, 'stats.radio'], [2, 'stats.packets']].map(([ty, l]) => h('button', { class: 'btn', onclick: () => runStats(ty) }, t(l)))),
        h('div', { id: 'statsout' }, Object.values(TOOLS.stats))),
      h('div', { class: 'card' }, h('h3', {}, I('qr'), t('my_card')), h('p', { class: 'hint' }, t('my_card_hint')), h('button', { class: 'btn', onclick: () => exportContact(null) }, I('qr'), t('show_qr'))));
  },
};
function hopsView(items) {
  return h('div', { class: 'hops' }, items.map((x, i) => [i ? h('span', { html: icon('chevron') }) : null,
    h('div', { class: 'hop' }, h('b', {}, x.label), x.snr != null ? h('span', { class: snrClass(x.snr) }, `${x.snr.toFixed(1)} dB`) : null)]));
}
async function runTrace() {
  const key = $('#tracesel').value, c = contactByKey(key), out = $('#traceout');
  out.innerHTML = '';
  out.append(h('p', {}, h('span', { class: 'spinner' }), ' ', t('waiting_reply')));
  const j = await apiDo('trace', { key });
  if (!j.ok) { out.innerHTML = ''; return; }
  const ev = await waitEvent(d => d.t === 'trace' && d.tag === j.tag, 30000);
  out.innerHTML = '';
  if (!ev) { out.append(h('div', { class: 'notice err' }, t('trace_timeout'))); return; }
  const sz = 1 << (ev.flags & 3);
  const hashes = ev.hashes.match(new RegExp(`.{${sz * 2}}`, 'g')) || [];
  const items = [{ label: t('me') }];
  hashes.forEach((hx, i) => items.push({ label: hashName(hx), snr: ev.snrs[i] }));
  items.push({ label: t('me'), snr: ev.snrs[ev.snrs.length - 1] });
  TOOLS.trace = h('div', {}, h('p', { class: 'hint' }, `${c?.name}: ${t('route.hops', { n: hashes.length })}`), hopsView(items));
  out.append(TOOLS.trace);
}
async function runDisc() {
  const key = $('#discsel').value, out = $('#discout'), c = contactByKey(key);
  out.innerHTML = '';
  out.append(h('p', {}, h('span', { class: 'spinner' }), ' ', t('waiting_reply')));
  const r = await meshCmd('path_disc', { key });
  if (!r) { out.innerHTML = ''; return; }
  const ev = await waitEvent(d => d.t === 'pathdisc' && d.conv === c.conv, 40000);
  out.innerHTML = '';
  if (!ev) { out.append(h('div', { class: 'notice err' }, t('disc_timeout'))); return; }
  const split = (hx, len) => { const sz = (len >> 6) + 1; return hx.match(new RegExp(`.{${sz * 2}}`, 'g')) || []; };
  const o = split(ev.out, ev.outLen), i = split(ev.in, ev.inLen);
  TOOLS.disc = h('div', {},
    h('p', { class: 'hint' }, t('path_out')), hopsView([{ label: t('me') }, ...o.map(x => ({ label: hashName(x) })), { label: c.name }]),
    h('p', { class: 'hint' }, t('path_in')), hopsView([{ label: c.name }, ...i.map(x => ({ label: hashName(x) })), { label: t('me') }]));
  out.append(TOOLS.disc);
  loadContacts();
}
async function runStats(type) {
  const r = await meshCmd('stats', { type });
  const s = r?.stats;
  if (!s) return;
  const cell = (v, l) => h('div', {}, h('b', {}, String(v)), h('span', {}, l));
  let grid;
  if (s.type === 0) grid = [cell((s.battMv / 1000).toFixed(2) + ' V', t('battery')), cell(fmtDuration(s.uptime), t('uptime')), cell(s.queueLen, t('tx_queue')), cell('0x' + s.errFlags.toString(16), t('err_flags'))];
  else if (s.type === 1) grid = [cell(s.noiseFloor + ' dBm', t('noise_floor')), cell(s.lastRssi + ' dBm', 'RSSI'), cell(s.lastSnr.toFixed(1) + ' dB', 'SNR'), cell(s.txAir + ' s', t('tx_airtime')), cell(s.rxAir + ' s', t('rx_airtime'))];
  else grid = [cell(s.recv, t('pkts_recv')), cell(s.sent, t('pkts_sent')), cell(`${s.recvFlood} / ${s.recvDirect}`, t('recv_flood_direct')), cell(`${s.sentFlood} / ${s.sentDirect}`, t('sent_flood_direct')), cell(s.recvErrors, t('recv_errors'))];
  TOOLS.stats[s.type] = h('div', { style: { marginTop: '12px' } }, h('p', { class: 'hint' }, t(['stats.core', 'stats.radio', 'stats.packets'][s.type])), h('div', { class: 'stat' }, grid));
  const out = $('#statsout');
  if (out) { out.innerHTML = ''; out.append(...Object.values(TOOLS.stats)); }
}

// ------------------------------------------------------------------ node settings
// Community presets published by the MeshCore app API (api.meshcore.nz/api/v1/config)
const PRESETS = [
  ['EU/UK (Narrow)', 869.618, 62.5, 8, 8], ['EU/UK (Deprecated)', 869.525, 250, 11, 5], ['EU 433MHz (Long Range)', 433.650, 250, 11, 5],
  ['EU 433MHz (Narrow)', 433.650, 62.5, 8, 8], ['Czech Republic (Narrow)', 869.432, 62.5, 7, 5, 2], ['Hungary', 869.618, 62.5, 7, 5, 2],
  ['Netherlands', 869.618, 62.5, 7, 5], ['Netherlands (Limburg)', 869.618, 62.5, 8, 8, 2], ['Portugal 433', 433.375, 62.5, 9, 6],
  ['Portugal 868', 869.618, 62.5, 7, 6], ['Slovakia', 869.618, 62.5, 7, 5, 2], ['Switzerland', 869.618, 62.5, 8, 8],
  ['USA', 910.525, 62.5, 7, 5], ['USA - Southern California', 927.875, 62.5, 7, 5, 3], ['Canada', 910.525, 62.5, 7, 5, 3],
  ['Costa Rica', 910.525, 125, 11, 5], ['Brazil', 923.125, 62.5, 8, 8], ['Australia', 915.800, 250, 10, 5],
  ['Australia (Narrow)', 916.575, 62.5, 7, 7], ['Australia (Mid)', 915.075, 125, 9, 5], ['Australia: SA, WA', 923.125, 62.5, 8, 8],
  ['Australia: QLD', 923.125, 62.5, 8, 5], ['New Zealand (Narrow)', 917.375, 62.5, 7, 5, 2], ['New Zealand (Gisborne)', 917.375, 250, 11, 5, 1],
  ['Thailand', 921, 62.5, 7, 5], ['Vietnam (Narrow)', 920.250, 62.5, 8, 5], ['Vietnam (Deprecated)', 920.250, 250, 11, 5],
];
V.node = {
  noAutoRefresh: true,
  solo() {
    const n = S.status.node;
    if (!n || S.status.link?.state !== 'ready') return h('div', { class: 'cards' }, head(t('nav.node')), h('div', { class: 'card' }, h('div', { class: 'notice' }, I('info'), t('node_offline'))));
    const f = (id, val, attrs) => h('input', { id, value: val, ...attrs });
    const name = f('n_name', n.name, { type: 'text', maxlength: 31 });
    const lat = f('n_lat', n.lat || '', { type: 'number', step: 'any' }), lon = f('n_lon', n.lon || '', { type: 'number', step: 'any' });
    const tx = f('n_tx', n.txPower, { type: 'range', min: -9, max: n.maxTxPower || 22, style: { width: '100%' } });
    const txv = h('b', {}, n.txPower + ' dBm');
    tx.addEventListener('input', () => { txv.textContent = tx.value + ' dBm'; });
    const freq = f('n_f', n.freq, { type: 'number', step: '0.001' }), bw = h('select', { id: 'n_bw' }, [7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125, 250, 500].map(v => h('option', { value: v, selected: Math.abs(v - n.bw) < .05 }, v + ' kHz')));
    const sf = h('select', {}, [5, 6, 7, 8, 9, 10, 11, 12].map(v => h('option', { value: v, selected: v === n.sf }, 'SF' + v)));
    const cr = h('select', {}, [5, 6, 7, 8].map(v => h('option', { value: v, selected: v === n.cr }, '4/' + v)));
    const phs = h('select', {}, [1, 2, 3].map(v => h('option', { value: v, selected: v === (n.pathHashMode || 0) + 1 }, t('hash_bytes', { n: v }))));
    const preset = h('select', { onchange: e => { const p = PRESETS[e.target.value]; if (!p) return; freq.value = p[1]; bw.value = p[2]; sf.value = p[3]; cr.value = p[4]; if (p[5]) phs.value = p[5]; } },
      h('option', { value: '' }, t('preset_pick')), PRESETS.map((p, i) => h('option', { value: i }, `${p[0]} — ${p[1]} MHz / BW${p[2]} / SF${p[3]} / CR${p[4]}`)));
    const pin = f('n_pin', n.blePin || '', { type: 'number', min: 100000, max: 999999, placeholder: '123456' });
    const manual = h('input', { type: 'checkbox', checked: !(n.manualAdd & 1) });
    const loc = h('select', {}, [[0, 'loc.none'], [1, 'loc.share']].map(([v, l]) => h('option', { value: v, selected: v === n.advertLocPolicy }, t(l))));
    const telem = k => { const v = (n.telemetryModes >> k) & 3; return h('select', {}, [[0, 'telem.deny'], [1, 'telem.flags'], [2, 'telem.all']].map(([x, l]) => h('option', { value: x, selected: x === v }, t(l)))); };
    const tb = telem(0), tl = telem(2), te = telem(4);
    const acks = h('select', {}, [0, 1, 2].map(v => h('option', { value: v, selected: v === n.multiAcks }, v)));
    const saveOther = () => meshCmd('node_other', { manualAdd: manual.checked ? 0 : 1, telemetryModes: (+tb.value) | (+tl.value << 2) | (+te.value << 4), advertLocPolicy: +loc.value, multiAcks: +acks.value }, t('saved'));
    return h('div', { class: 'cards' }, head(t('nav.node'), `${n.manufacturer} · ${n.fwVersion}`),
      h('div', { class: 'card' }, h('h3', {}, I('user'), t('identity')),
        h('label', { class: 'f' }, t('node_name')), h('div', { class: 'row' }, name, h('button', { class: 'btn pri', onclick: () => meshCmd('node_name', { name: name.value }, t('saved')) }, t('save'))),
        h('dl', { class: 'kv', style: { marginTop: '12px' } }, h('dt', {}, t('public_key')), h('dd', { class: 'mono' }, n.key.slice(0, 24) + '… ', h('button', { class: 'btn sm', onclick: () => copy(n.key) }, I('copy'))),
          h('dt', {}, t('firmware')), h('dd', {}, `${n.fwVersion} (${n.buildDate}) · v${n.fwVer}`), h('dt', {}, t('capacity')), h('dd', {}, t('capacity_v', { c: n.maxContacts, ch: n.maxChannels })))),
      h('div', { class: 'card' }, h('h3', {}, I('location'), t('position')), h('p', { class: 'hint' }, t('position_hint')),
        h('div', { class: 'row' }, h('div', { class: 'grow' }, h('label', { class: 'f' }, t('latitude')), lat), h('div', { class: 'grow' }, h('label', { class: 'f' }, t('longitude')), lon)),
        h('div', { class: 'btns' }, h('button', { class: 'btn pri', onclick: () => meshCmd('node_pos', { lat: +lat.value, lon: +lon.value }, t('saved')) }, t('save')),
          h('button', { class: 'btn', onclick: () => {
            if (!navigator.geolocation || !window.isSecureContext) return toast(t('geo_insecure'), 'err');
            navigator.geolocation.getCurrentPosition(p => { lat.value = p.coords.latitude.toFixed(6); lon.value = p.coords.longitude.toFixed(6); }, () => toast(t('geo_denied'), 'err'));
          } }, I('location'), t('use_browser_pos'))),
        h('label', { class: 'f' }, t('advert_location')), loc),
      h('div', { class: 'card' }, h('h3', {}, I('antenna'), t('radio')),
        h('div', { class: 'notice' }, I('alert'), t('radio_warning')),
        h('label', { class: 'f' }, t('preset')), preset,
        h('div', { class: 'row' }, h('div', { class: 'grow' }, h('label', { class: 'f' }, t('frequency') + ' (MHz)'), freq), h('div', { class: 'grow' }, h('label', { class: 'f' }, t('bandwidth')), bw)),
        h('div', { class: 'row' }, h('div', { class: 'grow' }, h('label', { class: 'f' }, t('spreading')), sf), h('div', { class: 'grow' }, h('label', { class: 'f' }, t('coding_rate')), cr), h('div', { class: 'grow' }, h('label', { class: 'f' }, t('path_hash')), phs)),
        h('div', { class: 'btns' }, h('button', { class: 'btn pri', onclick: async () => { if (await confirmDlg(t('radio'), t('radio_confirm'), t('apply'))) meshCmd('node_radio', { freq: +freq.value, bw: +bw.value, sf: +sf.value, cr: +cr.value, pathHashSize: +phs.value }, t('saved')); } }, t('apply'))),
        h('label', { class: 'f' }, t('tx_power')), h('div', { class: 'row' }, tx, txv),
        h('div', { class: 'btns' }, h('button', { class: 'btn', onclick: () => meshCmd('node_tx', { dbm: +tx.value }, t('saved')) }, t('save')))),
      h('div', { class: 'card' }, h('h3', {}, I('settings'), t('behaviour')),
        h('label', { class: 'switch' }, manual, t('auto_add')),
        h('label', { class: 'f' }, t('multi_acks')), acks,
        h('label', { class: 'f' }, t('telem_base')), tb, h('label', { class: 'f' }, t('telem_loc')), tl, h('label', { class: 'f' }, t('telem_env')), te,
        h('div', { class: 'btns' }, h('button', { class: 'btn pri', onclick: saveOther }, t('save')))),
      h('div', { class: 'card' }, h('h3', {}, I('bt'), t('ble_pin')), h('p', { class: 'hint' }, t('ble_pin_hint')),
        h('div', { class: 'row' }, pin, h('button', { class: 'btn pri', onclick: () => { const v = +pin.value; if (v && (v < 100000 || v > 999999)) return toast(t('err.bad_pin'), 'err'); meshCmd('node_pin', { pin: v }, t('saved')); } }, t('save')))),
      h('div', { class: 'card' }, h('h3', {}, I('power'), t('maintenance')),
        h('div', { class: 'btns' }, h('button', { class: 'btn', onclick: () => apiDo('refresh').then(() => toast(t('refreshing'))) }, I('refresh'), t('resync')),
          h('button', { class: 'btn danger', onclick: async () => { if (await confirmDlg(t('reboot_module'), t('reboot_module_q'), t('reboot'), true)) apiDo('node_reboot'); } }, I('power'), t('reboot_module')))));
  },
};

// ------------------------------------------------------------------ hub settings
const TIMEZONES = [
  ['Europe/Paris', 'CET-1CEST,M3.5.0,M10.5.0/3'], ['Europe/London', 'GMT0BST,M3.5.0/1,M10.5.0'], ['Europe/Lisbon', 'WET0WEST,M3.5.0/1,M10.5.0'],
  ['Europe/Helsinki', 'EET-2EEST,M3.5.0/3,M10.5.0/4'], ['America/New_York', 'EST5EDT,M3.2.0,M11.1.0'], ['America/Chicago', 'CST6CDT,M3.2.0,M11.1.0'],
  ['America/Denver', 'MST7MDT,M3.2.0,M11.1.0'], ['America/Los_Angeles', 'PST8PDT,M3.2.0,M11.1.0'], ['America/Montreal', 'EST5EDT,M3.2.0,M11.1.0'],
  ['America/Sao_Paulo', '<-03>3'], ['Australia/Sydney', 'AEST-10AEDT,M10.1.0,M4.1.0/3'], ['Australia/Perth', 'AWST-8'],
  ['Pacific/Auckland', 'NZST-12NZDT,M9.5.0,M4.1.0/3'], ['Asia/Bangkok', '<+07>-7'], ['Indian/Reunion', '<+04>-4'], ['America/Martinique', 'AST4'], ['UTC', 'UTC0'],
];
// Same rule as the server: one DNS label (a-z 0-9 '-'), ".local" dropped. null = invalid.
function hubHostname(v) {
  v = v.trim().toLowerCase().replace(/\.local\.?$/, '') || 'meshcore-hub';
  return /^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$/.test(v) ? v : null;
}
// Live 4G modem state for the settings card
function cellStatus() {
  const c = S.status.cell || {}, n = (cls, ic, txt) => h('div', { class: 'notice ' + cls }, I(ic), h('div', {}, txt));
  if (c.supported === false || c.state === 'unsupported') return n('', 'info', t('cell_st.unsupported'));
  if (c.state === 'online') {
    const dbm = c.rssi >= 0 && c.rssi <= 31 ? ` · ${-113 + 2 * c.rssi} dBm` : '';
    return n('ok', 'check', t('cell_st.online', { o: c.operator || '4G' }) + dbm + (c.ip ? ' · ' + c.ip : ''));
  }
  if (c.state === 'error') return n('err', 'alert', t('cell_st.error', { e: t('cell_err.' + (c.error || 'unknown')) }));
  return n(c.state === 'off' || !c.state ? '' : 'info', 'info', t('cell_st.' + (c.state || 'off')));
}
// Live Telegram state for the settings card (the form itself is not re-rendered: it would wipe
// what is being typed).
function tgStatus(s) {
  const tg = S.status.telegram || {}, n = (cls, ic, txt) => h('div', { class: 'notice ' + cls }, I(ic), h('div', {}, txt));
  if (!tg.enabled) return n('', 'info', s?.apOnly ? t('tg_st_ap_only') : s?.tgToken ? t('tg_st_disabled') : t('tg_st_no_token'));
  if (tg.claiming) return n('info', 'telegram', t('tg_st_claiming', { u: tg.user || '…' }));
  if (!tg.online) return tg.error ? n('err', 'alert', t('tg_offline', { e: tg.error })) : n('info', 'refresh', t('tg_st_connecting'));
  if (!tg.chats) return n('err', 'alert', t('tg_st_no_chat', { u: tg.user }));
  return n('ok', 'check', t('tg_online', { u: tg.user }) + (tg.queued ? ' · ' + t('tg_st_queued', { n: tg.queued }) : ''));
}
V.settings = {
  noAutoRefresh: true,
  live() { $('#cellstatus')?.replaceChildren(cellStatus()); const el = $('#tgstatus'); if (el) el.replaceChildren(tgStatus(V.settings.data?.settings)); },
  data: null,
  // A conversation was linked: only refresh the chat list (re-rendering would wipe what is
  // being typed in the other cards). Chats the user typed meanwhile are kept.
  async claimed(d) {
    if (d) toast(t('wiz.tg_linked', { n: d.title || d.chat }), 'ok');
    let j;
    try { j = await apiGet('/api/settings'); } catch { return; }
    const s = V.settings.data?.settings, fresh = j.settings?.tgChats || [];
    if (!s) return;
    const add = fresh.filter(c => !(s.tgChats || []).includes(c)), el = $('#tgchats');
    s.tgChats = fresh;
    if (el && add.length) el.value = [el.value.trim(), ...add].filter(Boolean).join(', ');
    V.settings.live();
  },
  // Same reason: only the directory list is redrawn.
  async routeLinked() {
    let j;
    try { j = await apiGet('/api/settings'); } catch { return; }
    const s = V.settings.data?.settings;
    if (!s) return;
    s.tgRoutes = j.settings?.tgRoutes || [];
    V.settings.drawRoutes?.();
    $('#tgrtlink')?.replaceChildren();
  },
  async mounted() {
    try { V.settings.data = await apiGet('/api/settings'); } catch { return; }
    const el = $('#solo');
    if (el && S.view === 'settings') { el.innerHTML = ''; el.append(settingsPage(V.settings.data)); }
  },
  solo() { return V.settings.data ? settingsPage(V.settings.data) : h('div', { class: 'cards' }, head(t('nav.settings')), h('div', { class: 'card' }, h('div', { class: 'skel', style: { height: '120px' } }))); },
};
// "?" next to a setting: opens an explanation (a click, so it also works on a phone).
function helpBtn(key, title) {
  return h('button', { class: 'qhelp', type: 'button', title: t('help'), 'aria-label': t('help') + ' : ' + title, onclick: e => {
    e.preventDefault();  // inside the <label> of a switch: must not toggle it
    e.stopPropagation();
    dialog(title, h('p', { class: 'helptxt' }, t('help.' + key)), [{ label: t('close'), value: true, pri: true }]);
  } }, '?');
}
// Not a <label>: a click on its text would go to the button.
const lbl = (text, key) => h('div', { class: 'f' }, text, helpBtn(key, text));
function settingsPage(data) {
  const s = data.settings, st = S.status, lk = st.link || {};
  const save = async (patch, msg) => {
    let j;
    try { j = await apiDo('settings', { settings: patch }); } catch { return false; }  // network error: already toasted
    if (j.ok) { Object.assign(s, patch); toast(msg || t('saved'), 'ok'); if (j.reboot && await confirmDlg(t('reboot_needed'), t('reboot_needed_q'), t('reboot'))) apiDo('reboot'); }
    return j.ok;
  };
  // Switches and lists go back to the stored value when the Hub did not take the change.
  const sw = (key, label, hint, help) => h('label', { class: 'switch' }, h('input', { type: 'checkbox', checked: !!s[key], onchange: async e => { const el = e.target; if (!(await save({ [key]: el.checked }))) el.checked = !!s[key]; } }), h('div', {}, label, help ? helpBtn(help, label) : null, hint ? h('div', { class: 'hint' }, hint) : null));
  const pick = key => async e => { const el = e.target; if (!(await save({ [key]: el.value }))) el.value = s[key]; };
  // module
  const releaseSel = h('select', {}, [5, 15, 30, 60, 0].map(m => h('option', { value: m }, m ? t('minutes', { n: m }) : t('until_reconnect'))));
  const moduleCard = h('div', { class: 'card' }, h('h3', {}, I(s.link === 'tcp' ? 'wifi' : 'bt'), t('module_link')),
    h('dl', { class: 'kv' }, h('dt', {}, t('link_type')), h('dd', {}, s.link === 'ble' ? 'Bluetooth' : s.link === 'tcp' ? 'Wi-Fi TCP' : t('none')),
      h('dt', {}, t('module')), h('dd', {}, (s.link === 'ble' ? (s.bleName || s.bleAddr) : s.link === 'tcp' && s.tcpHost ? `${s.tcpHost}:${s.tcpPort}` : '') || '–'),
      h('dt', {}, t('state')), h('dd', {}, linkDot()[1]), lk.rssi ? [h('dt', {}, 'RSSI'), h('dd', {}, lk.rssi + ' dBm')] : null),
    h('div', { class: 'btns' },
      h('button', { class: 'btn pri', onclick: () => startWizard('link') }, I('link'), t('change_module')),
      h('button', { class: 'btn', onclick: () => apiDo('module_reconnect').then(() => toast(t('reconnecting'))) }, I('refresh'), t('reconnect')),
      s.link ? h('button', { class: 'btn danger', onclick: async () => { if (await confirmDlg(t('forget_module'), t('forget_module_q'), t('forget'), true)) { await apiDo('ble_forget'); V.settings.mounted(); } } }, I('unlink'), t('forget_module')) : null),
    lbl(t('release_module'), 'release'), h('p', { class: 'hint' }, t('release_hint')),
    h('div', { class: 'row' }, releaseSel, h('button', { class: 'btn', onclick: () => apiDo('module_release', { minutes: +releaseSel.value }).then(() => toast(t('released'))) }, t('release'))));
  // wifi
  // Pick a network the Hub sees instead of typing its name. Resolves to the name, or null.
  const pickWifi = current => dialog(t('wiz.networks'), close => {
    const list = h('div', { class: 'netlist' });
    let busy = false;
    const run = async () => {
      if (busy) return;
      busy = true;
      list.replaceChildren(h('div', { class: 'item' }, h('span', { class: 'spinner' }), t('wiz.scanning')));
      const nets = await wifiScan().catch(() => null);
      busy = false;
      list.replaceChildren(...(nets?.length ? nets.map(n => h('div', { class: 'item' + (n.ssid === current ? ' on' : ''), role: 'button', tabindex: 0,
        onclick: () => close(n.ssid), onkeydown: e => { if (e.key === 'Enter') close(n.ssid); } },
        wifiBars(n.rssi), h('div', { class: 'body' }, h('span', { class: 'name' }, n.ssid)), n.secure ? I('lock') : null))
        : [h('div', { class: 'item' }, t(nets ? 'wiz.no_networks' : 'wifi_scan_fail'))]));
    };
    setTimeout(run, 50);
    return [h('p', { class: 'hint' }, t('wifi_scan_hint')), list,
      h('div', { class: 'btns' }, h('button', { class: 'btn sm', onclick: run }, I('refresh'), t('wiz.rescan')))];
  }, [{ label: t('close'), value: null }]);
  // The password field holds the server's mask: kept as is, the old password would go with a
  // new network name, so it is cleared as soon as the network name changes.
  const wpass = h('input', { type: 'password', value: s.wifiPass, placeholder: '********' });
  const ssid = h('input', { type: 'text', value: s.wifiSsid, oninput: () => { if (ssid.value !== s.wifiSsid && wpass.value === s.wifiPass) wpass.value = ''; else if (ssid.value === s.wifiSsid && !wpass.value) wpass.value = s.wifiPass; } });
  const scanMain = h('button', { class: 'btn', style: { flex: 'none' }, onclick: async () => {
    const name = await pickWifi(ssid.value);
    if (!name || name === ssid.value) return;
    ssid.value = name;
    ssid.dispatchEvent(new Event('input'));
    wpass.focus();
  } }, I('search'), t('wifi_scan'));
  const host = h('input', { type: 'text', value: s.hostname }), hostHint = h('p', { class: 'hint' }, `http://${s.hostname}.local · ${st.wifi?.ip || ''}`);
  const saveWifi = async () => {
    const hn = hubHostname(host.value);
    if (hn === null) return toast(t('err.bad_hostname'), 'err');
    const newNet = ssid.value && ssid.value !== s.wifiSsid;
    if (newNet && !wpass.value && !(await confirmDlg(t('wifi_open_t'), t('wifi_open_q', { s: ssid.value }), t('save')))) return;
    host.value = hn;
    const nets = alts.filter(n => n.ssid.trim()).map(n => ({ ssid: n.ssid.trim(), pass: n.pass }));
    const names = [ssid.value, ...nets.map(n => n.ssid)];
    if (new Set(names).size < names.length) return toast(t('wifi_alt_dup'), 'err');
    if (await save({ wifiSsid: ssid.value, wifiPass: wpass.value, hostname: hn, wifiAlt: nets })) hostHint.textContent = `http://${hn}.local · ${st.wifi?.ip || ''}`;
  };
  // Other networks, by priority. A masked password goes with its network name: cleared if it changes.
  const alts = (s.wifiAlt || []).map(n => ({ ...n }));
  const altBox = h('div', {});
  const addAlt = h('button', { class: 'btn', onclick: () => { alts.push({ ssid: '', pass: '' }); drawAlts(); } }, I('plus'), t('wifi_alt_add'));
  const drawAlts = () => {
    altBox.replaceChildren(...alts.map((n, i) => {
      const p = h('input', { type: 'password', value: n.pass, placeholder: t('wifi_password'), maxlength: 63, autocomplete: 'off', oninput: e => { n.pass = e.target.value; } });
      const a = h('input', { type: 'text', value: n.ssid, placeholder: t('wifi_ssid'), maxlength: 32, oninput: e => { n.ssid = e.target.value; if (n.pass.includes('****')) { n.pass = ''; p.value = ''; } } });
      const find = h('button', { class: 'ibtn', title: t('wifi_scan'), 'aria-label': t('wifi_scan'), html: icon('search'), onclick: async () => {
        const name = await pickWifi(a.value);
        if (!name || name === a.value) return;
        a.value = name;
        a.dispatchEvent(new Event('input'));
        p.focus();
      } });
      return h('div', { class: 'row', style: { marginBottom: '8px' } }, a, find, p, h('button', { class: 'ibtn', title: t('delete'), 'aria-label': t('delete'), html: icon('trash'), onclick: () => { alts.splice(i, 1); drawAlts(); } }));
    }));
    addAlt.style.display = alts.length >= 3 ? 'none' : '';
  };
  drawAlts();
  const wifiCard = h('div', { class: 'card' }, h('h3', {}, I('wifi'), 'Wi-Fi'),
    lbl(t('wifi_ssid'), 'wifi'), h('div', { class: 'row' }, ssid, scanMain), h('label', { class: 'f' }, t('wifi_password')), wpass,
    lbl(t('wifi_alt'), 'wifi_alt'), h('p', { class: 'hint' }, t('wifi_alt_hint')), altBox, h('div', { class: 'btns' }, addAlt),
    lbl(t('hostname'), 'hostname'), host, hostHint,
    sw('apOnly', t('ap_only'), t('ap_only_hint'), 'ap_only'),
    h('div', { class: 'btns' }, h('button', { class: 'btn pri', onclick: saveWifi }, t('save'))));
  // 4G modem
  const apn = h('input', { type: 'text', value: s.cellApn || '', placeholder: t('cell_apn_ph') });
  const simPin = h('input', { type: 'password', value: s.cellSimPin || '', placeholder: '0000', inputmode: 'numeric', autocomplete: 'off' });
  const model = h('select', {}, [['a7670', 'SIMCom A7670 (Cat-1)'], ['sim7600', 'SIMCom SIM7600'], ['generic', t('cell_generic')]].map(([v, l]) => h('option', { value: v, selected: s.cellModel === v }, l)));
  const pinIn = (v, ph) => h('input', { type: 'number', min: -1, max: 48, value: v ?? -1, placeholder: ph, style: { maxWidth: '90px' } });
  const ctx = pinIn(s.cellTx, 'TX'), crx = pinIn(s.cellRx, 'RX'), cpwr = pinIn(s.cellPwr, 'PWR');
  // An emptied field would read as +'' = 0, i.e. GPIO0 (the BOOT button): empty means "no pin".
  const pinVal = el => el.validity.badInput ? NaN : el.value.trim() === '' ? -1 : Number(el.value);  // unreadable: refused
  const saveCell = () => {
    const [tx, rx, pwr] = [ctx, crx, cpwr].map(pinVal);
    if (![tx, rx, pwr].every(p => Number.isInteger(p) && p >= -1 && p <= 48)) return toast(t('cell_pin_bad'), 'err');
    save({ cellModel: model.value, cellApn: apn.value.trim(), cellSimPin: simPin.value.trim(), cellTx: tx, cellRx: rx, cellPwr: pwr });
  };
  const cellCard = h('div', { class: 'card' }, h('h3', {}, I('signal'), t('cell_title')),
    h('div', { id: 'cellstatus' }, cellStatus()),
    h('p', { class: 'hint' }, t('cell_intro')),
    sw('cellEnabled', t('cell_enable'), null, 'cell'),
    lbl(t('cell_mode'), 'cell_mode'),
    h('select', { onchange: async e => { const el = e.target; if (!(await save({ cellAlways: el.value === '1' }))) el.value = s.cellAlways ? '1' : '0'; } },
      h('option', { value: '0', selected: !s.cellAlways }, t('cell_backup')), h('option', { value: '1', selected: !!s.cellAlways }, t('cell_always'))),
    lbl(t('cell_model'), 'cell_model'), model,
    lbl('APN', 'cell_apn'), apn, h('p', { class: 'hint' }, t('cell_apn_hint')),
    lbl(t('cell_sim_pin'), 'cell_pin'), simPin,
    lbl(t('cell_pins'), 'cell_pins'), h('div', { class: 'row' }, ctx, crx, cpwr), h('p', { class: 'hint' }, t('cell_pins_hint')),
    h('div', { class: 'btns' }, h('button', { class: 'btn pri', onclick: saveCell }, t('save'))));
  // telegram
  const token = h('input', { type: 'password', value: s.tgToken, placeholder: '123456:ABC…' });
  const chats = h('input', { type: 'text', id: 'tgchats', value: (s.tgChats || []).join(', '), placeholder: '123456789, -100123…' });
  const tg = st.telegram || {};
  const saveTg = async () => {
    const list = chats.value.split(/[,;\s]+/).filter(Boolean), bad = list.find(c => !/^-?\d+$/.test(c));
    if (bad) return toast(t('err.bad_chat', { c: bad }), 'err');
    // An empty list lets the next person who sends /start take over the bot.
    if (!list.length && s.tgChats?.length && !(await confirmDlg(t('tg_chats_empty_t'), t('tg_chats_empty_q'), t('save'), true))) return;
    save({ tgToken: token.value, tgChats: list.map(Number) });
  };
  const tgCard = h('div', { class: 'card' }, h('h3', {}, I('telegram'), 'Telegram'),
    h('div', { id: 'tgstatus' }, tgStatus(s)),
    sw('tgEnabled', t('tg_enable'), null, 'tg'),
    lbl(t('tg_token'), 'tg_token'), h('div', { class: 'row' }, token, h('button', { class: 'btn', onclick: () => testToken(token.value) }, t('test'))),
    lbl(t('tg_chats'), 'tg_chats'), chats, h('p', { class: 'hint' }, t('tg_chats_hint')),
    h('div', { class: 'btns' }, h('button', { class: 'btn pri', onclick: saveTg }, t('save')),
      h('button', { class: 'btn', onclick: () => apiDo('tg_claim').then(() => toast(t('tg_claim_wait'))) }, t('tg_relink'))),
    lbl(t('tg_mode'), 'tg_mode'),
    h('select', { onchange: pick('tgMode') }, [['single', 'tg_mode_single'], ['topics', 'tg_mode_topics']].map(([v, l]) => h('option', { value: v, selected: s.tgMode === v }, t(l)))),
    sw('tgRelayDm', t('tg_relay_dm'), null, 'tg_relay'), sw('tgRelayChannels', t('tg_relay_ch'), null, 'tg_relay'), sw('tgRelayPublic', t('tg_relay_public'), null, 'tg_public'), sw('tgRelayRooms', t('tg_relay_rooms'), null, 'tg_relay'),
    sw('tgRadioInfo', t('tg_radio_info'), null, 'tg_radio'), sw('tgSkipOwn', t('tg_skip_own'), null, 'tg_skip'), sw('tgInsecure', t('tg_insecure'), t('tg_insecure_hint'), 'tg_insecure'),
    s.tgMutedConvs?.length ? h('div', {}, h('label', { class: 'f' }, t('tg_muted')), s.tgMutedConvs.map(c => h('span', { class: 'chip', style: { marginRight: '6px' }, onclick: () => save({ tgMutedConvs: s.tgMutedConvs.filter(x => x !== c) }).then(() => V.settings.mounted()) }, convName(c), ' ✕'))) : null);
  // private routing: "@name text" over the mesh goes to one chat of the directory
  const rtName = h('input', { type: 'text', maxlength: '20', placeholder: t('tg_rt_name_ph') });
  const rtLink = h('div', { id: 'tgrtlink' }), rtList = h('div', { class: 'row', style: { flexWrap: 'wrap', gap: '6px' } }), rtFrom = h('div', {});
  const invite = async group => {
    const j = await apiDo('tg_route_invite', { name: rtName.value.trim().replace(/^@+/, '') });
    if (!j.ok) return;
    const url = `https://t.me/${j.user}?${group ? 'startgroup' : 'start'}=${j.code}`;
    rtLink.replaceChildren(h('label', { class: 'f' }, t(group ? 'tg_rt_link_group' : 'tg_rt_link_person')),
      h('input', { type: 'text', readonly: 'readonly', value: url, onclick: e => e.target.select() }), h('p', { class: 'hint' }, t('tg_rt_link_hint')));
    rtName.value = '';
  };
  const drawRoutes = () => rtList.replaceChildren(...((s.tgRoutes || []).length ? s.tgRoutes.map(r => h('span', {
    class: 'chip', title: t('tg_rt_remove'), onclick: async () => {
      if (!(await confirmDlg(t('tg_rt_remove'), t('tg_rt_remove_q', { n: r.name }), t('delete'), true))) return;
      const j = await apiDo('tg_route_del', { name: r.name }).catch(() => null);
      if (j?.ok) { s.tgRoutes = j.settings.tgRoutes; drawRoutes(); }
    } }, '@' + r.name + (r.chat < 0 ? ' · ' + t('tg_rt_group') : ''), ' ×')) : [h('p', { class: 'hint' }, t('tg_rt_empty'))]));
  const drawFrom = () => {
    const on = s.tgRouteFrom || [], people = S.contacts.filter(c => c.type === 1 || on.includes(c.conv)).sort((a, b) => a.name.localeCompare(b.name));
    // Approved, but no longer in the module's contacts: stays listed so that it can be unticked.
    for (const conv of on) if (!S.contacts.some(c => c.conv === conv)) people.push({ conv, name: conv });
    rtFrom.replaceChildren(...(people.length ? people.map(c => h('label', { class: 'switch' }, h('input', { type: 'checkbox', checked: on.includes(c.conv), onchange: async e => {
      const j = await apiDo('tg_route_from', { conv: c.conv, on: e.target.checked }).catch(() => null);
      if (j?.ok) { s.tgRouteFrom = j.settings.tgRouteFrom; toast(t('saved'), 'ok'); }
      drawFrom();  // what the Hub really kept
    } }), h('div', {}, c.name))) : [h('p', { class: 'hint' }, t('tg_rt_no_contacts'))]));
  };
  V.settings.drawRoutes = drawRoutes;
  V.settings.drawFrom = drawFrom;
  drawRoutes();
  drawFrom();
  const rtCard = h('div', { class: 'card' }, h('h3', {}, I('lock'), t('tg_rt_title')),
    h('p', { class: 'hint' }, t('tg_rt_intro')),
    h('details', { class: 'guide' }, h('summary', {}, I('info'), t('tg_rt_guide')),
      ['before', 'field', 'tg', 'trouble'].map(k => [h('h4', {}, t('tg_rt_guide_' + k)), h('ol', {}, t('tg_rt_steps_' + k).split('\n').map(x => h('li', {}, x)))])),
    sw('tgRouteOn', t('tg_rt_enable'), null, 'rt_on'),
    lbl(t('tg_rt_dir'), 'rt_dir'), rtList,
    lbl(t('tg_rt_add'), 'rt_add'), rtName, h('p', { class: 'hint' }, t('tg_rt_name_hint')),
    h('div', { class: 'btns' }, h('button', { class: 'btn', onclick: () => invite(false) }, t('tg_rt_invite_person')),
      h('button', { class: 'btn', onclick: () => invite(true) }, t('tg_rt_invite_group'))),
    rtLink,
    lbl(t('tg_rt_from'), 'rt_from'), h('p', { class: 'hint' }, t('tg_rt_from_hint')), rtFrom,
    sw('tgRouteKeep', t('tg_rt_keep'), t('tg_rt_keep_hint'), 'rt_keep'));
  // security & appearance
  const oldPw = h('input', { type: 'password', autocomplete: 'current-password' }), newPw = h('input', { type: 'password', autocomplete: 'new-password' });
  const secCard = h('div', { class: 'card' }, h('h3', {}, I('lock'), t('security')),
    h('label', { class: 'f' }, t('old_password')), oldPw, h('label', { class: 'f' }, t('new_password')), newPw,
    h('div', { class: 'btns' }, h('button', { class: 'btn pri', onclick: async () => { const j = await apiDo('password', { old: oldPw.value, new: newPw.value }); if (!j.ok) return; toast(t('saved'), 'ok'); if (j.reboot && await confirmDlg(t('reboot_needed'), t('reboot_needed_q'), t('reboot'))) apiDo('reboot'); } }, t('change_password')),
      h('button', { class: 'btn', onclick: () => fetch('/api/logout', { method: 'POST' }).then(() => location.reload()) }, I('logout'), t('logout'))),
    h('div', { class: 'notice info' }, I('info'), t('https_note')),
    sw('arduinoOta', t('arduino_ota'), t('arduino_ota_hint'), 'arduino_ota'));
  const lang = h('select', { onchange: async e => { const el = e.target; if (!(await save({ lang: el.value }))) { el.value = LANG; return; } store.set('lang', el.value); location.reload(); } }, Object.entries(LANGS).map(([k, v]) => h('option', { value: k, selected: k === LANG }, v)));
  const theme = h('select', { onchange: async e => { const el = e.target, prev = store.get('theme', s.theme); if (!(await save({ theme: el.value }))) { el.value = prev; return; } store.set('theme', el.value); S.status.theme = el.value; applyTheme(); } }, ['auto', 'light', 'dark'].map(v => h('option', { value: v, selected: store.get('theme', s.theme) === v }, t('theme.' + v))));
  const accent = h('input', { type: 'color', value: s.accent, style: { width: '60px', height: '38px', padding: '2px' }, onchange: async e => { const el = e.target; if (!(await save({ accent: el.value }))) { el.value = s.accent; return; } S.status.accent = el.value; applyTheme(); } });
  const tz = h('select', { onchange: pick('tz') }, TIMEZONES.map(([n, v]) => h('option', { value: v, selected: v === s.tz }, n)), TIMEZONES.some(x => x[1] === s.tz) ? null : h('option', { value: s.tz, selected: true }, s.tz));
  const notifSupported = 'Notification' in window && window.isSecureContext;
  const looks = h('div', { class: 'card' }, h('h3', {}, I('moon'), t('appearance')),
    h('label', { class: 'f' }, t('language')), lang, h('label', { class: 'f' }, t('theme')), theme,
    h('label', { class: 'f' }, t('accent')), accent, h('label', { class: 'f' }, t('timezone')), tz,
    h('label', { class: 'f' }, t('notifications')),
    notifSupported ? h('button', { class: 'btn', onclick: () => Notification.requestPermission().then(p => toast(t('notif_' + p))) }, I('bell'), t('enable_notifications')) : h('p', { class: 'hint' }, t('notif_insecure')));
  // history, backup, OTA, logs
  const days = h('input', { type: 'number', min: 0, value: s.historyDays }), pct = h('input', { type: 'number', min: 30, max: 95, value: s.historyMaxPct });
  const fsPct = st.fsTotal ? Math.round(st.fsUsed * 100 / st.fsTotal) : 0;
  const histCard = h('div', { class: 'card' }, h('h3', {}, I('archive'), t('history')),
    h('p', { class: 'hint' }, t('storage_used', { u: Math.round(st.fsUsed / 1024), t: Math.round(st.fsTotal / 1024), p: fsPct })),
    h('div', { class: 'progress' }, h('div', { style: { width: fsPct + '%' } })),
    h('div', { class: 'row' }, h('div', { class: 'grow' }, lbl(t('retention_days'), 'retention'), days), h('div', { class: 'grow' }, lbl(t('max_storage'), 'max_storage'), pct)),
    h('div', { class: 'btns' }, h('button', { class: 'btn pri', onclick: () => save({ historyDays: +days.value, historyMaxPct: +pct.value }) }, t('save')),
      h('button', { class: 'btn danger', onclick: async () => { if (await confirmDlg(t('wipe_history'), t('wipe_history_q'), t('delete'), true)) { await apiDo('history_wipe'); S.convs = []; S.msgs = {}; toast(t('done'), 'ok'); } } }, I('trash'), t('wipe_history'))));
  const secrets = h('input', { type: 'checkbox' });
  const restoreIn = h('input', { type: 'file', accept: '.json,application/json', style: { display: 'none' }, onchange: e => upload('/api/restore', e.target.files[0], t('restore_done')) });
  const backupCard = h('div', { class: 'card' }, h('h3', {}, I('download'), t('backup')), h('p', { class: 'hint' }, t('backup_hint')),
    h('label', { class: 'switch' }, secrets, t('backup_secrets')),
    h('div', { class: 'btns' }, h('button', { class: 'btn pri', onclick: () => { location.href = '/api/backup' + (secrets.checked ? '?secrets=1' : ''); } }, I('download'), t('export')),
      h('button', { class: 'btn', onclick: () => restoreIn.click() }, I('upload'), t('import')), restoreIn));
  const fwIn = h('input', { type: 'file', accept: '.bin', style: { display: 'none' }, onchange: e => upload('/api/ota', e.target.files[0], t('ota_done')) });
  const packIn = h('input', { type: 'file', accept: '.pack', style: { display: 'none' }, onchange: e => upload('/api/webapp', e.target.files[0], t('webapp_done'), true) });
  const prog = h('div', { class: 'progress', id: 'otaprog', style: { display: 'none' } }, h('div', { style: { width: '0%' } }));
  const otaCard = h('div', { class: 'card' }, h('h3', {}, I('upload'), t('update')), h('p', { class: 'hint' }, t('update_hint', { v: st.version })),
    h('div', { class: 'btns' }, h('button', { class: 'btn pri', onclick: () => fwIn.click() }, t('upload_firmware')), h('button', { class: 'btn', onclick: () => packIn.click() }, t('upload_webapp')), fwIn, packIn), prog);
  const logbox = h('div', { class: 'term logs', id: 'logbox' });
  apiGet('/api/logs').then(j => { for (const l of j.lines) logbox.append(h('div', { class: l.split(' ')[1] }, l)); logbox.scrollTop = logbox.scrollHeight; }).catch(() => {});
  const sysCard = h('div', { class: 'card' }, h('h3', {}, I('terminal'), t('system')),
    h('dl', { class: 'kv' }, h('dt', {}, t('version')), h('dd', {}, st.version), h('dt', {}, t('uptime')), h('dd', {}, fmtDuration(st.uptime || 0)),
      h('dt', {}, t('memory')), h('dd', {}, `${Math.round(st.heap / 1024)} kB (min ${Math.round(st.minHeap / 1024)} kB)`), h('dt', {}, t('clock')), h('dd', {}, st.timeValid ? new Date(st.time * 1000).toLocaleString(LANG) : t('clock_unset'))),
    h('label', { class: 'f' }, t('live_log')), logbox,
    h('div', { class: 'btns' }, h('button', { class: 'btn', onclick: async () => { if (await confirmDlg(t('reboot_hub'), t('reboot_hub_q'), t('reboot'))) { await apiDo('reboot'); toast(t('rebooting')); setTimeout(() => location.reload(), 9000); } } }, I('power'), t('reboot_hub')),
      h('button', { class: 'btn', onclick: () => startWizard() }, I('refresh'), t('rerun_wizard')),
      h('button', { class: 'btn danger', onclick: async () => { if (await confirmDlg(t('factory_reset'), t('factory_reset_q'), t('reset'), true)) { await apiDo('factory_reset'); toast(t('rebooting')); } } }, I('trash'), t('factory_reset'))));
  return h('div', { class: 'cards' }, head(t('nav.settings')), moduleCard, wifiCard, cellCard, tgCard, rtCard, secCard, looks, histCard, backupCard, otaCard, sysCard);
}
function upload(url, file, okMsg) {
  if (!file) return;
  const prog = $('#otaprog');
  const fd = new FormData();
  fd.append('file', file, file.name);
  const x = new XMLHttpRequest();
  x.open('POST', url);
  x.upload.onprogress = e => { if (prog && e.lengthComputable) { prog.style.display = ''; prog.firstChild.style.width = (e.loaded * 100 / e.total) + '%'; } };
  x.onload = () => {
    let j = {};
    try { j = JSON.parse(x.responseText); } catch { /* ignore */ }
    if (x.status === 200 && j.ok) { toast(okMsg, 'ok'); if (url !== '/api/webapp') setTimeout(() => location.reload(), 12000); else setTimeout(() => location.reload(), 1500); }
    else toast(t('err.upload', { e: j.err || x.status }), 'err');
  };
  x.onerror = () => toast(t('err.network'), 'err');
  x.send(fd);
}
// Resolves on the "tg_test" WebSocket event, or by polling /api/status (telegram.test) for the
// browsers that get no WebSocket events (some captive portals).
async function testToken(token) {
  const seqOf = st => st?.telegram?.test?.seq || 0;
  const seq0 = seqOf(await apiGet('/api/status', 2).catch(() => S.status));
  // the field still shows the mask from /api/settings: test the saved token instead
  let j;
  try { j = await apiDo('tg_test', token.includes('****') ? { useSaved: true } : { token }); } catch { return null; }
  if (!j.ok) return null;
  let done = false;
  const poll = (async () => {
    for (const end = Date.now() + 20000; !done && Date.now() < end;) {
      await sleep(1500);
      const r = done ? null : (await apiGet('/api/status', 1).catch(() => null))?.telegram?.test;
      if (r && (r.seq || 0) !== seq0 && !r.pending) return { t: 'tg_test', ok: r.ok, name: r.name, user: r.user, err: r.err };
    }
    return null;
  })();
  const ev = await Promise.race([waitEvent(d => d.t === 'tg_test', 20000), poll]);
  done = true;
  if (!ev) return toast(t('err.timeout'), 'err'), null;
  toast(ev.ok ? t('tg_token_ok', { n: ev.name, u: ev.user }) : t('tg_err.' + ev.err) !== 'tg_err.' + ev.err ? t('tg_err.' + ev.err) : ev.err, ev.ok ? 'ok' : 'err');
  return ev;
}
