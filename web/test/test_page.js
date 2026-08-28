// Tests for the page logic. No browser, no npm, no node -- macOS ships a
// JavaScript engine, and these run against it. From the repo root:
//
//   /System/Library/Frameworks/JavaScriptCore.framework/Versions/A/Helpers/jsc \
//     web/test/test_page.js
//
// Minimal stubs so web/app.js can be loaded outside a browser.
var els = {};
function mkEl() { return { textContent: "", style: {} }; }
["count","word","detail","fill","stamp"].forEach(id => els[id] = mkEl());

var document = {
  body: { dataset: {} },
  hidden: false,
  getElementById: id => els[id] || null,
  addEventListener: () => {},
};
var navigator = {};
var console = { warn: () => {}, error: () => {} };
var intervals = 0;
function setInterval() { intervals++; return intervals; }
function clearInterval() { intervals--; }
function setTimeout() { return 0; }
function clearTimeout() {}
function AbortController() { this.signal = {}; this.abort = function () {}; }

// Swappable so a test can make the network hang or fail.
var fetchImpl = () => Promise.reject(new Error("no network in harness"));
function fetch() { return fetchImpl.apply(null, arguments); }

load("web/app.js");   // run from the repo root

var failures = 0;
function check(ok, what) {
  print((ok ? "  ok    " : "FAILED  ") + what);
  if (!ok) failures++;
}

// Force the gym open so the staleness path is reachable regardless of the
// real time in Regina when this runs.
CONFIG.hours = { weekday: [0, 24], weekend: [0, 24] };

// --- isStale ---------------------------------------------------------------
check(isStale({ updatedAt: Date.now() }) === false,                 "a just-written number is fresh");
check(isStale({ updatedAt: Date.now() - 5 * 60 * 1000 }) === false, "five minutes old is still fresh");
check(isStale({ updatedAt: Date.now() - 7 * 60 * 1000 }) === true,  "seven minutes old is stale");
check(isStale({ updatedAt: null }) === true,                        "no timestamp at all is stale");
check(isStale({ updatedAt: undefined }) === true,                   "a missing timestamp is stale");
check(isStale({ updatedAt: Date.now() + 60 * 1000 }) === false,     "a future timestamp is skew, not death");
check(isStale({ demo: true, updatedAt: 0 }) === false,              "demo data is never stale");

// --- describe --------------------------------------------------------------
check(describe(10, 150).word === "Quiet",    "10 of 150 is quiet");
check(describe(60, 150).word === "Steady",   "60 of 150 is steady");
check(describe(100, 150).word === "Busy",    "100 of 150 is busy");
check(describe(140, 150).word === "Packed",  "140 of 150 is packed");
check(describe(10, 0).word === "Unknown",    "a capacity of zero does not silently say Packed");
check(describe(10, 0).level === "unknown",   "and it styles as unknown");

// --- render refuses to paint a stale number --------------------------------
render({ occupancy: 12, capacity: 150, updatedAt: Date.now() - 60 * 60 * 1000 });
check(els.count.textContent !== 12,            "a stale count is not displayed");
check(els.count.textContent === "—",      "it shows a dash instead");
check(/out of date/.test(els.detail.textContent), "and says why: " + els.detail.textContent);

render({ occupancy: 12, capacity: 150, updatedAt: Date.now() });
check(els.count.textContent === 12,            "a fresh count IS displayed");
check(els.stamp.textContent === "updated just now", "with a freshness stamp");

// --- ago -------------------------------------------------------------------
check(ago(Date.now()) === "updated just now",              "ago: just now");
check(ago(Date.now() - 4 * 60 * 1000) === "updated 4 min ago", "ago: minutes");
check(ago(null) === "",                                    "ago: no timestamp is blank");
check(ago(Date.now() + 10000) === "updated just now",      "ago: never reports negative time");

// --- pass 3 regressions ----------------------------------------------------
render({ occupancy: 10, capacity: 0, updatedAt: Date.now() });
check(els.fill.style.width === "0%",            "a capacity of zero gives a zero-width bar, not Infinity%");
check(els.count.textContent === 10,             "the count still shows with an unknown capacity");

// A bar that was wide must not stay wide when the next render has no capacity.
render({ occupancy: 140, capacity: 150, updatedAt: Date.now() });
check(els.fill.style.width === "93.33333333333333%", "a full-ish bar renders wide");
render({ occupancy: 10, capacity: 0, updatedAt: Date.now() });
check(els.fill.style.width === "0%",            "and collapses rather than keeping the old width");

// Over capacity must not overflow the bar.
render({ occupancy: 400, capacity: 150, updatedAt: Date.now() });
check(els.fill.style.width === "100%",          "over capacity clamps to 100%");

// --- pass 4: a hung request must not wedge the poller ----------------------
// A fetch that never settles used to leave inFlight stuck true, so every later
// poll returned immediately and the page sat on its last number forever.
CONFIG.host = "example.invalid";

fetchImpl = () => new Promise(() => {});      // never settles
update();
drainMicrotasks();
check(inFlight === true, "a request in flight blocks a second one");

// The real abort comes from a timer the harness cannot run, so simulate the
// settle the timeout would cause and confirm the poller frees itself.
fetchImpl = () => Promise.reject(new Error("aborted"));
inFlight = false;
update();
drainMicrotasks();
check(inFlight === false,                   "a failed request releases the poller");
check(els.count.textContent === "\u2014",  "and the page stops showing a number");

// A recovered network renders again rather than staying stuck on the error.
fetchImpl = () => Promise.resolve({
  ok: true,
  json: () => Promise.resolve({ in_total: 30, out_total: 12, capacity: 150, updated_at: Date.now() }),
});
update();
drainMicrotasks();
check(els.count.textContent === 18,         "the page recovers when the network comes back");

CONFIG.host = null;

print("");
print(failures ? "SOME CHECKS FAILED" : "all checks passed");
