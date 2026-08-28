// Cougar Count -- the page students actually open.
//
// No build step, no framework, no dependencies. It fetches two numbers and
// subtracts them. Everything else here is presentation and honesty about
// whether the number can be trusted right now.

const CONFIG = {
  // Set this once the Firebase project exists, e.g.
  //   "cougar-count-default-rtdb.firebaseio.com"
  // While it is null the page runs on generated data so it can be built and
  // looked at before any hardware or backend is real.
  host: null,

  capacity: 150,
  pollMs: 30_000,

  // Well under pollMs, so a hung request can never outlive its own poll cycle.
  timeoutMs: 10_000,

  // FIX 2026-08-28: a number older than this is not shown AT ALL.
  //
  // Previously a dead board still painted its last count in huge type with a
  // small "not reporting" note underneath. A board that died at 2pm showed 2pm's
  // number all evening, and someone reading "12 - Quiet" walked across campus to
  // a packed gym. Failing confidently and wrongly is worse than failing visibly.
  //
  // This only works if the boards write updated_at on a timer and not merely on
  // a crossing -- otherwise an empty gym at 6am looks identical to a dead board.
  // That contract is written down in docs/data-model.md and the firmware has to
  // honour it. Six minutes is three missed two-minute heartbeats.
  staleAfterMs: 6 * 60 * 1000,

  // UNCONFIRMED -- guessed, needs checking with the FLC front desk.
  hours: { weekday: [6, 22], weekend: [8, 20] },
};

// --- Regina time -----------------------------------------------------------
// The gym's hours are in Saskatchewan time, and whoever is looking at this page
// may not be. Saskatchewan does not observe DST, but never assume that -- ask
// the browser for the real answer.

function reginaNow() {
  const parts = new Intl.DateTimeFormat("en-CA", {
    timeZone: "America/Regina",
    hour12: false,
    weekday: "short",
    hour: "2-digit",
    minute: "2-digit",
  }).formatToParts(new Date());

  // FIX 2026-08-28: this was `parts.find(...).value`, which throws if a browser
  // does not emit a part. The throw was caught by update() and shown to the user
  // as "the counter isn't reporting" -- a clock problem reported as a counter
  // problem, which would have sent someone hunting the wrong thing entirely.
  const get = (t, fallback) => {
    const part = parts.find((p) => p.type === t);
    return part ? part.value : fallback;
  };

  const hour = parseInt(get("hour", "12"), 10) % 24;
  const minute = parseInt(get("minute", "0"), 10);
  const weekend = ["Sat", "Sun"].includes(get("weekday", "Mon"));
  return { hour, minute, weekend, decimal: hour + minute / 60 };
}

function isOpen(t = reginaNow()) {
  const [open, close] = t.weekend ? CONFIG.hours.weekend : CONFIG.hours.weekday;
  return t.decimal >= open && t.decimal < close;
}

// --- data ------------------------------------------------------------------

async function fetchLive() {
  // FIX 2026-08-28 (pass 4): fetch() has no timeout of its own. A request that
  // hangs -- flaky campus wifi, a captive portal swallowing traffic -- would
  // never settle, so `inFlight` never cleared and every future poll returned
  // immediately. The page would sit on its last number forever with nothing
  // running to age it out. That is the stale-number disaster again, arriving by
  // a different route, so it gets the same answer: give up and say so.
  const abort = new AbortController();
  const bail = setTimeout(() => abort.abort(), CONFIG.timeoutMs);

  let res;
  try {
    res = await fetch(`https://${CONFIG.host}/gym/live.json`, {
      cache: "no-store",
      signal: abort.signal,
    });
  } finally {
    clearTimeout(bail);
  }

  if (!res.ok) throw new Error(`database returned ${res.status}`);
  const d = await res.json();
  if (!d || typeof d.in_total !== "number" || typeof d.out_total !== "number") {
    throw new Error("unexpected shape");
  }
  // FIX 2026-08-27: `d.capacity || CONFIG.capacity` silently ignored a stored
  // capacity of 0 and used the hardcoded default instead. Check the type.
  const capacity = typeof d.capacity === "number" && d.capacity > 0
    ? d.capacity
    : CONFIG.capacity;

  // Clamped because a negative occupancy is meaningless to a reader, but note
  // that clamping HIDES a broken counter -- if out_total ever exceeds in_total
  // something is wrong, and the page would show a calm 0. The nightly residual
  // is what actually catches that; this is only about not showing nonsense.
  const raw = d.in_total - d.out_total;
  if (raw < 0) console.warn("occupancy is negative -- a counter is wrong", d);

  return {
    occupancy: Math.max(0, raw),
    capacity,
    updatedAt: typeof d.updated_at === "number" ? d.updated_at : null,
  };
}

