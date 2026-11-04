'use strict';
/* MeshCore Hub - first-time setup wizard (captive portal). Also reused to change the module. */

// f: values typed in the fields (kept across re-renders), cfg: saved settings (/api/settings),
// tgToken: last token checked OK, wifiTest: last Wi-Fi test {ssid, pass, r}
const W = { step: 0, mode: 'full', link: 'ble', devices: [], scanning: false, picked: null, pin: '123456', phase: null, linkErr: '', tgBot: null, tgChat: null, tgToken: null, nets: [], f: {}, cfg: null, wifiTest: null, doneFresh: false };
const STEPS = ['welcome', 'prepare', 'wifi', 'link', 'pair', 'telegram', 'security', 'done'];

function startWizard(mode) {
  W.mode = mode === 'link' ? 'link' : 'full';
  W.step = W.mode === 'link' ? STEPS.indexOf('link') : 0;
  W.link = S.status.link?.type || 'ble';
  if (W.link !== 'tcp') W.link = 'ble';
  W.f = {}; W.cfg = null; W.wifiTest = null; W.doneFresh = false; W.phase = null; W.linkErr = ''; W.finished = false;
  loadCfg();
  renderWiz();
}
// The steps are prefilled from the saved settings, not only from the live state (a reload of the
// captive portal or a relaunched wizard must not show defaults and then save them over the real values).
async function loadCfg(force) {
  if (W.cfg && !force) return W.cfg;
  const j = await apiGet('/api/settings', 2).catch(() => null);
  if (j?.settings) W.cfg = j;
  return W.cfg;
}
const cfgOf = k => W.cfg?.settings?.[k];
const refreshStatus = () => apiGet('/api/status', 1).then(s => { S.status = { ...S.status, ...s }; return s; }).catch(() => null);
// typed values survive a re-render (language switch, Previous then Next)
function keep(k, el) {
  el.addEventListener(el.type === 'checkbox' ? 'change' : 'input', () => { W.f[k] = el.type === 'checkbox' ? el.checked : el.value; });
  return el;
}
// host name for mDNS / DHCP: a-z 0-9 '-', 1..63 chars; "name.local" (as shown in the hint) is accepted
function cleanHost(v) {
  const s = v.trim().toLowerCase().replace(/\.local$/, '');
  if (!s) return 'meshcore-hub';
  return /^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$/.test(s) ? s : null;
}
function wizSteps() { return W.mode === 'link' ? ['link', 'pair'] : STEPS; }
function renderWiz() {
  const app = $('#app');
  app.innerHTML = '';
  const steps = wizSteps(), name = STEPS[W.step], pos = steps.indexOf(name);
  const body = WIZ[name]();
  app.append(h('div', { class: 'wiz' }, h('div', { class: 'wiz-box' },
    h('div', { class: 'wiz-top' }, h('div', { html: LOGO, style: { display: 'contents' } }), h('h1', {}, W.mode === 'link' ? t('wiz.change_module') : t('wiz.title')),
      h('select', { 'aria-label': t('language'), style: { width: 'auto' }, onchange: async e => { store.set('lang', e.target.value); await loadLang(e.target.value); apiDo('settings', { settings: { lang: e.target.value } }, true); renderWiz(); } },
        Object.entries(LANGS).map(([k, v]) => h('option', { value: k, selected: k === LANG }, v)))),
    h('div', { class: 'progress', role: 'progressbar', 'aria-valuemin': 1, 'aria-valuemax': steps.length, 'aria-valuenow': pos + 1 }, h('div', { style: { width: ((pos + 1) * 100 / steps.length) + '%' } })),
    h('div', { class: 'steps-lbl' }, t('wiz.step_of', { n: pos + 1, m: steps.length }), ' · ', t('wiz.s.' + name)),
    h('div', { class: 'card' }, body))));
}
function wizNav(opts = {}) {
  const steps = wizSteps(), pos = steps.indexOf(STEPS[W.step]);
  const prev = pos > 0 ? h('button', { class: 'btn', onclick: () => { W.step = STEPS.indexOf(steps[pos - 1]); renderWiz(); } }, I('back'), t('wiz.prev')) : (W.mode === 'link' ? h('button', { class: 'btn', onclick: () => { leaveLinkWizard(); } }, t('cancel')) : h('span'));
  const next = h('button', { class: opts.secondary ? 'btn' : 'btn pri', 'data-next': '', disabled: !!opts.disabled, onclick: async () => {
    if (opts.before && (await opts.before()) === false) return;
    if (pos + 1 >= steps.length) return;
    W.step = STEPS.indexOf(steps[pos + 1]); renderWiz();
  } }, opts.label || t('wiz.next'), I('chevron'));
  return h('div', { class: 'wiz-nav', style: { marginTop: '22px' } }, prev, h('div', { class: 'row' }, opts.skip ? h('button', { class: 'btn', onclick: async () => { if (typeof opts.skip === 'function' && (await opts.skip()) === false) return; W.step = STEPS.indexOf(steps[pos + 1]); renderWiz(); } }, t('wiz.skip')) : null, opts.hideNext ? null : next));
}
const saveCfg = async patch => {
  const j = await apiDo('settings', { settings: patch });
  if (j.ok && W.cfg) Object.assign(W.cfg.settings, patch);
  return j;
};

