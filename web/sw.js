// Caches the page itself so it opens instantly, even on bad signal in the
// stairwell. It deliberately does NOT cache the occupancy number -- a cached
// count is a lie, and someone acting on a stale "quiet" walks into a packed gym.

const SHELL = "cougar-count-shell-v1";
const FILES = ["./", "index.html", "styles.css", "app.js", "manifest.json"];

self.addEventListener("install", (e) => {
  e.waitUntil(caches.open(SHELL).then((c) => c.addAll(FILES)).then(() => self.skipWaiting()));
});

self.addEventListener("activate", (e) => {
  e.waitUntil(
    caches.keys()
      .then((keys) => Promise.all(keys.filter((k) => k !== SHELL).map((k) => caches.delete(k))))
      .then(() => self.clients.claim())
  );
});

self.addEventListener("fetch", (e) => {
  const url = new URL(e.request.url);

  // Anything that is not this page's own files goes straight to the network.
  if (url.origin !== self.location.origin) return;

  // FIX 2026-08-27: cache.put() throws on anything that is not a GET, and the
  // rejection surfaced as a failed request rather than a cache miss. Nothing
  // here POSTs today, but a form or an analytics beacon added later would have
  // broken in a way that pointed at the wrong file entirely.
  if (e.request.method !== "GET") return;

  e.respondWith(
    fetch(e.request)
      .then((res) => {
        // FIX 2026-08-27: every response was cached, including 404s. The two
        // missing icons got stored as failures and kept being served from the
        // cache after the real files were added.
        if (res.ok) {
          const copy = res.clone();
          caches.open(SHELL).then((c) => c.put(e.request, copy));
        }
        return res;
      })
      .catch(() => caches.match(e.request).then((r) => r || caches.match("index.html")))
  );
});
