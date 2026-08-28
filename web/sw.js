// Caches the page itself so it opens instantly, even on bad signal in the
// stairwell. It deliberately does NOT cache the occupancy number -- a cached
// count is a lie, and someone acting on a stale "quiet" walks into a packed gym.

const SHELL = "cougar-count-shell-v1";

// FIX 2026-08-28 (pass 5): fetch() never times out on its own. Network-first
// with no limit means that on the kind of signal you get in a stairwell -- a
// connection that accepts the request and then says nothing -- the page hangs
// instead of loading, even though a perfectly good copy is sitting in the cache
// a few millimetres away. Four seconds, then use what we have.
const NETWORK_TIMEOUT_MS = 4000;

function fromNetwork(request) {
  return new Promise((resolve, reject) => {
    const giveUp = setTimeout(() => reject(new Error("network timeout")), NETWORK_TIMEOUT_MS);
    fetch(request).then(
      (res) => { clearTimeout(giveUp); resolve(res); },
      (err) => { clearTimeout(giveUp); reject(err); }
    );
  });
}
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
    fromNetwork(e.request)
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
      .catch(() =>
        caches
          .match(e.request)
          .then((r) => r || caches.match("index.html"))
          // FIX 2026-08-28 (pass 6): on a first visit with no network there is
          // nothing cached, so this resolved to undefined -- and respondWith
          // (undefined) throws a TypeError, which the browser renders as a
          // generic network failure with no clue what happened. Answer properly.
          .then((r) =>
            r ||
            new Response("Offline, and this page has not been cached yet.", {
              status: 503,
              headers: { "Content-Type": "text/plain" },
            })
          )
      )
  );
});
