// Pagan engine. Copyright (c) 2026 Mark B. Licensed under the GNU GPL v3 or later, see LICENSE.
//
// The game's data comes in packs named by what's in them and the hash of their bytes, plus the
// program itself (pack "program", the .wasm); tools/web_pack.py cuts each into chunks of 512 KB
// (files/<name>.<hash>.<i>.part) and lists them in version.json. Each chunk is downloaded on its
// own and kept in the browser's cache storage the moment it is complete, so a slow or broken
// connection, a reload, a closed tab or a VPN switch loses at most the chunks in flight, and a
// new version only downloads the chunks of the packs that changed. Three chunks come at a time;
// a chunk that gets no bytes for 15 s is dropped and asked for again (2, 4, 8 ... 30 s apart,
// for as long as the page is open, at once when the browser comes back online). Used by the
// game page (shell.html) and by exit.html. Saves (IndexedDB) and settings (localStorage) are
// separate and never touched here.
//
// Pack format: "PGPK", a 4-byte little-endian length, that much JSON ([[path, offset, size], ...]),
// then the files' bytes (offsets count from the end of the JSON).
(function () {
  const CACHE = 'pagan-packs';
  const PARALLEL = 3, STALL_MS = 15000;
  const base = new URL('.', location.href);
  const url = c => new URL('files/' + c.file, base).href;
  const caches_ok = typeof caches !== 'undefined';
  const digest_ok = typeof crypto !== 'undefined' && crypto.subtle && crypto.subtle.digest;

  async function manifest() {
    const r = await fetch(new URL('version.json', base), { cache: 'no-store', credentials: 'same-origin' });
    if (!r.ok) throw new Error('version.json: ' + r.status);
    return r.json();
  }

  let store = null;
  async function cache() {
    if (!caches_ok) return null;
    try { return store || (store = await caches.open(CACHE)); } catch (e) { return null; }
  }
  async function has(c) {
    const s = await cache();
    try { return s ? !!(await s.match(url(c))) : false; } catch (e) { return false; }
  }
  async function cached(c) {
    const s = await cache();
    try {
      const r = s && await s.match(url(c));
      if (!r) return null;
      const buf = await r.arrayBuffer();
      return buf.byteLength === c.size ? buf : null;
    } catch (e) { return null; }
  }
  async function keep(c, buf) {
    const s = await cache();
    if (!s) return;
    try { await s.put(url(c), new Response(buf, { headers: { 'Content-Type': 'application/octet-stream' } })); }
    catch (e) { console.warn('packs: could not keep', c.file, e); }
  }
  const hex = buf => Array.from(new Uint8Array(buf), b => b.toString(16).padStart(2, '0')).join('');
  async function verify(c, buf) {
    if (buf.byteLength !== c.size) return false;
    if (!c.sha1 || !digest_ok) return true;
    try { return hex(await crypto.subtle.digest('SHA-1', buf)) === c.sha1; } catch (e) { return true; }
  }

  // Waits `s` seconds (or until the browser says it is online again), calling tick(left) each second.
  function pause(s, tick) {
    return new Promise(done => {
      let left = s;
      const end = () => { clearInterval(t); removeEventListener('online', end); done(); };
      const t = setInterval(() => { left -= 1; if (left <= 0) end(); else tick(left); }, 1000);
      addEventListener('online', end);
      tick(left);
    });
  }

  // One chunk from the server, retried until it comes. ev: {bytes(n), stalled(left, why), ok()}.
  async function download(c, ev) {
    let wait = 2, fails = 0;
    for (;;) {
      const ctl = typeof AbortController !== 'undefined' ? new AbortController() : null;
      let timer = 0, got = 0, why = '';
      const arm = () => { clearTimeout(timer); timer = setTimeout(() => { why = 'stalled'; if (ctl) ctl.abort(); }, STALL_MS); };
      try {
        arm();
        const r = await fetch(url(c), { credentials: 'same-origin', signal: ctl ? ctl.signal : undefined });
        if (r.status === 404) { const e = new Error(c.file + ' is no longer on the server'); e.gone = true; throw e; }
        if (!r.ok) throw new Error(c.file + ': ' + r.status);
        let buf;
        if (r.body && r.body.getReader) {
          const reader = r.body.getReader(), all = new Uint8Array(c.size);
          for (;;) {
            const { done, value } = await reader.read();
            if (done) break;
            arm();
            if (got + value.length > all.length) throw new Error(c.file + ': too long');
            all.set(value, got);
            got += value.length;
            ev.bytes(value.length);
          }
          buf = all.buffer;
        } else {
          buf = await r.arrayBuffer();
          got = buf.byteLength;
          ev.bytes(got);
        }
        clearTimeout(timer);
        if (got !== c.size || !(await verify(c, buf))) throw new Error(c.file + ': damaged, fetching it again');
        await keep(c, buf);
        ev.ok();
        return buf;
      } catch (e) {
        clearTimeout(timer);
        ev.bytes(-got);
        if (e.gone && ++fails >= 3) throw e;
        console.warn('packs:', why || e.message || e);
        await pause(wait, left => ev.stalled(left, why === 'stalled' ? 'no data for 15 s' : 'connection lost'));
        ev.stalled(0);
        wait = Math.min(30, wait * 2);
      }
    }
  }

  // What of `m` (or of its `packs`) is not kept yet: {chunks, bytes, have, total}.
  async function missing(m, packs) {
    const out = { chunks: [], bytes: 0, have: 0, total: 0 };
    for (const p of packs || m.packs)
      for (const c of p.chunks) {
        out.total += c.size;
        if (await has(c)) out.have += c.size;
        else { out.chunks.push(c); out.bytes += c.size; }
      }
    return out;
  }

  // Downloads (three at a time) every chunk of `packs` not kept yet. onStatus(s) about four times
  // a second: {total, done, have (kept before this started), need, speed (bytes/s), stalled
  // (seconds to the next try, 0 = none), why}. Returns {file: ArrayBuffer} of what it fetched.
  async function fetch_chunks(m, packs, onStatus) {
    const miss = await missing(m, packs);
    const st = { total: miss.total, have: miss.have, need: miss.bytes, done: miss.have, speed: 0, stalled: 0, why: '' };
    const got = {}, samples = [];
    const stalls = new Map();
    let last = 0;
    const report = force => {
      const now = performance.now();
      samples.push([now, st.done]);
      while (samples.length > 2 && now - samples[0][0] > 5000) samples.shift();
      const [t0, b0] = samples[0];
      st.speed = now - t0 > 500 ? Math.max(0, (st.done - b0) / ((now - t0) / 1000)) : st.speed;
      const s = [...stalls.values()];
      st.stalled = s.length ? Math.min(...s.map(x => x[0])) : 0;
      st.why = s.length ? s[0][1] : '';
      if (onStatus && (force || now - last > 250)) { last = now; onStatus(Object.assign({}, st)); }
    };
    const timer = setInterval(() => report(true), 1000);
    const queue = miss.chunks.slice();
    async function worker() {
      for (let c; (c = queue.shift());) {
        got[c.file] = await download(c, {
          bytes: n => { st.done += n; report(); },
          stalled: (left, why) => { if (left) stalls.set(c.file, [left, why]); else stalls.delete(c.file); report(true); },
          ok: () => stalls.delete(c.file),
        });
      }
    }
    try {
      report(true);
      await Promise.all(Array.from({ length: Math.min(PARALLEL, queue.length) }, worker));
      report(true);
    } finally { clearInterval(timer); }
    return got;
  }

  // Every pack of `m` assembled: {name: Uint8Array}. Missing chunks are downloaded first.
  async function load(m, onStatus) {
    const got = await fetch_chunks(m, m.packs, onStatus);
    const out = {};
    for (const p of m.packs) {
      const all = new Uint8Array(p.size);
      let at = 0;
      for (const c of p.chunks) {
        let buf = got[c.file] || await cached(c);
        if (!buf) buf = await download(c, { bytes() {}, stalled() {}, ok() {} });  // evicted meanwhile
        all.set(new Uint8Array(buf), at);
        at += c.size;
      }
      out[p.name] = all;
    }
    return out;
  }

  function unpack(bytes) {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (String.fromCharCode(...bytes.subarray(0, 4)) !== 'PGPK') throw new Error('not a pack');
    const n = view.getUint32(4, true);
    const index = JSON.parse(new TextDecoder().decode(bytes.subarray(8, 8 + n)));
    const start = 8 + n;
    return index.map(([path, off, size]) => ({ path, data: bytes.subarray(start + off, start + off + size) }));
  }

  // Throws away chunks no longer in `m` (older versions; the server keeps the one before).
  async function prune(m) {
    const s = await cache();
    if (!s) return;
    try {
      const keepSet = new Set();
      for (const p of m.packs) for (const c of p.chunks) keepSet.add(url(c));
      for (const req of await s.keys()) if (!keepSet.has(req.url)) await s.delete(req);
    } catch (e) { /* not important */ }
  }

  // Downloads every chunk of `m` not kept yet (the exit page's "Update now").
  async function fetch_all(m, onStatus) {
    return Object.keys(await fetch_chunks(m, m.packs, onStatus)).length;
  }

  // Status text for the loading screen and the exit page.
  const mb = b => (b / 1048576).toFixed(1);
  function describe(s) {
    const pct = s.total ? Math.floor(100 * s.done / s.total) : 100;
    let t = mb(s.done) + ' of ' + mb(s.total) + ' MB (' + pct + '%)';
    if (s.speed > 0) t += ' · ' + (s.speed >= 1048576 ? mb(s.speed) + ' MB/s' : Math.round(s.speed / 1024) + ' KB/s');
    const lines = [t];
    if (s.have > 0 && s.need > 0) lines.push('Resuming: ' + mb(s.have) + ' MB already downloaded');
    if (s.stalled) lines.push('Connection ' + (s.why === 'no data for 15 s' ? 'stalled' : 'lost') + ', retrying in ' + s.stalled + ' s');
    return { pct, lines };
  }

  // The version this browser has complete: {version, build, built}.
  const installed = () => { try { return JSON.parse(localStorage.getItem('pagan.version') || 'null'); } catch (e) { return null; } };
  const set_installed = m => {
    try { localStorage.setItem('pagan.version', JSON.stringify({ version: m.version, build: m.build, built: m.built })); } catch (e) { }
  };

  // --- The player's own game folder (the public build, version.json "public") ---------------
  // The files are read here in the browser and kept in IndexedDB; they never leave the machine.
  const GAME_DB = 'u8eg-game', GAME_STORE = 'files';
  const GAME_DIRS = ['STATIC', 'GAMEDAT', 'USECODE', 'SOUND', 'SAVEGAME'];
  const GAME_REQUIRED = ['STATIC/U8PAL.PAL', 'STATIC/XFORMPAL.DAT', 'STATIC/U8SHAPES.FLX', 'STATIC/U8GUMPS.FLX',
    'STATIC/U8FONTS.FLX', 'STATIC/U8MOUSE.SHP', 'STATIC/TYPEFLAG.DAT', 'STATIC/ANIM.DAT', 'STATIC/GLOB.FLX',
    'STATIC/FIXED.DAT', 'GAMEDAT/NONFIXED.DAT', 'GAMEDAT/ITEMCACH.DAT', 'GAMEDAT/NPCDATA.DAT'];

  function game_db() {
    return new Promise((ok, no) => {
      const q = indexedDB.open(GAME_DB, 1);
      q.onupgradeneeded = () => q.result.createObjectStore(GAME_STORE, { keyPath: 'path' });
      q.onsuccess = () => ok(q.result);
      q.onerror = () => no(q.error);
    });
  }
  const db_done = tx => new Promise((ok, no) => { tx.oncomplete = ok; tx.onerror = () => no(tx.error); tx.onabort = () => no(tx.error); });

  // Files of a chosen folder: finds the game inside it (the folder itself or anywhere below,
  // any case), keeps what the engine reads. Throws an Error naming what is missing.
  function game_select(fileList) {
    const all = Array.from(fileList, f => ({ f, rel: (f.webkitRelativePath || f.name).replace(/\\/g, '/') }));
    let root = null;
    for (const a of all) {
      const parts = a.rel.split('/');
      if (parts.length >= 2 && parts[parts.length - 1].toUpperCase() === 'U8PAL.PAL' && parts[parts.length - 2].toUpperCase() === 'STATIC') {
        const r = parts.slice(0, -2).join('/');
        if (root === null || r.length < root.length) root = r;
      }
    }
    if (root === null) throw new Error('That folder does not hold Ultima VIII: no STATIC/U8PAL.PAL in it. Choose the folder holding STATIC, GAMEDAT and USECODE.');
    const prefix = root ? root + '/' : '';
    const out = [], have = new Set();
    for (const a of all) {
      if (!a.rel.startsWith(prefix)) continue;
      const rel = a.rel.slice(prefix.length), up = rel.toUpperCase(), top = up.split('/')[0];
      if (!GAME_DIRS.includes(top) || up.split('/').length !== 2) continue;
      if (top === 'SAVEGAME' && up !== 'SAVEGAME/U8SAVE.000') continue;  // the new-game state only
      out.push({ path: '/game/' + rel, file: a.f });
      have.add(up);
    }
    // GAMEDAT's files can come from the new-game save instead (the engine reads them from it).
    const missing = GAME_REQUIRED.filter(p => !have.has(p) && !(p.startsWith('GAMEDAT/') && have.has('SAVEGAME/U8SAVE.000')));
    if (![...have].some(p => /^USECODE\/.USECODE\.FLX$/.test(p))) missing.push('USECODE/EUSECODE.FLX');
    if (missing.length) throw new Error('This folder is missing files the game cannot run without:\n' + missing.join('\n'));
    return { name: root || (all[0] ? all[0].rel.split('/')[0] : ''), files: out };
  }

  // Reads the selected files. {path, data: Uint8Array}[]; progress(done, total).
  async function game_read(sel, progress) {
    const out = [];
    for (let i = 0; i < sel.files.length; ++i) {
      out.push({ path: sel.files[i].path, data: new Uint8Array(await sel.files[i].file.arrayBuffer()) });
      if (progress) progress(i + 1, sel.files.length);
    }
    return out;
  }

  // Keeps them in this browser (replacing the folder kept before). False if it can't.
  async function game_store(files, name) {
    try {
      const db = await game_db();
      let tx = db.transaction(GAME_STORE, 'readwrite');
      tx.objectStore(GAME_STORE).clear();
      for (const f of files) tx.objectStore(GAME_STORE).put({ path: f.path, data: f.data });
      await db_done(tx);
      db.close();
      localStorage.setItem('u8eg.folder', JSON.stringify({ name, files: files.length }));
      if (navigator.storage && navigator.storage.persist) navigator.storage.persist().catch(() => {});
      return true;
    } catch (e) { console.warn('game folder: could not keep it in this browser', e); return false; }
  }

  // The folder kept before: {path, data}[] or null.
  async function game_stored() {
    try {
      const db = await game_db();
      const tx = db.transaction(GAME_STORE, 'readonly');
      const q = tx.objectStore(GAME_STORE).getAll();
      const rows = await new Promise((ok, no) => { q.onsuccess = () => ok(q.result); q.onerror = () => no(q.error); });
      db.close();
      return rows && rows.length ? rows : null;
    } catch (e) { console.warn('game folder: could not read this browser\'s copy', e); return null; }
  }

  async function game_forget() {
    try {
      const db = await game_db();
      const tx = db.transaction(GAME_STORE, 'readwrite');
      tx.objectStore(GAME_STORE).clear();
      await db_done(tx);
      db.close();
      localStorage.removeItem('u8eg.folder');
    } catch (e) { console.warn('game folder: could not forget it', e); }
  }

  window.PGGame = { select: game_select, read: game_read, store: game_store, stored: game_stored, forget: game_forget };

  window.PGPacks = { manifest, missing, load, unpack, prune, fetch_all, describe, installed, set_installed, cached_ok: caches_ok };
})();
