#pragma once

// The whole web interface: one page, no external resources (there is no internet on the access point)
static const char INDEX_HTML[] = R"rawliteral(<!doctype html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Гостевая книга</title>
<style>
:root {
  --bg: #f6f3ee; --card: #fff; --text: #1d1b19; --muted: #77716a; --line: #e6e0d8;
  --accent: #9b3d2e; --accent-text: #fff; --warn-bg: #fff3d6; --warn-text: #6b4e00;
  --ok: #2f7d4a; --busy: #b7791f; --off: #a33;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #171513; --card: #221f1c; --text: #eee8e1; --muted: #a39a90; --line: #35302b;
    --accent: #d9735f; --accent-text: #1b1210; --warn-bg: #3a2f14; --warn-text: #f3d68a;
    --ok: #5cbf7c; --busy: #e0a845; --off: #e46a6a;
  }
}
* { box-sizing: border-box; }
body {
  margin: 0; background: var(--bg); color: var(--text);
  font: 16px/1.4 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  padding-bottom: 110px;
}
main { max-width: 640px; margin: 0 auto; padding: 20px 16px; }
h1 { font-size: 24px; margin: 0 0 4px; }
.status { color: var(--muted); font-size: 14px; display: flex; flex-wrap: wrap; gap: 4px 12px; align-items: center; }
.dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-right: 6px; background: var(--off); }
.dot.ok { background: var(--ok); } .dot.busy { background: var(--busy); }
.warn { background: var(--warn-bg); color: var(--warn-text); border-radius: 10px; padding: 10px 12px; margin: 14px 0 0; font-size: 14px; }
.error { color: var(--muted); font-size: 12px; margin-top: 6px; overflow-wrap: anywhere; }
.zip {
  display: block; margin: 18px 0; padding: 14px; border-radius: 12px; text-align: center;
  background: var(--accent); color: var(--accent-text); text-decoration: none; font-weight: 600;
}
.zip small { display: block; font-weight: 400; opacity: .8; }
.zip.disabled { opacity: .4; pointer-events: none; }
ul { list-style: none; margin: 0; padding: 0; background: var(--card); border-radius: 12px; overflow: hidden; }
li { display: flex; align-items: center; gap: 12px; padding: 10px 12px; border-top: 1px solid var(--line); }
li:first-child { border-top: 0; }
li.playing { background: color-mix(in srgb, var(--accent) 12%, var(--card)); }
.play {
  flex: none; width: 40px; height: 40px; border-radius: 50%; border: 0; cursor: pointer;
  background: var(--accent); color: var(--accent-text); font-size: 15px;
}
.info { flex: 1; min-width: 0; }
.title { font-weight: 600; }
.meta { color: var(--muted); font-size: 13px; }
.dl { flex: none; color: var(--accent); text-decoration: none; font-size: 14px; padding: 8px 4px; }
.empty { color: var(--muted); text-align: center; padding: 30px 12px; background: var(--card); border-radius: 12px; }
.player {
  position: fixed; left: 0; right: 0; bottom: 0; background: var(--card); border-top: 1px solid var(--line);
  padding: 8px 16px calc(8px + env(safe-area-inset-bottom)); display: none;
}
.player.on { display: block; }
.player div { max-width: 640px; margin: 0 auto; }
.player .now { font-size: 13px; color: var(--muted); margin-bottom: 4px; }
audio { width: 100%; }
</style>
</head>
<body>
<main>
  <h1>Гостевая книга</h1>
  <div class="status" id="status">Загрузка…</div>
  <div class="warn" id="clock" hidden>Часы телефона не выставлены, у новых записей не будет даты.
    Они выставятся автоматически по времени этого устройства.</div>
  <div class="error" id="error"></div>
  <a class="zip disabled" id="zip" href="/guestbook.zip" download>Скачать всё<small></small></a>
  <div id="list"></div>
</main>
<div class="player" id="player"><div>
  <div class="now" id="now"></div>
  <audio id="audio" controls preload="none"></audio>
</div></div>
<script>
const $ = id => document.getElementById(id);
const BYTES_PER_SEC = 88200; // 44.1 kHz, 16 bit, mono
const MONTHS = ['янв','фев','мар','апр','мая','июн','июл','авг','сен','окт','ноя','дек'];
let playing = null, lastJson = '';