const DIAGRAM = `<svg class="diagram" viewBox="0 0 640 170" role="img" aria-label="module - hub - telegram / navigateur">
<defs><linearGradient id="g1" x1="0" x2="1"><stop offset="0" stop-color="var(--accent)"/><stop offset="1" stop-color="#8b5cf6"/></linearGradient></defs>
<g fill="none" stroke="var(--accent)" stroke-width="3"><path class="flow" d="M150 85h120"/><path class="flow" d="M370 70 470 40"/><path class="flow" d="M370 100l100 30"/></g>
<g font-family="system-ui" font-size="13" fill="var(--ink-2)" text-anchor="middle">
<rect x="40" y="45" width="110" height="80" rx="14" fill="var(--panel-2)" stroke="var(--line)" stroke-width="2"/>
<path d="M95 45V18" stroke="var(--ink-3)" stroke-width="4" stroke-linecap="round"/><circle cx="95" cy="16" r="5" fill="var(--ink-3)"/>
<rect x="60" y="60" width="70" height="30" rx="4" fill="var(--panel)"/><text x="95" y="112">Module LoRa</text>
<rect x="270" y="40" width="100" height="90" rx="16" fill="url(#g1)"/><text x="320" y="92" fill="#fff" font-weight="700" font-size="15">Hub</text><text x="320" y="110" fill="#fff" font-size="11">ESP32</text>
<rect x="470" y="12" width="130" height="54" rx="14" fill="#2aa1da"/><text x="535" y="44" fill="#fff" font-weight="600">Telegram</text>
<rect x="470" y="104" width="130" height="54" rx="14" fill="var(--panel-2)" stroke="var(--line)" stroke-width="2"/><text x="535" y="136">Navigateur</text>
<text x="210" y="74">BLE / TCP</text><text x="420" y="34">Internet</text><text x="420" y="140">Wi-Fi</text></g>
<g fill="var(--accent)"><circle r="5"><animateMotion dur="1.6s" repeatCount="indefinite" path="M150 85h120"/></circle><circle r="5"><animateMotion dur="1.6s" begin=".5s" repeatCount="indefinite" path="M370 70 470 40"/></circle><circle r="5"><animateMotion dur="1.6s" begin=".9s" repeatCount="indefinite" path="M370 100l100 30"/></circle></g></svg>`;

// generic board drawings (no product photos)
function board(screen, round, long) {
  return `<svg viewBox="0 0 120 70"><rect x="${long ? 8 : 22}" y="${round ? 14 : 18}" width="${long ? 104 : 76}" height="${round ? 44 : 40}" rx="${round ? 18 : 6}" fill="${round ? '#2f3a4a' : '#1d6b46'}"/>
<path d="M${long ? 100 : 86} ${round ? 14 : 18}V4" stroke="#999" stroke-width="3" stroke-linecap="round"/>
${screen ? `<rect x="${long ? 22 : 34}" y="${round ? 22 : 25}" width="${screen === 2 ? 52 : 40}" height="${round ? 26 : 22}" rx="3" fill="#cfe3ff"/>` : `<rect x="44" y="30" width="22" height="16" rx="2" fill="#0d0f12"/>`}
<circle cx="${long ? 96 : 80}" cy="${round ? 46 : 50}" r="3" fill="#e6c34a"/></svg>`;
}
const MODULES = [['RAK4631 WisBlock', board(0, 0, 1)], ['Heltec V3', board(1)], ['Heltec T114', board(2)], ['LilyGO T-Echo', board(2, 1)],
  ['LilyGO T-Beam', board(1, 0, 1)], ['LilyGO T-Deck', board(2, 1, 1)], ['Seeed XIAO / Wio', board(0)], ['Station G2 / …', board(1, 1)]];

