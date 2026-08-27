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

  const get = (t) => parts.find((p) => p.type === t).value;
  const hour = parseInt(get("hour"), 10) % 24;
  const minute = parseInt(get("minute"), 10);
  const weekend = ["Sat", "Sun"].includes(get("weekday"));
  return { hour, minute, weekend, decimal: hour + minute / 60 };
}

function isOpen(t = reginaNow()) {
  const [open, close] = t.weekend ? CONFIG.hours.weekend : CONFIG.hours.weekday;
  return t.decimal >= open && t.decimal < close;
}

// --- data ------------------------------------------------------------------

async function fetchLive() {
  const res = await fetch(`https://${CONFIG.host}/gym/live.json`, { cache: "no-store" });
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
  const share = occupancy / capacity;
  if (share < 0.25) return { word: "Quiet", level: "quiet" };
  if (share < 0.55) return { word: "Steady", level: "steady" };
  if (share < 0.8) return { word: "Busy", level: "busy" };
  return { word: "Packed", level: "packed" };
}

function ago(ms) {
  if (!ms) return "";
  const secs = Math.round((Date.now() - ms) / 1000);
  if (secs < 90) return "updated just now";
  const mins = Math.round(secs / 60);
  if (mins < 60) return `updated ${mins} min ago`;
  return "not reporting";
}

const el = (id) => document.getElementById(id);

function render(data) {
  const open = isOpen();
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
  el("fill").style.width = `${Math.min(100, (data.occupancy / data.capacity) * 100)}%`;
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

async function update() {
  if (inFlight) return;
  inFlight = true;
  try {
    render(CONFIG.host ? await fetchLive() : demoLive());
  } catch (err) {
    // Never show a stale number as if it were current. A wrong number is worse
    // than no number -- someone walks over to a packed gym believing it is empty.
    showProblem("The counter isn't reporting. Try again in a minute.");
    console.error(err);
  } finally {
    inFlight = false;
  }
}

update();
setInterval(update, CONFIG.pollMs);
document.addEventListener("visibilitychange", () => !document.hidden && update());

if ("serviceWorker" in navigator) {
  navigator.serviceWorker.register("sw.js").catch(() => {});
}