// A plausible gym day, so the layout can be judged before real data exists.
// Quiet at open, a morning bump, a midday dip, and the after-class rush.
function demoLive() {
  const t = reginaNow();
  const shape = (h) =>
    35 * Math.exp(-(((h - 8.0) / 1.5) ** 2)) +
    28 * Math.exp(-(((h - 12.5) / 1.4) ** 2)) +
    95 * Math.exp(-(((h - 17.5) / 2.0) ** 2));

  const base = isOpen(t) ? shape(t.decimal) : 0;
  const wobble = Math.sin(Date.now() / 90_000) * 4;
  return {
    occupancy: Math.max(0, Math.round(base + wobble)),
    capacity: CONFIG.capacity,
    updatedAt: Date.now(),
    demo: true,
  };
}

// --- presentation ----------------------------------------------------------

function describe(occupancy, capacity) {
  // FIX 2026-08-28: fetchLive() guarded the SERVER's capacity but nothing
  // guarded CONFIG.capacity, which is hand-edited. A zero made `share` NaN,
  // every comparison below false, and the page silently answered "Packed".
  if (!(capacity > 0)) return { word: "Unknown", level: "unknown" };

  const share = occupancy / capacity;
  if (share < 0.25) return { word: "Quiet", level: "quiet" };
  if (share < 0.55) return { word: "Steady", level: "steady" };
  if (share < 0.8) return { word: "Busy", level: "busy" };
  return { word: "Packed", level: "packed" };
}

// Only ever called for data already judged fresh, so it has no "too old" case
// left -- render() refuses to display a stale number at all.
function ago(ms) {
  if (!ms) return "";
  const secs = Math.max(0, Math.round((Date.now() - ms) / 1000));
  if (secs < 90) return "updated just now";
  return `updated ${Math.round(secs / 60)} min ago`;
}

// FIX 2026-08-28: see CONFIG.staleAfterMs.
//
// Note the clock this compares against is the VIEWER's, not the server's, so a
// phone with a badly wrong clock can misjudge freshness. Six minutes is wide
// enough that ordinary drift never trips it. A timestamp in the future is
// treated as fresh rather than stale -- that is skew, not a dead board.
function isStale(data) {
  if (data.demo) return false;
  if (typeof data.updatedAt !== "number") return true;
  return Date.now() - data.updatedAt > CONFIG.staleAfterMs;
}

const el = (id) => document.getElementById(id);

function render(data) {
  const open = isOpen();

  // Closed is decided from the clock, so it is still true even with no data.
  // Staleness is only checked while open, because nothing writes overnight.
  if (open && isStale(data)) {
    showProblem("The counter hasn't reported recently, so this could be out of date.");
    return;
  }

  document.body.dataset.state = open ? describe(data.occupancy, data.capacity).level : "closed";

  if (!open) {
    el("count").textContent = "Closed";
    el("word").textContent = "";
    el("detail").textContent = nextOpening();
    el("fill").style.width = "0%";
    el("stamp").textContent = "";
    return;
  }

  const { word } = describe(data.occupancy, data.capacity);
  el("count").textContent = data.occupancy;
  el("word").textContent = word;
  el("detail").textContent = `of about ${data.capacity} people`;
  // FIX 2026-08-28 (pass 3): with capacity 0 this produced "Infinity%", which
  // browsers discard -- leaving whatever width the bar happened to have from the
  // last render. describe() already guards the wording; guard the bar too.
  const share = data.capacity > 0 ? (data.occupancy / data.capacity) * 100 : 0;
  el("fill").style.width = `${Math.min(100, share)}%`;
  el("stamp").textContent = data.demo ? "sample data — not live yet" : ago(data.updatedAt);
}

