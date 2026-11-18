/* Responsive client. Profile and conversation ownership are verified by the gateway. */
(() => {
  const $ = id => document.getElementById(id);
  const read = (key, fallback) => { try { return localStorage.getItem(key) ?? fallback; } catch { return fallback; } };
  const save = (key, value) => { try { localStorage.setItem(key, value); } catch {} };
  let gateway = null, audio = null, handsFree = false, held = false, approval = null, approvalTimer = null;
  let muted = false, state = 'idle', loginGeneration = 0, session = null, expiryTimer = null, animationFrame = 0;
  const portrait = new AvatarView($('portrait'), () => {
    $('portrait').hidden = true; notice('Avatar artwork unavailable. Conversation controls remain ready.');
  }, $('voice-meter'));
  const motion = matchMedia('(prefers-reduced-motion: reduce)');
  const labels = { idle: 'Ready', listening: 'Listening', thinking: 'Thinking', working: 'Working',
    speaking: 'Speaking', success: 'Done', error: 'Needs attention', approval: 'Waiting for approval', offline: 'Disconnected' };
  function notice(text = '') { $('notice').textContent = text; $('sign-in-status').textContent = text; }
  function avatar(id) {
    id = ['zenti', 'orbit'].includes(id) ? id : 'zenti';
    $('avatar').dataset.avatar = id; $('avatar-select').value = id;
    portrait.select(id); $('portrait').alt = id === 'zenti' ? 'Zenti' : 'Orbit';
    $('avatar-name').textContent = $('portrait').alt; save('zenti-avatar', id);
    scheduleAvatar();
  }
  function render(next = state) {
    state = next === 'executing' ? 'working' : next; $('avatar').dataset.state = state;
    $('voice-meter').dataset.state = state;
    $('activity').textContent = (labels[state] || state) + (muted ? ' · Mic muted' : '');
    const ready = !!gateway?.ready && gateway.contextReady;
    const busy = held || !!gateway?.turn || !!audio?.starting || !!audio?.capturing || !!audio?.playing;
    $('talk').disabled = !ready || muted || handsFree || (busy && !held);
    $('talk').textContent = held ? 'Release to send' : 'Hold to talk';
    $('send').disabled = !ready || !!gateway?.turn;
    $('new-conversation').disabled = !gateway?.ready || !!gateway?.turn || !!gateway?.pendingContext;
    $('hands-free').disabled = !ready || muted || held || !!approval;
    $('hands-free').textContent = handsFree ? 'End hands-free' : 'Hands-free';
    $('hands-free').setAttribute('aria-label', handsFree ? 'End hands-free' : 'Start hands-free');
    $('hands-free').setAttribute('aria-pressed', String(handsFree));
    $('mic').textContent = muted ? 'Unmute mic' : 'Mute mic';
    $('mic').setAttribute('aria-label', muted ? 'Unmute microphone' : 'Mute microphone');
    $('mic').setAttribute('aria-pressed', String(muted));
    const cancelFocused = document.activeElement === $('stop');
    $('stop').disabled = !busy; $('stop').hidden = !busy;
    $('stop').textContent = audio?.playing ? 'Stop audio' : held || audio?.capturing || audio?.starting ? 'Cancel recording' : 'Cancel reply';
    if (!busy && cancelFocused) ($('talk').disabled ? $('mic') : $('talk')).focus({ preventScroll: true });
    $('reconnect').disabled = !gateway;
    $('connection').dataset.connected = String(ready);
    $('empty').textContent = !session ? 'Sign in in Settings to start a private conversation.'
      : !ready ? 'Connecting your private conversation…' : 'A conversation starts with you. Type a message or hold to talk.';
    $('voice-hint').textContent = !session ? 'Sign in to talk or send a message.'
      : muted ? 'Microphone muted. You can still type a message.'
      : held ? 'Release to send. Cancel discards this recording.'
      : audio?.playing ? 'Playing your reply. Stop audio ends this response.'
      : handsFree ? 'Hands-free is on. Mute mic to pause.'
      : busy ? 'Preparing your reply. You can cancel at any time.' : 'Hold to talk, or turn on hands-free.';
    scheduleAvatar();
  }
  function halt(cancel = true) {
    handsFree = false; held = false; audio?.abortCapture();
    if (cancel) gateway?.cancel(); render();
  }
  function clearConversation() {
    $('transcript').replaceChildren(); $('transcript').hidden = true; $('empty').hidden = false; $('export').disabled = true;
    $('message').value = ''; clearApproval();
    $('new-replies').hidden = true;
  }
  function clearApproval() {
    clearTimeout(approvalTimer); approvalTimer = null; approval = null;
    if ($('approval').open) $('approval').close();
  }
  function append(event) {
    if (!gateway?.ready || !gateway.contextReady) return;
    const transcript = $('transcript');
    const follow = transcript.hidden || transcript.scrollHeight - transcript.scrollTop - transcript.clientHeight < 64;
    const li = document.createElement('li'), role = event.type === 'transcript.final' ? 'user' : 'assistant';
    const title = document.createElement('strong'); title.textContent = role === 'user' ? 'You' : 'Assistant';
    const heading = document.createElement('div'); heading.className = 'message-heading'; heading.append(title);
    const copy = document.createElement('button'); copy.type = 'button'; copy.className = 'copy-message';
    copy.textContent = 'Copy'; copy.setAttribute('aria-label', 'Copy ' + title.textContent.toLowerCase() + ' message');
    copy.onclick = async () => {
      try { await navigator.clipboard.writeText(event.text); copy.textContent = 'Copied'; }
      catch { notice('Copy unavailable. Select the message text to copy it.'); }
    };
    heading.append(copy); li.dataset.role = role; li.messageText = event.text;
    li.append(heading, window.renderMessage(event.text));
    transcript.append(li); while (transcript.children.length > 64) transcript.firstChild.remove();
    $('empty').hidden = true; transcript.hidden = false; $('export').disabled = false;
    if (follow) transcript.scrollTop = transcript.scrollHeight;
    $('new-replies').hidden = follow;
  }
  $('new-replies').onclick = () => {
    $('transcript').scrollTop = $('transcript').scrollHeight; $('new-replies').hidden = true;
  };
  $('transcript').addEventListener('scroll', () => {
    const el = $('transcript');
    if (el.scrollHeight - el.scrollTop - el.clientHeight < 64) $('new-replies').hidden = true;
  });
  async function arm() {
    if (!handsFree || document.hidden || muted || !gateway?.ready || !gateway.contextReady || gateway.turn || audio.starting || audio.capturing || approval) return;
    const owner = audio;
    if (!(await owner.startCapture(true))) {
      if (audio === owner) { handsFree = false; render(); }
    } else if (audio === owner) {
      await devices();
      if (audio === owner && owner.capturing && handsFree && !muted) render('listening');
    }
  }
  function connect(info) {
    halt(); gateway?.disconnect(); void audio?.dispose(); clearConversation();
    const client = new GatewayClient(location.origin.replace(/^https:/, 'wss:') + '/gateway', { device_id: info.device_id }, true);
    gateway = client; client.useContext = true;
    const engine = new AudioEngine(client); audio = engine;
    showVolume(engine.level);
    engine.setMicMuted(muted); engine.voiceReplies = $('voice-replies').checked;
    engine.inputDevice = $('input-device').value;
    let checkingSession = false;
    async function checkSession() {
      if (checkingSession || session !== info) return;
      checkingSession = true;
      try {
        const response = await fetch('/api/session', { credentials: 'same-origin', cache: 'no-store' });
        if (response.status === 401 && gateway === client && session === info) {
          signedOut(); notice('Session ended. Sign in again.');
        }
      } catch { /* Network loss retains the normal bounded reconnect backoff. */ }
      finally { checkingSession = false; }
    }
    const on = (target, type, fn) => target.addEventListener(type, e => { if (gateway === client) fn(e); });
    on(client, 'state', e => {
      $('connection').textContent = e.detail.state === 'ready' ? 'Connected' : e.detail.state;
      if (e.detail.state !== 'ready') {
        halt(false); $('transcript').hidden = true; $('empty').hidden = false; clearApproval();
        if (e.detail.state === 'disconnected') void checkSession();
      }
      if (e.detail.detail) notice(e.detail.detail.replaceAll('_', ' '));
      render(e.detail.state === 'ready' ? 'idle' : 'offline');
    });
    on(client, 'conversation', e => {
      if (!e.detail.same) clearConversation();
      $('transcript').hidden = !client.contextReady || !$('transcript').children.length;
      $('empty').hidden = !$('transcript').hidden;
      if (e.detail.error) notice(e.detail.error); else notice(); render();
    });
    on(client, 'text', e => append(e.detail));
    on(client, 'voice', e => {
      const error = e.detail.error;
      if (error && error !== 'cancelled') {
        handsFree = false;
        notice(error === 'microphone_permission_denied' ? 'Microphone permission denied. You can still type a message.' : error.replaceAll('_', ' '));
      }
      render(e.detail.state);
      if (!error && e.detail.state === 'idle') void arm();
    });
    on(engine, 'capture-stop', () => {
      held = false;
      // Silent hands-free pre-roll has no gateway turn to reset its status.
      render(state === 'listening' && !client.turn ? 'idle' : state);
    });
    on(engine, 'playback-start', () => render('speaking'));
    on(engine, 'playback-end', () => render('idle'));
    on(client, 'approval', e => {
      halt(); approval = e.detail.approval_id || e.detail.id;
      $('approval-text').textContent = String(e.detail.summary || e.detail.action || 'Review this action');
      $('approval').showModal(); render('approval');
    });
    client.connect(); render();
  }
  async function devices() {
    if (!navigator.mediaDevices?.enumerateDevices) return;
    try {
      const selected = $('input-device').value;
      const list = (await navigator.mediaDevices.enumerateDevices()).filter(d => d.kind === 'audioinput');
      $('input-device').replaceChildren(new Option('System default', ''));
      list.forEach((d, i) => $('input-device').add(new Option(d.label || `Microphone ${i + 1}`, d.deviceId)));
      if (selected && !list.some(d => d.deviceId === selected)) {
        halt(); audio?.setInputDevice(''); notice('Selected microphone is unavailable. Choose another microphone.');
      } else $('input-device').value = selected;
    } catch { notice('Microphone list unavailable. You can still type.'); }
  }
  async function start(e) {
    if (e.type === 'keydown' && (![' ', 'Enter'].includes(e.key) || e.repeat)) return;
    if (e.type === 'pointerdown' && e.button !== 0) return;
    e.preventDefault(); if (held || muted || handsFree || !gateway?.contextReady || gateway.turn) return;
    held = true; if (e.type === 'pointerdown') $('talk').setPointerCapture(e.pointerId);
    const owner = audio; render();
    await owner.startCapture(); if (audio === owner) { await devices(); render(); }
  }
  function release(e) {
    if (e.type === 'keyup' && ![' ', 'Enter'].includes(e.key)) return;
    if (!held) return; e.preventDefault(); held = false; void audio.stopCapture(); render();
  }
  $('talk').addEventListener('pointerdown', start); $('talk').addEventListener('keydown', start);
  $('talk').addEventListener('pointerup', release); $('talk').addEventListener('keyup', release);
  $('talk').addEventListener('pointercancel', () => halt());
  $('talk').addEventListener('lostpointercapture', () => { if (held) halt(); });
  $('mic').onclick = () => { muted = !muted; if (muted) halt(false); audio?.setMicMuted(muted); render(); };
  $('stop').onclick = () => { halt(); audio?.stopPlayback(); };
  $('hands-free').onclick = () => { if (handsFree) halt(); else { handsFree = true; void arm(); render(); } };
  $('reconnect').onclick = () => { halt(); gateway.disconnect(); gateway.connect(); };
  $('export').onclick = () => {
    // Exports the messages shown (the page keeps the latest 64); built locally, nothing is uploaded.
    const now = new Date(), stamp = new Intl.DateTimeFormat('sv-SE', { dateStyle: 'short', timeStyle: 'short', timeZone: 'America/New_York' }).format(now);
    const parts = [`# Conversation ${stamp}`];
    for (const li of $('transcript').children) parts.push(`## ${li.querySelector('strong').textContent}`, li.messageText);
    const link = document.createElement('a');
    link.href = URL.createObjectURL(new Blob([parts.join('\n\n') + '\n'], { type: 'text/markdown' }));
    link.download = `conversation-${stamp.replace(/[^0-9]+/g, '-')}.md`; link.click();
    setTimeout(() => URL.revokeObjectURL(link.href), 1000);
  };
  $('new-conversation').onclick = () => { halt(); clearConversation(); gateway.openConversation(true); render(); };
  $('text-form').onsubmit = async e => {
    e.preventDefault(); if (!gateway?.contextReady || gateway.turn) return;
    halt(false); const client = gateway, engine = audio;
    // Text stays usable without microphone permission, including audio failures.
    if (engine.voiceReplies) try { await engine.init(); } catch { engine.voiceReplies = false; notice('Audio unavailable. Reply will be shown as text.'); }
    // Preserve an earlier reading position; New replies provides an explicit jump.
    if (gateway === client && client.sendText($('message').value)) $('message').value = '';
    render();
  };
  $('input-device').onchange = () => { halt(false); audio?.setInputDevice($('input-device').value); };
  // Opening a microphone can itself fire devicechange; devices() stops only if the chosen input vanished.
  navigator.mediaDevices?.addEventListener('devicechange', () => { void devices(); });
  $('voice-replies').onchange = () => {
    save('zenti-voice-replies', $('voice-replies').checked ? '1' : '0');
    if (audio) { audio.voiceReplies = $('voice-replies').checked; if (!audio.voiceReplies) { audio.stopPlayback(); if (gateway.turn?.state === 'draining') gateway.completePlayback(); } }
  };
  $('volume').oninput = () => {
    const level = Number($('volume').value); audio?.setLevel(level);
    save('zenti-volume-level', String(level)); showVolume(level);
  };
  function showVolume(level) {
    $('volume').value = level; $('volume-value').value = `${level}%`;
    $('volume').setAttribute('aria-valuetext', level === 0 ? 'Speaker muted' : `${level} percent`);
  }
  $('avatar-select').onchange = () => avatar($('avatar-select').value);
  function reduceMotion() {
    document.body.classList.toggle('motion', !motion.matches && !$('reduce-motion').checked && !document.hidden);
    save('zenti-reduce-motion', $('reduce-motion').checked ? '1' : '0');
    cancelAnimationFrame(animationFrame); animationFrame = 0; scheduleAvatar();
  }
  $('reduce-motion').onchange = reduceMotion; motion.addEventListener('change', reduceMotion);
  $('settings-open').onclick = () => { $('settings').showModal(); void devices(); };
  function signedOut() {
    clearTimeout(expiryTimer); session = null; loginGeneration++;
    halt(); gateway?.disconnect(); void audio?.dispose(); gateway = null; audio = null;
    clearConversation(); render('offline'); $('connection').textContent = 'Signed out';
    $('login').hidden = false; $('logout').disabled = true;
  }
  function signedIn(info) {
    if (!info || typeof info.csrf !== 'string' || !/^[A-Za-z0-9_-]{43}$/.test(info.csrf) ||
        info.gateway_path !== '/gateway' || !Number.isInteger(info.expires_in) || info.expires_in <= 0 ||
        info.expires_in > 28800 || !/^[a-z0-9-]{8,64}$/.test(info.device_id)) throw Error();
    session = info; clearTimeout(expiryTimer);
    expiryTimer = setTimeout(() => { signedOut(); notice('Session expired. Sign in again.'); }, info.expires_in * 1000);
    $('login').hidden = true; $('logout').disabled = false; connect(info); notice();
  }
  $('sign-in').onclick = async () => {
    const token = ++loginGeneration, field = $('password');
    let password = field.value; field.value = ''; $('sign-in').disabled = true; notice();
    try {
      const pending = fetch('/api/login', { method: 'POST', credentials: 'same-origin', cache: 'no-store',
        headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ password }) });
      password = '';
      const response = await pending;
      if (token !== loginGeneration) return;
      if (!response.ok) { notice(response.status === 429 ? 'Too many attempts. Wait a minute.' : 'Sign-in failed.'); return; }
      signedIn(await response.json()); if ($('settings').open) $('settings').close();
    } catch { if (token === loginGeneration) notice('Sign-in unavailable. Try again.'); }
    finally { password = ''; field.value = ''; $('sign-in').disabled = false; }
  };
  $('password').addEventListener('keydown', e => {
    if (e.key === 'Enter') { e.preventDefault(); $('sign-in').click(); }
  });
  $('logout').onclick = async () => {
    const owner = session; if (!owner) return;
    // Stop speech immediately; revoke the server session before reporting logout.
    halt(); gateway?.disconnect(); $('logout').disabled = true;
    try {
      const response = await fetch('/api/logout', { method: 'POST', credentials: 'same-origin', cache: 'no-store',
        headers: { 'X-Zenti-CSRF': owner.csrf } });
      if (!response.ok && response.status !== 401) throw Error();
      signedOut(); notice();
    } catch { $('logout').disabled = false; notice('Could not sign out on the server. Retry sign-out.'); }
  };
  function decision(approved) { if (approval) gateway?.respondApproval(approved, approval); clearApproval(); render('idle'); }
  $('deny').onclick = () => decision(false);
  function approveStart(e) {
    if (e.type === 'keydown' && (![' ', 'Enter'].includes(e.key) || e.repeat)) return;
    e.preventDefault(); clearTimeout(approvalTimer); approvalTimer = setTimeout(() => decision(true), 1500);
  }
  $('approve').addEventListener('pointerdown', approveStart); $('approve').addEventListener('keydown', approveStart);
  for (const event of ['pointerup', 'pointerleave', 'pointercancel', 'keyup', 'blur']) $('approve').addEventListener(event, () => clearTimeout(approvalTimer));
  $('approval').addEventListener('cancel', () => decision(false));
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) { if (handsFree) notice('Hands-free ended because the page was hidden.'); halt(); }
    reduceMotion();
  });
  // Focus loss can swallow the release of a held talk button; hands-free and replies continue while visible.
  window.addEventListener('blur', () => { if (held) halt(); }); window.addEventListener('pagehide', () => { halt(); gateway?.disconnect(); });
  window.addEventListener('keydown', e => { if (e.key === 'Escape') halt(); });
  function clock() { $('clock').textContent = new Intl.DateTimeFormat(undefined, { dateStyle: 'medium', timeStyle: 'short', timeZone: 'America/New_York' }).format(new Date()); }
  clock(); setInterval(clock, 60000);
  const samples = new Float32Array(256);
  function scheduleAvatar() {
    if (!document.hidden && !animationFrame) animationFrame = requestAnimationFrame(animate);
  }
  function animate(now) {
    animationFrame = 0;
    if (document.hidden) return;
    const enabled = !motion.matches && !$('reduce-motion').checked;
    let rms = null;
    if (enabled && audio?.playing && audio.analyser) {
      audio.analyser.getFloatTimeDomainData(samples);
      rms = Math.sqrt(samples.reduce((sum, x) => sum + x * x, 0) / samples.length);
    }
    portrait.update(state, rms, now, enabled);
    if (rms !== null) scheduleAvatar();
  }
  $('reduce-motion').checked = read('zenti-reduce-motion', '0') === '1';
  $('voice-replies').checked = read('zenti-voice-replies', '1') === '1';
  $('volume').value = Math.max(0, Math.min(100, Number(read('zenti-volume-level', 40)) || 0));
  showVolume(Number($('volume').value));
  avatar(read('zenti-avatar', 'zenti')); reduceMotion(); render();
  // Remove legacy enrollment without reading, logging or transmitting it.
  try { sessionStorage.removeItem('zenti-browser-enrollment'); } catch {}
  try { localStorage.removeItem('zenti-credential'); localStorage.removeItem('zenti-secret'); } catch {}
  signedOut();
  const restoreGeneration = loginGeneration;
  fetch('/api/session', { credentials: 'same-origin', cache: 'no-store' }).then(async response => {
    if (response.ok) {
      const info = await response.json();
      if (restoreGeneration === loginGeneration) signedIn(info);
    }
  }).catch(() => {});

  if ('serviceWorker' in navigator) navigator.serviceWorker.register('./sw.js').catch(() => {});
})();
