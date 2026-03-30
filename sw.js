const CACHE = 'zerodot-v6';
const ASSETS = [
  '/ZeroDot-app/',
  '/ZeroDot-app/index.html',
  '/ZeroDot-app/manifest.json',
  '/ZeroDot-app/icon-192.png',
  '/ZeroDot-app/icon-512.png',
];

self.addEventListener('install', e => {
  e.waitUntil(
    caches.open(CACHE).then(c => c.addAll(ASSETS)).then(() => self.skipWaiting())
  );
});

self.addEventListener('activate', e => {
  e.waitUntil(
    caches.keys().then(keys =>
      Promise.all(keys.filter(k => k !== CACHE).map(k => caches.delete(k)))
    ).then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', e => {
  // Network first per API, cache first per assets
  if (e.request.url.includes('api.emailjs') || e.request.url.includes('api.anthropic')) {
    return; // bypass service worker per chiamate API
  }
  e.respondWith(
    caches.match(e.request).then(cached => {
      const network = fetch(e.request).then(res => {
        if (res.ok) {
          const clone = res.clone();
          caches.open(CACHE).then(c => c.put(e.request, clone));
        }
        return res;
      }).catch(() => cached);
      return cached || network;
    })
  );
});
