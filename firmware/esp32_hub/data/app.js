'use strict';
// eLingo Hub student page. Talks only to the Hub (same origin), never to the internet.

(function () {
  var $ = function (id) { return document.getElementById(id); };
  var HEARTBEAT_MS = 10000;
  var STATUS_MS = 3000;

  // ---------------------------------------------------------------- storage (may be blocked)
  var store = {
    get: function (k) { try { return window.localStorage.getItem(k); } catch (e) { return null; } },
    set: function (k, v) { try { window.localStorage.setItem(k, v); } catch (e) { /* private mode */ } },
  };

  // A stable random id per phone: a refresh or reconnect re-uses the same subscription.
  function newListenerId() {
    var bytes = new Uint8Array(12);
    window.crypto.getRandomValues(bytes);
    var s = 'l';
    for (var i = 0; i < bytes.length; i++) s += (bytes[i] < 16 ? '0' : '') + bytes[i].toString(16);
    return s;
  }
  var listenerId = store.get('elingo.listenerId');
  if (!listenerId || !/^[A-Za-z0-9_-]{8,64}$/.test(listenerId)) {
    listenerId = newListenerId();
    store.set('elingo.listenerId', listenerId);
  }

  var state = {
    status: null,
    language: store.get('elingo.language'),
    joined: false,
    hubReachable: false,
    wantPlaying: false,
  };

  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

  function api(path, opts) {
    opts = opts || {};
    var ctrl = typeof AbortController !== 'undefined' ? new AbortController() : null;
    var timer = setTimeout(function () { if (ctrl) ctrl.abort(); }, opts.timeout || 8000);
    return fetch(path, {
      method: opts.method || 'GET',
      headers: opts.body ? { 'Content-Type': 'application/json' } : undefined,
      body: opts.body ? JSON.stringify(opts.body) : undefined,
      cache: 'no-store',
      signal: ctrl ? ctrl.signal : undefined,
    }).then(function (res) {
      clearTimeout(timer);
      var isJson = (res.headers.get('content-type') || '').indexOf('json') >= 0;
      return (isJson ? res.json() : Promise.resolve(null)).then(function (data) {
        if (!res.ok) {
          var err = new Error((data && data.message) || ('HTTP ' + res.status));
          err.status = res.status;
          err.code = data && data.error;
          throw err;
        }
        return data;
      });
    }, function (e) { clearTimeout(timer); throw e; });
  }

  // ---------------------------------------------------------------- UI helpers
  function setMessage(text, bad) {
    var m = $('message');
    m.textContent = text || '';
    m.className = 'message' + (bad ? ' bad' : '');
  }
  function setPlayState(text) { $('play-state').textContent = text; }
  function languageInfo(code) {
    var langs = (state.status && state.status.languages) || [];
    for (var i = 0; i < langs.length; i++) if (langs[i].code === code) return langs[i];
    return null;
  }
  function languageLabel(code) {
    var l = languageInfo(code);
    return l ? l.nativeName || l.name : '—';
  }

  function renderStatus() {
    var s = state.status;
    var pill = $('conn-pill');
    if (!state.hubReachable) {
      pill.textContent = 'Hub unreachable';
      pill.className = 'pill pill-bad';
      $('session-line').textContent = 'Reconnecting to the Hub…';
      return;
    }
    $('hub-name').textContent = s.hub.name || 'eLingo Hub';
    if (s.session.active) {
      pill.textContent = s.session.degraded || !s.cloud.online ? 'Live · weak link' : 'Live';
      pill.className = s.session.degraded || !s.cloud.online ? 'pill pill-wait' : 'pill pill-ok';
      $('session-line').textContent = 'Lecture in progress';
    } else {
      pill.textContent = 'Waiting';
      pill.className = 'pill pill-wait';
      $('session-line').textContent = 'Waiting for the lecture to start';
    }
    $('clients').textContent = s.network.clients + ' of ' + s.network.maxClients + ' devices connected';

    var grid = $('languages');
    var langs = s.languages || [];
    $('lang-empty').classList.toggle('hidden', langs.length > 0);
    if (grid.childElementCount !== langs.length) {
      grid.textContent = '';
      langs.forEach(function (l) {
        var b = document.createElement('button');
        b.type = 'button';
        b.className = 'lang';
        b.setAttribute('role', 'radio');
        b.dataset.code = l.code;
        var strong = document.createElement('strong');
        var span = document.createElement('span');
        b.appendChild(strong);
        b.appendChild(span);
        b.addEventListener('click', function () { chooseLanguage(l.code); });
        grid.appendChild(b);
      });
    }
    Array.prototype.forEach.call(grid.children, function (b) {
      var l = languageInfo(b.dataset.code);
      if (!l) return;
      b.firstChild.textContent = l.nativeName || l.name;
      var n = l.listeners || 0;
      b.lastChild.textContent = (l.name !== l.nativeName ? l.name + ' · ' : '') + n + ' listening';
      b.setAttribute('aria-checked', String(state.language === l.code));
    });

    $('now-lang').textContent = state.language ? languageLabel(state.language) : '—';
    $('listen').disabled = !state.language;
    $('listen').textContent = state.wantPlaying ? 'Pause' : (player.started ? 'Resume' : 'Start listening');
    $('leave').disabled = !state.joined;
  }

  // ---------------------------------------------------------------- subscription lifecycle
  function join() {
    if (!state.language) return Promise.resolve();
    return api('/api/join', { method: 'POST', body: { listenerId: listenerId, language: state.language } })
      .then(function () {
        state.joined = true;
        setMessage('');
        renderStatus();
      })
      .catch(function (e) {
        if (e.code === 'hub_full') setMessage('This Hub is full. Ask your lecturer about another Hub.', true);
        else setMessage('Could not reach the Hub. Retrying…', true);
        throw e;
      });
  }

  function chooseLanguage(code) {
    if (state.language === code && state.joined) return;
    state.language = code;
    store.set('elingo.language', code);
    renderStatus();
    // Joining with a new language switches the existing subscription (no duplicates).
    join().then(function () { player.switchTo(code); }).catch(function () { /* heartbeat retries */ });
  }

  function heartbeatLoop() {
    var delay = HEARTBEAT_MS;
    var beat = function () {
      if (!state.joined) { setTimeout(beat, 2000); return; }
      api('/api/heartbeat', { method: 'POST', body: { listenerId: listenerId, playedSeq: player.cursor } })
        .then(function () { delay = HEARTBEAT_MS; })
        .catch(function (e) {
          if (e.status === 404) return join();  // Hub restarted or we expired: re-subscribe
          delay = 3000;                         // Hub unreachable: retry sooner
        })
        .then(function () { setTimeout(beat, delay); }, function () { setTimeout(beat, delay); });
    };
    setTimeout(beat, HEARTBEAT_MS);
  }

  function statusLoop() {
    api('/api/status', { timeout: 5000 })
      .then(function (s) {
        var wasUnreachable = !state.hubReachable;
        state.status = s;
        state.hubReachable = true;
        if (state.language && !languageInfo(state.language) && s.languages.length) state.language = null;
        if (wasUnreachable && state.language && !state.joined) join().catch(function () {});
        renderStatus();
      })
      .catch(function () {
        state.hubReachable = false;
        renderStatus();
      })
      .then(function () { setTimeout(statusLoop, STATUS_MS); });
  }

  function leave() {
    player.stop();
    state.wantPlaying = false;
    api('/api/leave', { method: 'POST', body: { listenerId: listenerId } }).catch(function () {});
    state.joined = false;
    setPlayState('Not started');
    setMessage('You left the session. Choose a language to listen again.');
    renderStatus();
  }

  // ---------------------------------------------------------------- audio player
  // Segments are fetched one by one and scheduled back-to-back on a Web Audio clock (gapless).
  // Each phone has its own cursor; reading a segment does not remove it for anyone else.
  var player = {
    ctx: null,
    started: false,
    running: false,
    lang: null,
    cursor: -1,
    nextTime: 0,
    generation: 0,
    sources: [],
    pausedAt: 0,

    ensureContext: function () {
      if (!this.ctx) {
        var AC = window.AudioContext || window.webkitAudioContext;
        if (!AC) throw new Error('This browser cannot play audio streams.');
        this.ctx = new AC();
      }
      return this.ctx.resume();
    },
    start: function (lang) {
      var self = this;
      return this.ensureContext().then(function () {
        // After a long pause, jump to the live edge instead of replaying old audio.
        if (self.pausedAt && Date.now() - self.pausedAt > 15000) self.cursor = -1;
        self.pausedAt = 0;
        self.lang = lang;
        self.started = true;
        if (!self.running) {
          self.running = true;
          self.loop(++self.generation);
        }
      });
    },
    pause: function () {
      this.running = false;
      this.pausedAt = Date.now();
      this.stopSources();
      this.nextTime = 0;
      if (this.ctx) this.ctx.suspend();
    },
    stop: function () {
      this.pause();
      this.cursor = -1;
      this.started = false;
    },
    switchTo: function (lang) {
      if (this.lang === lang) return;
      this.lang = lang;
      this.cursor = -1;
      this.stopSources();
      this.nextTime = 0;
      if (this.running) this.loop(++this.generation);
    },
    stopSources: function () {
      this.sources.forEach(function (s) { try { s.stop(); } catch (e) { /* already ended */ } });
      this.sources = [];
    },
    bufferedSeconds: function () {
      return this.ctx ? Math.max(0, this.nextTime - this.ctx.currentTime) : 0;
    },
    decode: function (buf) {
      var ctx = this.ctx;
      return new Promise(function (resolve, reject) {
        var p = ctx.decodeAudioData(buf, resolve, reject);
        if (p && p.then) p.then(resolve, reject);
      });
    },
    loop: function (gen) {
      var self = this;
      var alive = function () { return self.running && gen === self.generation; };
      (function step() {
        if (!alive()) return;
        if (self.bufferedSeconds() > 6) { setTimeout(step, 500); return; }
        var lang = self.lang;
        api('/api/segments?lang=' + encodeURIComponent(lang) + '&after=' + self.cursor)
          .then(function (list) {
            if (!alive()) return;  // paused/switched while the request was in flight
            var segs = list.segments || [];
            if (self.cursor < 0 && segs.length > 1) segs = segs.slice(-1);  // join at the live edge
            if (!segs.length) {
              // Nothing new yet. Only report waiting once the already-scheduled audio has run out.
              if (self.bufferedSeconds() < 0.25) {
                setPlayState(list.sessionActive ? 'Waiting for translation…' : 'Waiting for the lecture');
              }
              return sleep(1000);
            }
            return segs.reduce(function (chain, seg) {
              return chain.then(function () {
                if (!alive() || self.lang !== lang) return;
                return fetch('/api/audio?lang=' + encodeURIComponent(lang) + '&seq=' + seg.seq, { cache: 'no-store' })
                  .then(function (res) {
                    if (res.status === 404) { self.cursor = seg.seq; return null; }  // already evicted: skip
                    if (!res.ok) throw new Error('HTTP ' + res.status);
                    return res.arrayBuffer();
                  })
                  .then(function (buf) { return buf ? self.decode(buf) : null; })
                  .then(function (audio) {
                    if (!audio || !alive() || self.lang !== lang) return;
                    var src = self.ctx.createBufferSource();
                    src.buffer = audio;
                    src.connect(self.ctx.destination);
                    var now = self.ctx.currentTime;
                    if (self.nextTime < now + 0.05) self.nextTime = now + 0.15;  // underrun: restart just ahead
                    src.start(self.nextTime);
                    self.nextTime += audio.duration;
                    self.sources.push(src);
                    src.onended = function () {
                      var i = self.sources.indexOf(src);
                      if (i >= 0) self.sources.splice(i, 1);
                    };
                    self.cursor = seg.seq;
                    setPlayState('Playing');
                    setMessage('');
                  });
              });
            }, Promise.resolve());
          })
          .catch(function () {
            if (!alive()) return;
            setPlayState('Reconnecting…');
            return sleep(2000);
          })
          .then(function () { setTimeout(step, 50); });
      })();
    },
  };

  setInterval(function () {
    var pct = Math.min(100, (player.bufferedSeconds() / 10) * 100);
    $('buffer-bar').style.width = pct + '%';
  }, 500);

  // ---------------------------------------------------------------- wiring
  $('listen').addEventListener('click', function () {
    if (state.wantPlaying) {
      state.wantPlaying = false;
      player.pause();
      setPlayState('Paused');
      renderStatus();
      return;
    }
    state.wantPlaying = true;
    renderStatus();
    // The AudioContext must be created inside this tap (mobile autoplay rules).
    player.start(state.language)
      .then(function () { return state.joined ? null : join(); })
      .then(function () { setPlayState('Starting…'); })
      .catch(function (e) {
        state.wantPlaying = false;
        setMessage(e.message || 'Could not start audio.', true);
        renderStatus();
      });
  });
  $('leave').addEventListener('click', leave);
  $('dismiss-welcome').addEventListener('click', function () {
    $('welcome').classList.add('hidden');
    store.set('elingo.welcomeSeen', '1');
  });
  if (store.get('elingo.welcomeSeen') === '1') $('welcome').classList.add('hidden');

  statusLoop();
  heartbeatLoop();
})();
