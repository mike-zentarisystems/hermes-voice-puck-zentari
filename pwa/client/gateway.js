/* M2 authentication and bounded M3a JSON/PCM turns. No audio replay. */
(() => {
  const PCM = Object.freeze({ encoding: 'pcm_s16le', sample_rate: 16000, channels: 1 });
  const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
  const utf8 = new TextEncoder();
  const b64 = bytes => btoa(String.fromCharCode(...bytes));
  function decode(value, max) {
    if (typeof value !== 'string' || value.length > Math.ceil(max / 3) * 4 ||
        !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(value)) throw Error('invalid_base64');
    const raw = atob(value);
    if (!raw.length || raw.length > max) throw Error('invalid_base64');
    return Uint8Array.from(raw, c => c.charCodeAt(0));
  }
  function credentialValid(url, credential) {
    const target = new URL(url);
    if (target.protocol !== 'wss:' || target.username || target.password || target.hash ||
        !/^[a-z0-9-]{8,64}$/.test(credential.device_id) ||
        !Number.isSafeInteger(credential.generation) || credential.generation < 1 ||
        typeof credential.secret !== 'string' || !credential.secret.startsWith('base64:') ||
        decode(credential.secret.slice(7), 512).length < 32) throw Error('invalid_credential');
    if (credential.gateway_origin && new URL(credential.gateway_origin).href !== target.href) throw Error('credential_origin_mismatch');
    if (credential.expires_at !== undefined && (!Number.isSafeInteger(credential.expires_at) || credential.expires_at * 1000 <= Date.now())) throw Error('credential_expired');
    return true;
  }
  class GatewayClient extends EventTarget {
    constructor(url, credential, bridge = false) {
      super(); this.url = url; this.credential = { ...credential }; this.bridge = bridge;
      this.ws = null; this.sessionId = null; this.helloId = null;
      this._authed = false; this._stopped = true; this._reconnectDelay = 1000;
      this._reconnectTimer = null; this._helloTimer = null; this._sessionTimer = null;
      this._turnTimer = null; this._challengeId = null; this._proofSent = false;
      this.conversationId = null; this.principal = null; this.contextReady = false; this.pendingContext = null;
      this.inputSeq = 0; this.outputSeq = 0; this.turn = null; this.retired = new Set();
    }
    get ready() { return this._authed && this.ws?.readyState === WebSocket.OPEN; }
    connect() {
      try {
        if (this.bridge) {
          const target = new URL(this.url);
          if (location.protocol !== 'https:' || target.origin !== location.origin.replace(/^https:/, 'wss:') ||
              target.pathname !== '/gateway' || target.search || target.hash ||
              target.username || target.password || !/^[a-z0-9-]{8,64}$/.test(this.credential.device_id)) throw Error('bridge_origin_invalid');
        } else credentialValid(this.url, this.credential);
      }
      catch (err) { this._emit('state', { state: 'error', detail: err.message }); return false; }
      this._stopped = false; clearTimeout(this._reconnectTimer);
      if (this.ws && this.ws.readyState < 2) return true;
      this._emit('state', { state: 'connecting' });
      let socket;
      try { socket = new WebSocket(this.url, 'zentari.voice.v1'); }
      catch { this._emit('state', { state: 'error', detail: 'connection_failed' }); return false; }
      this.ws = socket; socket.binaryType = 'arraybuffer';
      socket.onopen = () => {
        if (this.ws !== socket || this._stopped) return;
        if (socket.protocol !== 'zentari.voice.v1') { this._fail('subprotocol_invalid'); return; }
        this.helloId = crypto.randomUUID(); this._challengeId = null; this._proofSent = false;
        this._sendJson(this.bridge ? { v: 1, type: 'device.hello', id: this.helloId, firmware: '0.3.0' } :
          { v: 1, type: 'device.hello', id: this.helloId,
            device_id: this.credential.device_id, board_id: 'pwa-browser-v1', firmware: '0.2.0' });
        this._helloTimer = setTimeout(() => this._fail('auth_timeout'), 10000);
        this._emit('state', { state: 'connected' });
      };
      socket.onmessage = ev => { if (this.ws === socket && !this._stopped) this._onMessage(ev); };
      socket.onclose = ev => { if (this.ws === socket) this._onClose(ev); };
      socket.onerror = () => { if (this.ws === socket) this._emit('state', { state: 'error', detail: 'connection_failed' }); };
      return true;
    }
    async _onChallenge(msg) {
      const socket = this.ws, hello = this.helloId;
      if (this._authed || this._challengeId || msg.v !== 1 || msg.hello_id !== hello ||
          !UUID.test(msg.challenge_id) || typeof msg.nonce !== 'string' ||
          !/^[A-Za-z0-9_-]{43,64}$/.test(msg.nonce) || !Number.isSafeInteger(msg.expires_ms) ||
          msg.expires_ms <= Date.now()) { this._fail('challenge_invalid'); return; }
      this._challengeId = msg.challenge_id;
      try {
        const key = await crypto.subtle.importKey('raw', decode(this.credential.secret.slice(7), 512),
          { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
        const canonical = `zv1\n${msg.challenge_id}\n${msg.nonce}\n${this.credential.device_id}\n${this.credential.generation}\n${hello}`;
        const sig = await crypto.subtle.sign('HMAC', key, utf8.encode(canonical));
        if (this.ws !== socket || this._stopped || socket.readyState !== WebSocket.OPEN) return;
        if (msg.expires_ms <= Date.now()) { this._fail('challenge_expired'); return; }
        this._proofSent = this._sendJson({ v: 1, type: 'auth.prove', challenge_id: msg.challenge_id,
          hello_id: hello, device_id: this.credential.device_id, generation: this.credential.generation,
          proof: b64(new Uint8Array(sig)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '') });
      } catch { if (this.ws === socket) this._fail('auth_failed'); }
    }
    _onMessage(ev) {
      if (typeof ev.data !== 'string' || utf8.encode(ev.data).length > 4096) { this._fail('frame_invalid'); return; }
      let msg;
      try { msg = JSON.parse(ev.data); } catch { this._fail('frame_invalid'); return; }
      if (!msg || typeof msg !== 'object' || Array.isArray(msg)) { this._fail('frame_invalid'); return; }
      this._onControl(msg);
    }
    _onControl(msg) {
      if (msg.type === 'auth.challenge') { if (this.bridge) this._fail('bridge_protocol_invalid'); else void this._onChallenge(msg); return; }
      if (msg.type === 'session.rejected') { this._fail('session_rejected'); return; }
      if (msg.type === 'session.accepted') {
        if (this._authed || (!this.bridge && !this._proofSent) || msg.v !== 1 || msg.correlation_id !== this.helloId ||
            msg.device_id !== this.credential.device_id || !UUID.test(msg.session_id) ||
            !Number.isSafeInteger(msg.expires_ms) || msg.expires_ms <= Date.now()) { this._fail('session_invalid'); return; }
        clearTimeout(this._helloTimer);
        this.sessionId = msg.session_id; this._authed = true; this._reconnectDelay = 1000;
        this.inputSeq = 0; this.outputSeq = 0; this.retired.clear();
        this._sessionTimer = setTimeout(() => {
          this._clearSession(); if (this.ws) this.ws.close(1000, 'session expired');
        }, Math.min(msg.expires_ms - Date.now(), (this.credential.expires_at ? this.credential.expires_at * 1000 - Date.now() : Infinity), 2147483647));
        this._emit('state', { state: 'ready', sessionId: this.sessionId });
        if (this.useContext) this.openConversation(); return;
      }
      if (!this.ready) return;
      if (msg.type === 'approval.request') { if (msg.v === 1) this._emit('approval', msg); return; }
      this._voice(msg);
    }
    _voice(msg) {
      if (msg.type === 'conversation.ready') {
        if (msg.v !== 1 || msg.device_id !== this.credential.device_id || msg.session_id !== this.sessionId ||
            msg.correlation_id !== this.pendingContext || msg.seq !== this.outputSeq + 1 ||
            !UUID.test(msg.conversation_id) || typeof msg.principal !== 'string' ||
            !Array.isArray(msg.capabilities) || !msg.capabilities.includes('text')) { this._fail('context_invalid'); return; }
        this.outputSeq = msg.seq; this.pendingContext = null;
        const same = this.principal === msg.principal && this.conversationId === msg.conversation_id;
        this.principal = msg.principal; this.conversationId = msg.conversation_id; this.contextReady = true;
        this._emit('conversation', { same, principal: this.principal, id: this.conversationId }); return;
      }
      if (msg.type === 'error' && this.pendingContext && msg.correlation_id === this.pendingContext &&
          msg.device_id === this.credential.device_id && msg.session_id === this.sessionId && msg.seq === this.outputSeq + 1) {
        this.outputSeq = msg.seq; this.pendingContext = null; this.contextReady = false;
        this.conversationId = null; this.principal = null;
        this._emit('conversation', { same: false, error: 'Conversation unavailable. Start a new conversation.' }); return;
      }
      const extras = { 'transcript.final': 'text', 'assistant.thinking': null, 'assistant.text': 'text',
        'audio.output.start': 'format', 'audio.output.chunk': 'data', 'audio.output.stop': null, error: 'code' };
      const keys = ['v', 'type', 'device_id', 'session_id', 'correlation_id', 'seq'];
      if (!Object.hasOwn(extras, msg.type) || msg.v !== 1 || msg.device_id !== this.credential.device_id ||
          msg.session_id !== this.sessionId || !UUID.test(msg.correlation_id) ||
          !Number.isSafeInteger(msg.seq) || msg.seq !== this.outputSeq + 1 ||
          Object.keys(msg).sort().join() !== [...keys, ...(extras[msg.type] ? [extras[msg.type]] : [])].sort().join()) {
        this._fail('event_invalid'); return;
      }
      this.outputSeq = msg.seq;
      // Cancelled events consume the session sequence but can never play.
      if (this.retired.has(msg.correlation_id)) return;
      const turn = this.turn;
      if (!turn || msg.correlation_id !== turn.id) { this._fail('turn_stale'); return; }
      const bad = code => this.cancel(code);
      switch (msg.type) {
        case 'error':
          if (typeof msg.code !== 'string' || !/^[a-z0-9_]{1,48}$/.test(msg.code)) { bad('error_invalid'); return; }
          this._endTurn(msg.code); return;
        case 'transcript.final': case 'assistant.text':
          if (turn.state !== 'thinking' || typeof msg.text !== 'string' || !msg.text || utf8.encode(msg.text).length > 4096) { bad('text_invalid'); return; }
          this._emit('text', { type: msg.type, text: msg.text }); return;
        case 'assistant.thinking':
          if (turn.state !== 'thinking') bad('order_invalid'); return;
        case 'audio.output.start':
          if (turn.state !== 'thinking' || !msg.format || Object.keys(msg.format).sort().join() !== 'channels,encoding,sample_rate' ||
              msg.format.encoding !== PCM.encoding || msg.format.sample_rate !== 16000 || msg.format.channels !== 1) { bad('format_invalid'); return; }
          turn.state = 'speaking'; this._emit('voice', { state: 'speaking' }); this._emit('audio-start'); return;
        case 'audio.output.chunk': {
          if (turn.state !== 'speaking') { bad('order_invalid'); return; }
          let bytes;
          try { bytes = decode(msg.data, 1024); } catch { bad('audio_invalid'); return; }
          if (bytes.length % 2 || turn.outputBytes + bytes.length > 1920000) { bad('output_limit'); return; }
          turn.outputBytes += bytes.length; this._emit('audio', { data: bytes.buffer }); return;
        }
        case 'audio.output.stop':
          if (turn.state !== 'speaking' || !turn.outputBytes) { bad('output_empty'); return; }
          turn.state = 'draining'; this._emit('audio-end'); return;
      }
    }
    _sendJson(obj) {
      if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return false;
      const text = JSON.stringify(obj);
      if (utf8.encode(text).length > 4096 || (this.ws.bufferedAmount || 0) > 16384) { this._fail('transport_backpressure'); return false; }
      try { this.ws.send(text); return true; } catch { this._fail('transport_failed'); return false; }
    }
    _sendTurn(type, fields = {}) {
      if (!this.ready || !this.turn) return false;
      return this._sendJson({ v: 1, type, device_id: this.credential.device_id, session_id: this.sessionId,
        correlation_id: this.turn.id, seq: ++this.inputSeq, ...fields });
    }
    openConversation(reset = false) {
      if (!this.ready || this.turn || this.pendingContext) return false;
      this.contextReady = false; this.pendingContext = crypto.randomUUID();
      return this._sendJson({ v: 1, type: reset ? 'conversation.reset' : 'conversation.open',
        device_id: this.credential.device_id, session_id: this.sessionId,
        correlation_id: this.pendingContext, seq: ++this.inputSeq,
        ...(!reset && this.conversationId ? { conversation_id: this.conversationId } : {}) });
    }
    sendText(text) {
      text = text.trim();
      if (!this.ready || !this.contextReady || this.turn || !text || utf8.encode(text).length > 3000 || this.retired.size >= 255) return false;
      this.turn = { id: crypto.randomUUID(), state: 'thinking', inputBytes: 0, outputBytes: 0 };
      if (!this._sendTurn('text.input', { text })) { this._endTurn('send_failed'); return false; }
      this._turnTimer = setTimeout(() => this.cancel('turn_timeout'), 150000);
      this._emit('voice', { state: 'thinking' }); return true;
    }
    startCapture() {
      if (!this.ready || this.turn) return false;
      if (this.retired.size >= 255) { this._clearSession(); this.ws.close(1000, 'renew turn budget'); return false; }
      this.turn = { id: crypto.randomUUID(), state: 'listening', inputBytes: 0, outputBytes: 0 };
      if (!this._sendTurn('audio.input.start', { format: PCM })) { this._endTurn('send_failed'); return false; }
      this._turnTimer = setTimeout(() => this.cancel('turn_timeout'), 150000);
      this._emit('voice', { state: 'listening' }); return true;
    }
    sendAudio(buf) {
      if (!this.ready || this.turn?.state !== 'listening') return false;
      const bytes = new Uint8Array(buf);
      if (!bytes.length || bytes.length > 1024 || bytes.length % 2 || this.turn.inputBytes + bytes.length > 480000) {
        this.cancel('input_limit'); return false;
      }
      this.turn.inputBytes += bytes.length;
      return this._sendTurn('audio.input.chunk', { data: b64(bytes) });
    }
    stopCapture() {
      if (this.turn?.state !== 'listening') return false;
      if (!this.turn.inputBytes) { this.cancel('input_empty'); return false; }
      if (!this._sendTurn('audio.input.stop')) return false;
      this.turn.state = 'thinking'; this._emit('voice', { state: 'thinking' }); return true;
    }
    cancel(reason = 'cancelled') {
      if (this.turn && this.turn.state !== 'draining') this._sendTurn('cancel');
      this._endTurn(reason);
    }
    completePlayback() { if (this.turn?.state === 'draining') this._endTurn(); }
    _endTurn(error) {
      if (this.turn) this.retired.add(this.turn.id);
      this.turn = null; clearTimeout(this._turnTimer);
      this._emit('reset', { error }); this._emit('voice', { state: this.ready ? 'idle' : 'offline', error });
    }
    _clearSession() {
      this._authed = false; this.sessionId = null; this._proofSent = false; this.contextReady = false; this.pendingContext = null;
      clearTimeout(this._helloTimer); clearTimeout(this._sessionTimer); this._endTurn();
    }
    _fail(detail) {
      this._stopped = true; clearTimeout(this._reconnectTimer); this._clearSession();
      this._emit('state', { state: 'error', detail }); if (this.ws) this.ws.close(4008, 'client rejected session');
    }
    _onClose(ev) {
      this._clearSession(); this._emit('state', { state: 'disconnected', code: ev.code });
      if (this._stopped) return;
      const delay = Math.min(30000, this._reconnectDelay * (0.5 + Math.random()));
      this._reconnectDelay = Math.min(30000, this._reconnectDelay * 2);
      this._reconnectTimer = setTimeout(() => this.connect(), delay);
    }
    respondApproval(approved, approvalId) {
      if (this.bridge || !this.ready || !UUID.test(approvalId) || (this.credential.scope && !this.credential.scope.includes('approval:respond'))) return false;
      return this._sendJson({ v: 1, type: 'approval.response', approval_id: approvalId,
        decision: approved ? 'approved' : 'denied', device_id: this.credential.device_id,
        decided_at: Math.floor(Date.now() / 1000) });
    }
    _emit(type, detail) { this.dispatchEvent(new CustomEvent(type, { detail })); }
    disconnect() {
      this._stopped = true; clearTimeout(this._reconnectTimer); this.cancel(); this._clearSession();
      if (this.ws) this.ws.close(1000, 'client disconnect');
    }
  }
  window.GatewayClient = GatewayClient; window.validateVoiceCredential = credentialValid;
})();