const WIZ = {
  welcome() {
    return [h('h2', {}, t('wiz.welcome_t')), h('p', {}, t('wiz.welcome_p')), h('div', { html: DIAGRAM }),
      h('ul', {}, ['wiz.welcome_1', 'wiz.welcome_2', 'wiz.welcome_3'].map(k => h('li', {}, t(k)))),
      h('details', { class: 'help' }, h('summary', {}, t('wiz.help')), h('p', {}, t('wiz.welcome_help'))), wizNav({ label: t('wiz.start') })];
  },
  prepare() {
    return [h('h2', {}, t('wiz.prepare_t')), h('p', {}, t('wiz.prepare_p')),
      h('p', {}, h('a', { href: 'https://flasher.meshcore.co.uk', target: '_blank', rel: 'noopener' }, I('link'), ' flasher.meshcore.co.uk')),
      h('div', { class: 'gallery' }, MODULES.map(([n, svg]) => h('div', {}, h('div', { html: svg }), n))),
      h('div', { class: 'notice' }, I('alert'), h('div', {}, h('b', {}, t('wiz.tips')), h('ul', { style: { margin: '6px 0 0', paddingLeft: '18px' } }, ['wiz.tip_phone', 'wiz.tip_pin', 'wiz.tip_range'].map(k => h('li', {}, t(k)))))),
      h('details', { class: 'help' }, h('summary', {}, t('wiz.help')), h('ol', {}, ['wiz.flash_1', 'wiz.flash_2', 'wiz.flash_3', 'wiz.flash_4'].map(k => h('li', {}, t(k))))), wizNav()];
  },
  wifi() {
    const list = h('div', { class: 'netlist' }, h('div', { class: 'item' }, h('span', { class: 'spinner' }), t('wiz.scanning')));
    const ssid = keep('ssid', h('input', { type: 'text', placeholder: t('wifi_ssid') }));
    const pass = keep('pass', h('input', { type: 'password', placeholder: t('wifi_password'), autocomplete: 'off', value: W.f.pass || '' }));
    const host = keep('host', h('input', { type: 'text' }));
    const apOnly = keep('apOnly', h('input', { type: 'checkbox' }));
    // typed value first, then the saved settings, then the live state
    const fill = () => {
      ssid.value = W.f.ssid ?? (cfgOf('wifiSsid') || S.status.wifi?.ssid || '');
      host.value = W.f.host ?? (cfgOf('hostname') || S.status.hostname || 'meshcore-hub');
      apOnly.checked = W.f.apOnly ?? !!cfgOf('apOnly');
    };
    fill();
    if (!W.cfg) loadCfg().then(() => { if (W.cfg && ssid.isConnected) fill(); });
    const res = h('div');
    const bars = r => h('span', { class: 'bars', 'aria-label': r + ' dBm' }, [-85, -75, -65, -55].map((x, i) => h('i', { class: r >= x ? 'on' : '', style: { height: (4 + i * 3) + 'px' } })));
    const scan = async () => {
      await apiDo('wifi_scan', {}, true);
      for (let i = 0; i < 20; i++) {
        await sleep(800);
        const j = await apiGet('/api/wifi').catch(() => null);
        if (j?.done) { W.nets = j.networks.sort((a, b) => b.rssi - a.rssi); break; }
      }
      list.innerHTML = '';
      if (!W.nets.length) list.append(h('div', { class: 'item' }, t('wiz.no_networks')));
      for (const n of W.nets) list.append(h('div', { class: 'item' + (ssid.value === n.ssid ? ' on' : ''), onclick: e => { ssid.value = n.ssid; W.f.ssid = n.ssid; list.querySelectorAll('.item').forEach(x => x.classList.remove('on')); e.currentTarget.classList.add('on'); pass.focus(); } },
        bars(n.rssi), h('div', { class: 'body' }, h('span', { class: 'name' }, n.ssid)), n.secure ? I('lock') : null));
    };
    const test = async () => {
      const tried = { ssid: ssid.value, pass: pass.value };
      res.innerHTML = '';
      res.append(h('p', {}, h('span', { class: 'spinner' }), ' ', t('wiz.wifi_testing')));
      await apiDo('wifi_test', { ssid: ssid.value, pass: pass.value });
      for (let i = 0; i < 30; i++) {
        await sleep(1000);
        const j = await apiGet('/api/wifi').catch(() => null);
        const r = j?.net?.test;
        if (r && r !== 'pending') {
          W.wifiTest = { ...tried, r };
          res.innerHTML = '';
          res.append(h('div', { class: 'notice ' + (r === 'ok' ? 'ok' : 'err') }, r === 'ok' ? t('wiz.wifi_ok', { ip: j.net.ip }) : t('wiz.wifi_' + r)));
          return r === 'ok';
        }
      }
      W.wifiTest = { ...tried, r: 'timeout' };
      res.innerHTML = '';
      res.append(h('div', { class: 'notice err' }, t('wiz.wifi_timeout')));
      return false;
    };
    if (W.wifiTest && W.wifiTest.ssid === ssid.value && W.wifiTest.pass === pass.value)
      res.append(h('div', { class: 'notice ' + (W.wifiTest.r === 'ok' ? 'ok' : 'err') }, W.wifiTest.r === 'ok' ? t('wiz.wifi_ok', { ip: S.status.wifi?.ip || '' }) : t('wiz.wifi_' + W.wifiTest.r)));
    setTimeout(scan, 50);
    return [h('h2', {}, t('wiz.wifi_t')), h('p', {}, t('wiz.wifi_p')),
      h('div', { class: 'row' }, h('b', { class: 'grow' }, t('wiz.networks')), h('button', { class: 'btn sm', onclick: () => { list.innerHTML = ''; list.append(h('div', { class: 'item' }, h('span', { class: 'spinner' }), t('wiz.scanning'))); scan(); } }, I('refresh'), t('wiz.rescan'))),
      list, h('label', { class: 'f' }, t('wifi_ssid')), ssid, h('label', { class: 'f' }, t('wifi_password')), pass,
      h('div', { class: 'btns' }, h('button', { class: 'btn', onclick: test }, I('wifi'), t('wiz.wifi_test'))), res,
      h('label', { class: 'f' }, t('hostname')), host, h('p', { class: 'hint' }, t('wiz.hostname_hint')),
      h('label', { class: 'switch' }, apOnly, h('div', {}, t('ap_only'), h('div', { class: 'hint' }, t('ap_only_hint')))),
      h('details', { class: 'help' }, h('summary', {}, t('wiz.help')), h('p', {}, t('wiz.wifi_help'))),
      wizNav({ before: async () => {
        if (!apOnly.checked && !ssid.value) { toast(t('wiz.wifi_need'), 'err'); return false; }
        const hn = cleanHost(host.value);
        if (!hn) { toast(t('err.bad_hostname'), 'err'); host.focus(); return false; }
        if (!apOnly.checked) {
          // a network saved without a successful test leaves the Hub unreachable for minutes after
          // the reboot (the fallback access point only opens after 3 min): test it, then ask
          const wt = W.wifiTest, live = S.status.wifi;
          const ok = wt && wt.ssid === ssid.value && wt.pass === pass.value ? wt.r === 'ok'
            : !pass.value && live?.connected && live.ssid === ssid.value && W.cfg && cfgOf('wifiSsid') === ssid.value ? true : await test();
          if (!ok && !(await confirmDlg(t('wiz.wifi_unverified_t'), t('wiz.wifi_unverified_q', { s: ssid.value }), t('wiz.wifi_save_anyway')))) return false;
        }
        // empty password field: keep the saved one only for the same network (an open network must
        // not inherit the previous network's password)
        const same = !W.cfg || ssid.value === cfgOf('wifiSsid');
        const j = await saveCfg({ wifiSsid: apOnly.checked ? '' : ssid.value, wifiPass: pass.value || (apOnly.checked || same ? undefined : ''), apOnly: apOnly.checked, hostname: hn });
        if (j.ok) { host.value = hn; W.f.host = hn; }
        return j.ok;
      } })];
  },
  link() {
    const choice = h('div', { class: 'choice' },
      h('button', { class: W.link === 'ble' ? 'on' : '', onclick: () => { W.link = 'ble'; renderWiz(); } }, h('b', {}, I('bt'), t('wiz.link_ble')), h('span', {}, t('wiz.link_ble_d'))),
      h('button', { class: W.link === 'tcp' ? 'on' : '', onclick: () => { W.link = 'tcp'; renderWiz(); } }, h('b', {}, I('wifi'), t('wiz.link_tcp')), h('span', {}, t('wiz.link_tcp_d'))));
    return [h('h2', {}, t('wiz.link_t')), h('p', {}, t('wiz.link_p')), choice, wizNav()];
  },
  pair() { return W.link === 'tcp' ? tcpStep() : bleStep(); },
  telegram() {
    const token = keep('token', h('input', { type: 'password', placeholder: '123456789:AA…', autocomplete: 'off', value: W.f.token || '' }));
    const res = h('div');
    const claim = h('div');
    let mode = W.f.tgMode || cfgOf('tgMode') || 'single';
    const modes = h('div', { class: 'choice' });
    const setMode = m => { mode = m; modes.querySelectorAll('button').forEach(b => b.classList.toggle('on', b.dataset.m === m)); };
    const pick = m => { W.f.tgMode = m; setMode(m); };
    modes.append(
      h('button', { 'data-m': 'topics', class: mode === 'topics' ? 'on' : '', onclick: () => pick('topics') }, h('b', {}, I('users'), t('tg_mode_topics')), h('span', {}, t('wiz.tg_topics_d'))),
      h('button', { 'data-m': 'single', class: mode === 'topics' ? '' : 'on', onclick: () => pick('single') }, h('b', {}, I('chat'), t('tg_mode_single')), h('span', {}, t('wiz.tg_single_d'))));
    if (!W.cfg) loadCfg().then(() => { if (!W.f.tgMode && cfgOf('tgMode')) setMode(cfgOf('tgMode')); });
    // without the saved settings, only an explicit choice is saved (never the default over "topics")
    const saveMode = () => (W.f.tgMode || W.cfg) && mode !== cfgOf('tgMode') ? saveCfg({ tgMode: mode }) : null;
    const okNote = ev => h('div', { class: 'notice ok' }, I('check'), t('tg_token_ok', { n: ev.name, u: ev.user }));
    const showClaim = ev => {
      claim.innerHTML = '';
      if (W.tgChat) return claim.append(linkedNote(W.tgChat));
      claim.append(h('div', { class: 'notice info' }, h('div', {}, h('b', {}, t('wiz.tg_send_start')), h('p', { style: { margin: '6px 0' } }, t('wiz.tg_send_start_p')),
        h('a', { class: 'btn', href: `https://t.me/${ev.user}?start=hub`, target: '_blank', rel: 'noopener' }, I('telegram'), '@' + ev.user),
        h('p', { class: 'hint', id: 'claimwait' }, h('span', { class: 'spinner' }), ' ', t('wiz.tg_waiting')))));
      setTimeout(waitClaim, 0);
    };
    const test = async () => {
      const tok = token.value.trim();
      res.innerHTML = '';
      res.append(h('p', {}, h('span', { class: 'spinner' }), ' ', t('wiz.tg_checking')));
      const ev = await testToken(tok);
      res.innerHTML = '';
      if (!ev?.ok) { res.append(h('div', { class: 'notice err' }, ev ? (t('tg_err.' + ev.err) !== 'tg_err.' + ev.err ? t('tg_err.' + ev.err) : ev.err) : t('err.timeout'))); return; }
      // Same bot as the one already linked (checked again, or a regenerated token): keep its
      // conversation instead of unlinking it and waiting for a new /start.
      const tg = (await refreshStatus())?.telegram || S.status.telegram || {};
      const kept = (tok === W.tgToken && W.tgChat) || (ev.user && ev.user === tg.user && tg.chats > 0);
      W.tgBot = ev; W.tgToken = tok;
      res.append(okNote(ev));
      if (kept) {
        await saveCfg({ tgToken: tok, tgEnabled: true });
        W.tgChat = W.tgChat || { kept: true };
      } else {
        W.tgChat = null;
        await saveCfg({ tgToken: tok, tgEnabled: true, tgChats: [] });
        await apiDo('tg_claim', { reset: true });
      }
      showClaim(ev);
    };
    // back on this step (Previous, language switch): show what was already done
    if (W.tgBot) { res.append(okNote(W.tgBot)); showClaim(W.tgBot); }
    return [h('h2', {}, t('wiz.tg_t')), h('p', {}, t('wiz.tg_p')),
      S.status.wifi?.connected ? null : h('div', { class: 'notice' }, I('alert'), t('wiz.tg_no_internet')),
      h('ol', { class: 'tg-steps' }, ['wiz.tg_1', 'wiz.tg_2', 'wiz.tg_3', 'wiz.tg_4'].map(k => h('li', { html: t(k) }))),
      h('label', { class: 'f' }, t('tg_token')), h('div', { class: 'row' }, token, h('button', { class: 'btn pri', onclick: test }, t('wiz.tg_verify'))), res, claim,
      h('label', { class: 'f' }, t('tg_mode')), modes,
      h('details', { class: 'help' }, h('summary', {}, t('wiz.tg_topics_help_t')), h('ol', {}, ['wiz.tg_topics_1', 'wiz.tg_topics_2', 'wiz.tg_topics_3'].map(k => h('li', {}, t(k))))),
      // A token typed but never checked used to be dropped silently: "Next" checks and saves it
      // (also a new token typed after another one was checked), "Skip" asks first.
      wizNav({ skip: async () => {
        const tok = token.value.trim();
        if (tok && tok !== W.tgToken && !(await confirmDlg(t('wiz.tg_skip_t'), t('wiz.tg_skip_q'), t('wiz.skip')))) return false;
        await saveMode();
      }, before: async () => {
        const tok = token.value.trim();
        if (tok && tok !== W.tgToken) { await test(); if (W.tgToken !== tok) return false; }
        await saveMode();
        const tg = (await refreshStatus())?.telegram || S.status.telegram || {};
        const user = W.tgBot?.user || (tg.enabled ? tg.user : '');
        if (user && !W.tgChat && !(tg.chats > 0) && !(await confirmDlg(t('wiz.tg_no_chat_t'), t('wiz.tg_no_chat_q', { u: user }), t('wiz.next')))) return false;
        return true;
      } })];
  },
  security() {
    const p1 = keep('p1', h('input', { type: 'password', autocomplete: 'new-password', value: W.f.p1 || '' })), p2 = keep('p2', h('input', { type: 'password', autocomplete: 'new-password', value: W.f.p2 || '' }));
    return [h('h2', {}, t('wiz.sec_t')), h('p', {}, t('wiz.sec_p')),
      h('label', { class: 'f' }, t('new_password')), p1, h('label', { class: 'f' }, t('wiz.confirm_password')), p2,
      h('div', { class: 'notice info' }, I('info'), t('https_note')),
      wizNav({ before: async () => {
        if (p1.value.length < 6) { toast(t('wiz.pw_short'), 'err'); return false; }
        if (p1.value !== p2.value) { toast(t('wiz.pw_mismatch'), 'err'); return false; }
        return (await saveCfg({ password: p1.value })).ok;
      } })];
  },
  done() {
    // describe what the Hub will do after the reboot: saved settings, refreshed once on arrival
    if (!W.doneFresh) { W.doneFresh = true; Promise.all([refreshStatus(), loadCfg(true)]).then(() => { if (STEPS[W.step] === 'done' && $('.wiz') && !W.finished) renderWiz(); }); }
    const st = S.status, s = W.cfg?.settings, w = st.wifi || {};
    const host = s?.hostname || st.hostname || 'meshcore-hub', url = `http://${host}.local`;
    const apMode = s ? !!s.apOnly || !s.wifiSsid : !w.connected, saved = s ? s.wifiSsid : w.ssid;
    const apSsid = W.cfg?.net?.apSsid || w.apSsid || 'MeshCore-Hub';
    const wifiOk = !apMode && w.connected && w.ssid === saved;
    const ip = apMode ? (w.apIp && w.apIp !== '0.0.0.0' ? w.apIp : '192.168.7.1') : wifiOk && w.ip && w.ip !== '0.0.0.0' ? w.ip : null;
    const wt = W.wifiTest?.ssid === saved && W.wifiTest.r !== 'ok' ? W.wifiTest.r : null;
    const l = st.link || {}, tg = st.telegram || {};
    const tgOn = s ? !!(s.tgEnabled && s.tgToken) : !!(tg.enabled || W.tgBot), bot = tg.user || W.tgBot?.user;
    const tgLinked = tg.chats !== undefined ? tg.chats > 0 : !!W.tgChat;
    return [h('h2', {}, t('wiz.done_t')), h('p', {}, t('wiz.done_p')),
      h('dl', { class: 'kv' },
        h('dt', {}, 'Wi-Fi'), h('dd', {}, apMode ? `${t('ap_only')} (${apSsid})` : saved + (wifiOk ? ' ✅' : ' · ' + (wt ? t('wiz.wifi_' + wt) : t('wiz.wifi_unverified')))),
        h('dt', {}, t('module')), h('dd', {}, l.state === 'ready' ? `${st.node?.name} ✅` : t('wiz.not_connected')),
        h('dt', {}, 'Telegram'), h('dd', {}, tgOn ? (bot ? '@' + bot : t('wiz.tg_configured')) + (tgLinked ? ' ✅' : ' · ' + t('wiz.tg_no_chat_t')) : t('wiz.not_configured')),
        h('dt', {}, t('wiz.address')), h('dd', {}, h('a', { href: url }, url), ip ? ` · http://${ip}` : '')),
      h('div', { style: { textAlign: 'center', margin: '18px 0' } }, qrSvg(ip ? `http://${ip}` : url), h('p', { class: 'hint' }, t('wiz.qr_hint'))),
      h('div', { class: 'notice info' }, I('info'), apMode ? t('wiz.done_reconnect_ap', { s: apSsid }) : t('wiz.done_reconnect')),
      h('div', { class: 'wiz-nav', style: { marginTop: '18px' } }, h('button', { class: 'btn', onclick: () => { W.step--; W.doneFresh = false; renderWiz(); } }, I('back'), t('wiz.prev')),
        h('button', { class: 'btn pri', onclick: async () => {
          W.finished = true;  // the Hub reboots: a late refresh must not bring this screen back
          const j = await apiDo('setup_finish');
          if (!j.ok) { W.finished = false; return; }
          $('#app').innerHTML = '';
          $('#app').append(h('div', { class: 'wiz' }, h('div', { class: 'wiz-box' }, h('div', { class: 'card', style: { textAlign: 'center' } }, h('h2', {}, t('wiz.rebooting')), h('p', {}, apMode ? t('wiz.rebooting_ap_p', { s: apSsid, u: ip ? `http://${ip}` : url }) : t('wiz.rebooting_p', { u: url })), h('p', {}, h('a', { href: url }, url), ip ? [' · ', h('a', { href: `http://${ip}` }, `http://${ip}`)] : '')))));
          setTimeout(() => { location.href = ip ? `http://${ip}` : url; }, 15000);
        } }, I('check'), t('wiz.finish')))];
  },
};

