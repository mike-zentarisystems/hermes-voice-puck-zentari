/* Zenti Voice PWA — service worker for the offline shell.
 *
 * The asset list is the whole contract for offline use: `cache.addAll` rejects
 * the entire install if any single entry 404s, which previously meant this
 * worker never installed at all. Each asset is therefore added individually so
 * one missing file degrades to "that file is not cached" instead of "the app
 * has no offline copy".
 */
const CACHE = 'zenti-public-v1-neutral-avatar';

const SHELL = [
  './',
  './index.html',
  './app.css',
  './app.js',
  './avatar.js',
  './message.js',
  './avatars/zenti.png',
  './avatars/orbit.png',
  './avatars/zenti-idle.png',
  './avatars/zenti-listening.png',
  './avatars/zenti-thinking.png',
  './avatars/zenti-working.png',
  './avatars/zenti-speaking.png',
  './avatars/zenti-success.png',
  './avatars/zenti-error.png',
  './avatars/zenti-approval.png',
  './avatars/orbit-idle.png',
  './avatars/orbit-listening.png',
  './avatars/orbit-thinking.png',
  './avatars/orbit-working.png',
  './avatars/orbit-speaking.png',
  './avatars/orbit-success.png',
  './avatars/orbit-error.png',
  './avatars/orbit-approval.png',
  './gateway.js',
  './audio.js',
  './capture-worklet.js',
  './playback-worklet.js',
  './voice.js',
  './shell.js',
  './manifest.json',
  './icons/icon-192.png',
  './icons/icon-512.png',
];

self.addEventListener('install', e => {
  e.waitUntil((async () => {
    const cache = await caches.open(CACHE);
    await Promise.all(SHELL.map(async (url) => {
      try {
        await cache.add(new Request(url, { cache: 'reload' }));
      } catch (err) {
        console.warn('[zenti-sw] not cached:', url, err);
      }
    }));
    await self.skipWaiting();
  })());
});

self.addEventListener('activate', e => {
  e.waitUntil((async () => {
    const keys = await caches.keys();
    await Promise.all(keys.filter(k => k.startsWith('zenti-public-') && k !== CACHE).map(k => caches.delete(k)));
    await self.clients.claim();
  })());
});

self.addEventListener('fetch', e => {
  const req = e.request;
  if (req.method !== 'GET') return;
  // Never cache gateway traffic; a stale control frame is worse than none.
  if (req.url.includes('/gateway') || req.url.includes('/api/')) return;
  if (new URL(req.url).origin !== self.location.origin) return;
  const scope = self.registration?.scope || self.location.origin + '/';
  if (!SHELL.some(path => new URL(path, scope).href === req.url)) return;

  /* Stale-while-revalidate, not cache-first. Plain cache-first means a
   * corrected shell is never delivered until someone remembers to bump CACHE,
   * which is how a bug fix silently fails to reach installed clients. Here the
   * cached copy is served immediately and the network copy replaces it for the
   * next load, so offline stays instant and updates still land. */
  // Register lifetime work synchronously, and include cache.put completion.
  const cachePromise = caches.open(CACHE);
  const network = fetch(new Request(req, { cache: 'no-cache' })).then(async (fresh) => {
    if (fresh && fresh.status === 200 && fresh.type === 'basic') {
      try {
        const cache = await cachePromise;
        await cache.put(req, fresh.clone());
      } catch (err) {
        console.warn('[zenti-sw] refresh not cached:', req.url, err);
      }
    }
    return fresh;
  }).catch(() => null);
  e.waitUntil(network.then(() => undefined));
  e.respondWith((async () => {
    const cache = await cachePromise;
    const cached = await cache.match(req, { ignoreSearch: true });
    if (cached) return cached;
    const fresh = await network;
    if (fresh) return fresh;
    if (req.mode === 'navigate') {
      const shell = await cache.match('./index.html');
      if (shell) return shell;
    }
    return new Response('offline', { status: 503, statusText: 'offline' });
  })());

});
