/* OHpanel demo: the web interface (www/index.html) without a panel. Every call to /api/v1/... is answered here,
   in the browser, from invented data (demo/config.json, demo/items.json, demo/api.json); the display preview
   draws from them as from a panel. Changes stay in this browser tab (the configuration in sessionStorage).
   Loaded before the page's own script (tools/build_demo.py puts it there). */
(() => {
  'use strict';
  const realFetch = window.fetch.bind(window);
  const base = new URL('.', document.currentScript ? document.currentScript.src : location.href);
  const asset = name => new URL(name, base).href;
  const json = (body, status = 200) => new Response(JSON.stringify(body), {status, headers: {'Content-Type': 'application/json'}});
  const ok = () => json({ok: true});
  const notInDemo = () => json({error: 'Not in the demo: this needs a panel.'}, 400);
  const started = Date.now();
  // the panel variant shown: the NSPanel EU, or with ?variant=us the US, ?variant=us-landscape the US on its side
  // (its layout, what the panel says it is)
  const VARIANTS = {eu: 'EU', us: 'US', 'us-landscape': 'US landscape'};
  const asked = new URLSearchParams(location.search).get('variant'), variant = VARIANTS[asked] ? asked : 'eu';

  // the data, loaded once
  let data = null;
  const load = () => data || (data = Promise.all(['config.json', 'items.json', 'api.json', 'qr.json']
    .map(f => realFetch(asset(f)).then(r => r.json())))
    .then(([config, items, api, qr]) => {
      let saved = null;
      try { saved = JSON.parse(sessionStorage.getItem('ohpanel-demo-config') || 'null'); } catch (e) { saved = null; }
      return {config: saved || config, items, api, qr};
    }));

  /* An item's state as the panel's /openhab/states gives it: state, display, type, numeric, unit. */
  const pad = n => String(n).padStart(2, '0');
  function iso(d) {
    const off = -d.getTimezoneOffset(), s = off >= 0 ? '+' : '-';
    return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) + 'T00:00:00.000' + s +
      pad(Math.floor(Math.abs(off) / 60)) + pad(Math.abs(off) % 60);
  }
  function stateOf(it, live) {
    let st = live !== undefined ? live : it.state;
    if (typeof st === 'string' && st.startsWith('@days:')) {   // a date that many days from today
      const d = new Date(); d.setDate(d.getDate() + Number(st.slice(6)));
      return {state: iso(d), display: iso(d), type: 'DateTime'};
    }
    if (typeof st === 'string' && st.startsWith('@clock:+')) {   // a time that many minutes from now
      const d = new Date(Date.now() + Number(st.slice(8)) * 60000);
      const t = pad(d.getHours()) + ':' + pad(d.getMinutes());
      return {state: t, display: t, type: 'String'};
    }
    st = String(st);
    const t = it.itype.replace(/^Group:/, '');
    if (st === 'NULL' || st === 'UNDEF') return {state: st, display: st, type: 'UnDef'};
    if (t === 'Switch') return {state: st, display: st, type: 'OnOff'};
    if (t === 'Contact') return {state: st, display: st, type: 'OpenClosed'};
    if (t === 'Player') return {state: st, display: st, type: 'PlayPause'};
    if (t === 'Color') return {state: st, display: st, type: 'HSB'};
    if (t === 'Dimmer' || t === 'Rollershutter') {
      const n = Number(st);
      return {state: st, display: n + ' %', type: 'Percent', numeric: n};
    }
    if (t.startsWith('Number')) {
      const m = st.match(/^(-?[\d.]+)\s*(.*)$/);
      if (!m) return {state: st, display: st, type: 'String'};
      const n = Number(m[1]), unit = m[2];
      return unit ? {state: st, display: st, type: 'Quantity', unit, numeric: n} : {state: st, display: st, type: 'Decimal', numeric: n};
    }
    return {state: st, display: st, type: 'String'};
  }

  // what the demo changes by itself: the sun comes and goes, the house draws power, the washer moves on
  const live = {};
  function tick() {
    const t = (Date.now() - started) / 1000;
    const solar = Math.max(0, Math.round(2840 + 420 * Math.sin(t / 9) + 90 * Math.sin(t / 2.3)));
    const house = Math.round(960 + 180 * Math.sin(t / 5.5));
    live.Solar_Power = solar + ' W';
    live.House_Power = house + ' W';
    live.Grid_Power = (house - solar + 1500) + ' W';
    live.Washer_Progress = Math.min(99, 62 + Math.floor(t / 20)) + ' %';
  }
  setInterval(tick, 2000);
  tick();

  function statesMap(d) {
    const out = {};
    for (const [name, it] of Object.entries(d.items.items)) out[name] = stateOf(it, live[name]);
    return out;
  }
  function optionsOf(d, item) {
    const it = d.items.items[item];
    const r = {known: true, options: d.items.options[item] || [], members: [], metadata: {}};
    if (it && it.min !== undefined) { r.min = it.min; r.max = it.max; r.step = it.step; }
    const g = d.items.groups[item];
    if (g) {
      r.metadata = g.metadata || {};
      r.members = g.members.map(m => ({name: m, label: d.items.items[m].label, metadata: d.items.member_metadata[m] || {}}));
    }
    return r;
  }
  function itemsSearch(d, q, type, offset) {
    q = (q || '').toLowerCase();
    const types = (type || '').split(',').filter(Boolean);
    const all = Object.entries(d.items.items)
      .filter(([n, it]) => (!q || n.toLowerCase().includes(q) || it.label.toLowerCase().includes(q)) &&
                           (!types.length || types.some(t => it.itype.split(':')[0] === t || it.itype === t)))
      .map(([n, it]) => ({name: n, label: it.label, type: it.itype}));
    const from = Number(offset) || 0;
    return {matches: all.length, items: all.slice(from, from + 50)};
  }
  function info(d) {
    const i = JSON.parse(JSON.stringify(d.api.info));
    i.uptime_s = d.api.info.uptime_s + Math.floor((Date.now() - started) / 1000);
    i.time.epoch_ms = Date.now();
    i.variant = variant;
    i.board = 'nspanel-' + variant;
    return i;
  }

  async function route(method, path, query, body) {
    const d = await load();
    if (method === 'GET') {
      switch (path) {
        case 'layout': return realFetch(asset('layout-' + variant + '.json'));
        case 'assets': return realFetch(asset('icon_set.bin'));
        case 'panel': return json(d.config);
        case 'info': return json(info(d));
        case 'health': return json(Object.assign({}, d.api.health, {uptime_s: info(d).uptime_s}));
        case 'tls': return json(d.api.tls);
        case 'transfer': return json({transfer: {running: false, kind: '', done: 0, total: 0, elapsed_ms: 0}});
        case 'ota/status': return json(d.api.ota_status);
        case 'config/openhab': return json(d.api.config_openhab);
        case 'wifi/scan': return json(d.api.wifi_scan);
        case 'openhab/scan': return json(d.api.openhab_scan);
        case 'openhab/states': return json(statesMap(d));
        case 'openhab/options': return json(optionsOf(d, query.get('item')));
        case 'openhab/items': return json(itemsSearch(d, query.get('q'), query.get('type'), query.get('offset')));
        case 'log': return json(Number(query.get('since') || 0) >= d.api.log.next ? {next: d.api.log.next, text: ''} : d.api.log);
        case 'coredump': return json({error: 'no core dump'}, 404);
        case 'backup': return json(Object.assign({ohpanel_backup: 1, device: {name: d.api.info.device}}, {panel: d.config}));
        case 'tls/ca.crt': return notInDemo();
        default: return json({error: 'unknown in the demo: ' + path}, 404);
      }
    }
    if (path === 'panel' && method === 'PUT') {
      try { d.config = JSON.parse(body); } catch (e) { return json({error: 'bad JSON'}, 400); }
      try { sessionStorage.setItem('ohpanel-demo-config', JSON.stringify(d.config)); } catch (e) { /* kept in memory */ }
      return ok();
    }
    if (path === 'panel' && method === 'DELETE') {
      try { sessionStorage.removeItem('ohpanel-demo-config'); } catch (e) { /* nothing kept */ }
      data = null;
      return ok();
    }
    if (['restart', 'reset', 'ota', 'display/tft', 'tls/ca', 'backup'].includes(path)) return notInDemo();
    if (path === 'qr') {   // made when the demo was built (tools/build_demo.py): the codes of its own qr pages
      let q = {};
      try { q = JSON.parse(body); } catch (e) { q = {}; }
      return json(d.qr[(q.ecc || 'medium') + '|' + q.text] || {error: 'the demo draws only its own QR codes'});
    }
    if (path === 'openhab/test') return json({ok: true, version: '5.1.4', items: Object.keys(d.items.items).length});
    return ok();   // settings saved: accepted, not kept
  }

  window.fetch = function (input, init) {
    const url = new URL(typeof input === 'string' ? input : input.url, location.href);
    const at = url.pathname.indexOf('/api/v1');
    if (url.origin !== location.origin || at < 0) return realFetch(input, init);
    const path = url.pathname.slice(at + 7).replace(/^\//, '');
    const method = ((init && init.method) || (typeof input !== 'string' && input.method) || 'GET').toUpperCase();
    return route(method, path, url.searchParams, init && init.body);
  };

  // uploads (firmware, display design, backups) go over XMLHttpRequest: refused, as by a panel that is not there
  const RealXHR = window.XMLHttpRequest;
  window.XMLHttpRequest = function () {
    const x = new RealXHR();
    const open = x.open.bind(x);
    x.open = (method, url, ...rest) => {
      if (!new URL(url, location.href).pathname.includes('/api/v1')) return open(method, url, ...rest);
      // a stand-in: the upload "fails" at once with the panel's error format
      const fake = {status: 400, readyState: 4, responseText: '{"error":"Not in the demo: this needs a panel."}'};
      fake.response = fake.responseText;
      Object.defineProperties(x, Object.fromEntries(Object.entries(fake).map(([k, v]) => [k, {get: () => v}])));
      x.send = () => setTimeout(() => {
        if (typeof x.onreadystatechange === 'function') x.onreadystatechange();
        if (typeof x.onload === 'function') x.onload();
        x.dispatchEvent(new Event('load'));
        x.dispatchEvent(new Event('loadend'));
      }, 300);
      x.setRequestHeader = () => {};
    };
    return x;
  };

  // a note at the top: what this is
  document.addEventListener('DOMContentLoaded', () => {
    // the note about http vs https is about a panel: there is none
    const st = document.createElement('style');
    st.textContent = '.sidesec{display:none!important}';
    document.head.appendChild(st);
    const bar = document.createElement('div');
    // the other variant: the same page and configuration, its layout
    const pick = v => v === variant ? '<b style="text-decoration:underline">' + VARIANTS[v] + '</b>'
      : '<a href="?variant=' + v + '" data-variant style="color:#fff">' + VARIANTS[v] + '</a>';
    bar.innerHTML = 'OHpanel demo: invented data, no panel. Changes stay in this tab. NSPanel ' +
      Object.keys(VARIANTS).map(pick).join(' · ') + ' · <a href="../licences/THIRD-PARTY-NOTICES.md" style="color:#fff">Licences</a>';
    bar.querySelectorAll('a[data-variant]').forEach(a => a.addEventListener('click', () => { a.href += location.hash; }));
    bar.style.cssText = 'position:fixed;left:50%;bottom:12px;transform:translateX(-50%);z-index:9999;padding:6px 14px;' +
      'border-radius:999px;background:#e64a19;color:#fff;font:600 13px system-ui,sans-serif;box-shadow:0 2px 8px rgba(0,0,0,.3)';
    document.body.appendChild(bar);
  });
})();