// ------------------------------------------------------------------ BLE pairing
const PHASES = ['connecting', 'securing', 'discovering', 'handshake', 'ready'];
function bleStep() {
  const list = h('div', { class: 'netlist', id: 'blelist' });
  const pin = h('input', { type: 'text', inputmode: 'numeric', maxlength: 6, value: W.pin, style: { maxWidth: '160px' } });
  pin.addEventListener('input', () => { W.pin = pin.value.replace(/\D/g, ''); });
  const fillList = () => {
    list.innerHTML = '';
    if (W.scanning) { list.append(h('div', { class: 'item' }, h('span', { class: 'spinner' }), t('wiz.ble_scanning'))); return; }
    const mc = W.devices.filter(d => d.mc !== false), others = W.devices.filter(d => d.mc === false);
    const row = d => h('div', { class: 'item' + (W.picked?.addr === d.addr ? ' on' : ''), style: d.mc === false ? { opacity: .75 } : null, onclick: () => { W.picked = d; fillList(); } },
      h('span', { html: icon('bt') }), h('div', { class: 'body' }, h('div', { class: 'name' }, d.name || t('wiz.unnamed')), h('div', { class: 'prev mono' }, d.addr)), h('span', { class: 'time' }, d.rssi + ' dBm'));
    if (!mc.length) {
      const why = W.heard < 0 ? t('wiz.ble_failed') : W.heard === 0 ? t('wiz.ble_deaf') : t('wiz.ble_none_heard', { n: W.heard });
      list.append(h('div', { class: 'item', style: { display: 'block' } }, h('b', {}, t('wiz.ble_none')), h('div', { class: 'hint' }, why)));
    }
    if (!W.picked && mc.length === 1) W.picked = mc[0];
    mc.forEach(d => list.append(row(d)));
    if (others.length) {
      list.append(h('div', { class: 'item', style: { cursor: 'default', fontSize: '12.5px', color: 'var(--ink-3)' } }, t('wiz.ble_others')));
      others.forEach(d => list.append(row(d)));
    }
  };
  const scan = async () => {
    W.scanning = true; W.devices = []; fillList();
    await apiDo('ble_scan');
    // fallback when web socket events don't arrive (some captive-portal browsers)
    for (let i = 0; i < 20 && W.scanning; i++) {
      await sleep(1500);
      const j = await apiGet('/api/ble').catch(() => null);
      if (j && !j.scanning && j.age >= 0 && j.age < 25) { W.scanning = false; W.devices = j.devices; W.heard = j.heard; fillList(); }
    }
  };
  W.refreshBle = fillList;
  const prog = h('div', { id: 'pairprog' });
  const connect = async () => {
    if (!W.picked) return toast(t('wiz.ble_pick'), 'err');
    if (!/^\d{6}$/.test(W.pin)) return toast(t('err.bad_pin'), 'err');
    W.phase = 'connecting'; W.linkErr = '';
    renderPhases();
    await apiDo('ble_pair', { addr: W.picked.addr, type: W.picked.type, name: W.picked.name, pin: +W.pin });
    pollLink();
  };
  if (!W.devices.length && !W.scanning) setTimeout(scan, 50); else setTimeout(fillList, 0);
  setTimeout(renderPhases, 0);
  return [h('h2', {}, t('wiz.ble_t')), h('p', {}, t('wiz.ble_p')),
    h('div', { class: 'row' }, h('b', { class: 'grow' }, t('wiz.ble_found')), h('button', { class: 'btn sm', onclick: scan }, I('search'), t('wiz.ble_search'))), list,
    h('label', { class: 'f' }, t('wiz.ble_pin')), pin, h('p', { class: 'hint' }, t('wiz.ble_pin_hint')),
    h('div', { class: 'btns' }, h('button', { class: 'btn pri', onclick: connect }, I('link'), t('wiz.ble_connect'))), prog,
    h('details', { class: 'help' }, h('summary', {}, t('wiz.help')), h('ul', {}, ['wiz.ble_help_1', 'wiz.ble_help_2', 'wiz.ble_help_3', 'wiz.ble_help_4'].map(k => h('li', {}, t(k))))),
    linkNav()];
}
function tcpStep() {
  const [aHost, aPort] = S.status.link?.type === 'tcp' ? (S.status.link.addr || '').split(':') : [];
  const host = keep('tcpHost', h('input', { type: 'text', placeholder: '192.168.1.50', value: W.f.tcpHost ?? (aHost || '') }));
  const port = keep('tcpPort', h('input', { type: 'number', value: W.f.tcpPort ?? (+aPort || 5000), style: { maxWidth: '120px' } }));
  const prog = h('div', { id: 'pairprog' });
  setTimeout(renderPhases, 0);
  return [h('h2', {}, t('wiz.tcp_t')), h('p', {}, t('wiz.tcp_p')),
    h('div', { class: 'row' }, h('div', { class: 'grow' }, h('label', { class: 'f' }, t('wiz.tcp_host')), host), h('div', {}, h('label', { class: 'f' }, t('wiz.tcp_port')), port)),
    h('p', { class: 'hint' }, t('wiz.tcp_sim')),
    h('div', { class: 'btns' }, h('button', { class: 'btn pri', onclick: async () => { W.phase = 'connecting'; W.linkErr = ''; renderPhases(); await apiDo('tcp_set', { host: host.value.trim(), port: +port.value }); pollLink(); } }, I('link'), t('wiz.ble_connect'))), prog,
    linkNav()];
}
// Leaving the module step without a connected module is allowed (set it up later in the
// settings) but must be a choice: the main button stays "Connect" until the module is ready.
function leaveLinkWizard() {
  if (S.status.link?.state !== 'ready') apiDo('link_restore', {}, true);  // no-op once the new module worked
  location.hash = '#/settings'; shell(); route();
}
function linkNav() {
  const ready = S.status.link?.state === 'ready';
  return wizNav({ label: ready ? t('wiz.next') : t('wiz.skip_module'), secondary: !ready, before: async () => {
    // read at click time: the pairing may have finished since the step was drawn
    const isReady = S.status.link?.state === 'ready';  // live state ('link' events), W.phase may be stale
    if (!isReady && W.mode !== 'link' && !(await confirmDlg(t('wiz.skip_module_t'), t('wiz.skip_module_q'), t('wiz.skip_module')))) return false;
    if (W.mode === 'link') { leaveLinkWizard(); return false; }
    return true;
  } });
}
function renderPhases() {
  const box = $('#pairprog');
  if (!box || !W.phase) return;
  box.innerHTML = '';
  const steps = W.link === 'tcp' ? ['connecting', 'handshake', 'ready'] : PHASES;
  const cur = steps.indexOf(W.phase);
  box.append(h('ul', { class: 'phases' }, steps.map((p, i) => h('li', { class: i < cur || W.phase === 'ready' ? 'done' : i === cur && !W.linkErr ? 'cur' : '' },
    h('span', { class: 'ph', html: i < cur || W.phase === 'ready' ? icon('check') : '' }), t('wiz.phase.' + p)))));
  if (W.linkErr) box.append(h('div', { class: 'notice err' }, I('alert'), h('div', {}, h('b', {}, t('linkerr.' + W.linkErr)), h('div', {}, t('wiz.err_help.' + W.linkErr) !== 'wiz.err_help.' + W.linkErr ? t('wiz.err_help.' + W.linkErr) : ''))));
  if (W.phase === 'ready') {
    const n = S.status.node || {}, b = S.status.battery || {};
    box.append(h('div', { class: 'notice ok' }, I('check'), h('div', {}, h('b', {}, t('wiz.paired', { n: n.name || '?' })),
      h('dl', { class: 'kv', style: { marginTop: '6px' } }, h('dt', {}, t('public_key')), h('dd', { class: 'mono' }, (n.key || '').slice(0, 16) + '…'),
        h('dt', {}, t('battery')), h('dd', {}, b.mv ? `${b.pct}% (${(b.mv / 1000).toFixed(2)} V)` : '–'),
        h('dt', {}, t('frequency')), h('dd', {}, n.freq ? `${n.freq} MHz · BW ${n.bw} · SF${n.sf}` : '–')))));
    const next = box.closest('.card')?.querySelector('.wiz-nav [data-next]');
    if (next) { next.className = 'btn pri'; next.replaceChildren(document.createTextNode(t('wiz.next')), I('chevron')); }
  }
}
// polling fallback for the pairing progress (same data as the 'link' events)
async function pollLink() {
  for (let i = 0; i < 60 && W.phase && W.phase !== 'ready' && $('#pairprog'); i++) {
    await sleep(2000);
    const st = await apiGet('/api/status').catch(() => null);
    if (st?.link) { S.status = { ...S.status, ...st }; wizOnLink(st.link); }
  }
}
// link events while the wizard is open
function wizOnLink(d) {
  if (!$('.wiz') || !W.phase) return;
  // the Hub retries by itself: an error from a previous attempt must not stay on screen while
  // the next one gets through (first bonding often needs a second connection)
  if (d.state === 'connecting' || d.state === 'handshake') W.linkErr = '';
  if (d.state === 'connecting') W.phase = ['connecting', 'securing', 'discovering'].includes(d.phase) ? d.phase : 'connecting';
  else if (d.state === 'handshake') W.phase = 'handshake';
  else if (d.state === 'ready') { W.phase = 'ready'; W.linkErr = ''; apiGet('/api/status').then(s => { S.status = { ...S.status, ...s }; renderPhases(); }); }
  else if (d.state === 'waiting' && d.error) W.linkErr = d.error;
  renderPhases();
}
function wizOnEvent(d) {
  if (d.t === 'ble_scan') {
    W.scanning = d.state === 'scanning';
    if (d.devices) W.devices = d.devices;
    if (d.heard !== undefined) W.heard = d.heard;
    W.refreshBle?.();
  } else if (d.t === 'tg_claimed') {
    W.tgChat = d;
    const el = $('#claimwait');
    if (el) el.replaceWith(linkedNote(d));
  }
}
function linkedNote(d) {
  return h('div', { class: 'notice ok' }, I('check'), d.title || d.chat ? t('wiz.tg_linked', { n: d.title || d.chat }) : t(d.kept ? 'wiz.tg_kept' : 'wiz.tg_linked_ok'));
}
// fallback when the 'tg_claimed' web socket event doesn't arrive (some captive-portal browsers):
// the claim resets the allowed chats, so the first one counted means /start was received
async function waitClaim() {
  const run = W.claimRun = (W.claimRun || 0) + 1;  // a re-render starts a new wait: only one polls
  for (let i = 0; i < 150 && !W.tgChat && $('#claimwait'); i++) {
    await sleep(2000);
    if (run !== W.claimRun || W.tgChat || !$('#claimwait')) return;
    const tg = (await refreshStatus())?.telegram;
    if (tg?.chats > 0 && !W.tgChat) wizOnEvent({ t: 'tg_claimed' });
  }
}