function clockLabel(hour) {
  const h = hour % 12 || 12;
  return `${h}${hour < 12 ? "am" : "pm"}`;
}

function nextOpening() {
  const t = reginaNow();
  const todayOpen = (t.weekend ? CONFIG.hours.weekend : CONFIG.hours.weekday)[0];

  // Before opening: today's own hours are the answer.
  if (t.decimal < todayOpen) return `Opens at ${clockLabel(todayOpen)}`;

  // FIX 2026-08-27: after closing, this used TODAY's opening time for tomorrow.
  // On a Friday night it promised 6am when the gym actually opens at 8am on
  // Saturday, and on a Sunday night it promised 8am for a 6am Monday. Ask what
  // tomorrow actually is.
  const tomorrowWeekend = ["Sat", "Sun"].includes(
    new Intl.DateTimeFormat("en-CA", { timeZone: "America/Regina", weekday: "short" })
      .format(new Date(Date.now() + 24 * 60 * 60 * 1000))
  );
  const tomorrowOpen = (tomorrowWeekend ? CONFIG.hours.weekend : CONFIG.hours.weekday)[0];
  return `Opens ${clockLabel(tomorrowOpen)} tomorrow`;
}

function showProblem(message) {
  el("count").textContent = "—";
  el("word").textContent = "Can't tell right now";
  el("detail").textContent = message;
  el("fill").style.width = "0%";
  el("stamp").textContent = "";
  document.body.dataset.state = "unknown";
}

// FIX 2026-08-27: switching back to the tab fires an update while the 30s timer
// may already have one in flight. Two responses could then land out of order and
// render the older one. One at a time.
let inFlight = false;

// FIX 2026-08-28 (pass 3): kept so that reopening a backgrounded tab can re-judge
// what is already on screen BEFORE the network answers. Without this, coming back
// to the app after an hour showed the hour-old number for as long as the fetch
// took -- a smaller version of exactly the bug this pass set out to remove.
let lastData = null;

async function update() {
  if (inFlight) return;
  inFlight = true;
  try {
    const data = CONFIG.host ? await fetchLive() : demoLive();
    lastData = data;
    render(data);
  } catch (err) {
    // Never show a stale number as if it were current. A wrong number is worse
    // than no number -- someone walks over to a packed gym believing it is empty.
    showProblem("The counter isn't reporting. Try again in a minute.");
    console.error(err);
  } finally {
    inFlight = false;
  }
}

// FIX 2026-08-28: this polled every 30s forever, including in tabs buried behind
// a dozen others all day. Firebase bills egress and the free tier is 10 GB a
// month; background tabs were the only part of this design that scaled with
// users, and they scaled the wrong way. Nobody is reading a hidden tab.
let timer = null;

function startPolling() {
  if (timer === null) timer = setInterval(update, CONFIG.pollMs);
}

function stopPolling() {
  if (timer !== null) {
    clearInterval(timer);
    timer = null;
  }
}

document.addEventListener("visibilitychange", () => {
  if (document.hidden) {
    stopPolling();
  } else {
    // Re-judge what is already on screen first: if it aged out while the tab was
    // hidden, it must stop being displayed now, not once the network replies.
    if (lastData) render(lastData);
    update();
    startPolling();
  }
});

update();
if (!document.hidden) startPolling();

if ("serviceWorker" in navigator) {
  navigator.serviceWorker.register("sw.js").catch(() => {});
}