function size(b) {
  if (b >= 1e9) return (b / 1e9).toFixed(1) + ' ГБ';
  if (b >= 1e6) return (b / 1e6).toFixed(1) + ' МБ';
  return Math.max(1, Math.round(b / 1e3)) + ' КБ';
}
function duration(b) {
  const s = Math.max(0, Math.round((b - 44) / BYTES_PER_SEC));
  return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
}
// Times are local wall-clock time stored as unix timestamps, so read them back as UTC
function date(t) {
  const d = new Date(t * 1000);
  return d.getUTCDate() + ' ' + MONTHS[d.getUTCMonth()] + ', ' +
    String(d.getUTCHours()).padStart(2, '0') + ':' + String(d.getUTCMinutes()).padStart(2, '0');
}
function ago(s) {
  if (s < 60) return s + ' с назад';
  if (s < 3600) return Math.floor(s / 60) + ' мин назад';
  return Math.floor(s / 3600) + ' ч назад';
}

async function loadStatus() {
  try {
    const s = await (await fetch('/api/status')).json();
    let dot = 'off', text = 'Нет связи с телефоном';
    if (s.teensyOnline) {
      if (s.teensyMode === 'Ready') { dot = 'ok'; text = 'Телефон на связи'; }
      else if (s.teensyMode === 'Recording') { dot = 'busy'; text = 'Идёт запись'; }
      else { dot = 'busy'; text = 'Телефон занят'; }
    }
    const parts = [`<span><span class="dot ${dot}"></span>${text}</span>`];
    if (s.teensyFiles >= 0) parts.push(`<span>На телефоне: ${s.teensyFiles}</span>`);
    if (s.copyingSize) parts.push(`<span>Копирую №${s.copyingNum}: ${Math.floor(100 * s.copyingDone / s.copyingSize)}%</span>`);
    if (s.pending) parts.push(`<span>Ждут копирования: ${s.pending}</span>`);
    else if (s.lastSyncAgo >= 0) parts.push(`<span>Проверено ${ago(s.lastSyncAgo)}</span>`);
    if (s.sdTotal) parts.push(`<span>Свободно ${size(s.sdTotal - s.sdUsed)}</span>`);
    $('status').innerHTML = parts.join('');
    $('clock').hidden = !s.teensyOnline || s.clockValid;
    $('error').textContent = s.lastError ? 'Последняя ошибка: ' + s.lastError : '';
  } catch (e) {
    $('status').innerHTML = '<span><span class="dot off"></span>Нет связи с сервером</span>';
  }
}

async function loadRecordings() {
  let json;
  try { json = await (await fetch('/api/recordings')).text(); } catch (e) { return; }
  if (json === lastJson) return;
  lastJson = json;
  const recs = JSON.parse(json).reverse(); // newest first
  const total = recs.reduce((a, r) => a + r.size, 0);
  $('zip').classList.toggle('disabled', recs.length === 0);
  $('zip').querySelector('small').textContent = recs.length ? `${recs.length} зап., ${size(total)}, ZIP` : 'пока нет записей';
  if (!recs.length) {
    $('list').innerHTML = '<div class="empty">Записей пока нет</div>';
    return;
  }
  $('list').innerHTML = '<ul>' + recs.map(r => `
    <li data-n="${r.n}"${r.n === playing ? ' class="playing"' : ''}>
      <button class="play" aria-label="Слушать">▶</button>
      <div class="info">
        <div class="title">№${r.n}${r.t ? ' · ' + date(r.t) : ''}</div>
        <div class="meta">${duration(r.size)} · ${size(r.size)}</div>
      </div>
      <a class="dl" href="/api/rec?n=${r.n}&dl=1" download="${r.name}">Скачать</a>
    </li>`).join('') + '</ul>';
}

$('list').addEventListener('click', e => {
  const btn = e.target.closest('.play');
  if (!btn) return;
  const li = btn.closest('li');
  playing = Number(li.dataset.n);
  document.querySelectorAll('li.playing').forEach(x => x.classList.remove('playing'));
  li.classList.add('playing');
  $('now').textContent = li.querySelector('.title').textContent;
  $('player').classList.add('on');
  $('audio').src = '/api/rec?n=' + playing;
  $('audio').play();
});

// Give the phone's clock to the Teensy (it has no backup battery)
const now = new Date();
fetch('/api/time?t=' + Math.floor(now.getTime() / 1000 - now.getTimezoneOffset() * 60), { method: 'POST' });

loadStatus(); loadRecordings();
setInterval(loadStatus, 3000);
setInterval(loadRecordings, 10000);
</script>
</body>
</html>
)rawliteral";
