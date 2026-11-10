'use strict';
(() => {
  const $ = id => document.getElementById(id);
  let timer = null, variant = 'zenti';
  const pose = state => { $('portrait').src = `avatars/${variant}-${state}.png`; };
  function stop() { clearTimeout(timer); timer = null; $('stop').hidden = true; $('send').hidden = false; $('state').textContent = 'Ready when you are'; pose('idle'); }
  function add(role, text) { const li = document.createElement('li'); li.className = role; li.textContent = text; $('messages').append(li); while ($('messages').children.length > 32) $('messages').firstElementChild.remove(); li.scrollIntoView({block:'nearest'}); }
  function answer(text) {
    if (/private|privacy|data|record/i.test(text)) return 'This preview never records or sends anything. In the full app, microphone capture starts only when you ask. Your gateway keeps bounded conversation context in RAM. Reset clears the active context; remote providers are an explicit opt-in.';
    if (/local|run|install|setup/i.test(text)) return 'Use the repository’s Compose quick start: create private sign-in settings, trust your local TLS certificate, select a local OpenAI-compatible chat model, and supply speech models whose licenses you have reviewed. No account is needed.';
    if (/puck|hardware|desk/i.test(text)) return 'The reference puck uses a round ESP32-S3 display. It supports hold-to-talk, cancellation, menu and volume controls. Public hardware source remains experimental while charging, signed updates, recovery and soak acceptance are open.';
    return 'This is a scripted preview, so I’m not using an AI model here. Try “What stays private?”, “Run locally”, or “Meet the puck”. The full app connects to a model you choose.';
  }
  function send(text) { text = text.trim().slice(0,3000); if (!text) return; stop(); add('user',text); $('message').value = ''; $('state').textContent = 'Thinking · scripted preview'; pose('thinking'); $('send').hidden = true; $('stop').hidden = false; timer = setTimeout(() => { add('assistant',answer(text)); stop(); }, 700); }
  $('composer').addEventListener('submit', event => { event.preventDefault(); send($('message').value); });
  document.querySelectorAll('[data-prompt]').forEach(button => button.addEventListener('click', () => send(button.dataset.prompt)));
  $('stop').addEventListener('click', () => { stop(); $('message').focus(); });
  $('reset').addEventListener('click', () => { stop(); $('messages').replaceChildren(); add('assistant','A fresh start. Try an example question below.'); $('message').value = ''; });
  $('avatar').addEventListener('click', () => { variant = variant === 'zenti' ? 'orbit' : 'zenti'; $('portrait').alt = variant === 'zenti' ? 'Zenti geometric companion' : 'Orbit geometric companion'; $('demo-title').textContent = variant === 'zenti' ? 'Meet Zenti' : 'Meet Orbit'; pose(timer ? 'thinking' : 'idle'); });
})();
