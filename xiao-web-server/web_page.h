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
.top { display: flex; align-items: center; justify-content: space-between; gap: 12px; }
.tool {
  flex: none; width: 40px; height: 40px; border-radius: 50%; border: 1px solid var(--line); cursor: pointer;
  background: var(--card); color: var(--muted); display: grid; place-items: center; padding: 0;
}
.tool.on { color: var(--accent); border-color: var(--accent); }
#service { margin: 18px 0 24px; }
.svc-head { display: flex; align-items: baseline; justify-content: space-between; margin-bottom: 8px; }
h2 { font-size: 18px; margin: 0; }
.link { background: none; border: 0; color: var(--accent); font: inherit; font-size: 14px; cursor: pointer; padding: 4px; }
.log li { display: block; padding: 8px 12px; }
.log .num { font-weight: 600; font-variant-numeric: tabular-nums; overflow-wrap: anywhere; }
.log .meta { font-variant-numeric: tabular-nums; }
.svc-head.second { margin-top: 18px; }
dialog {
  border: 0; border-radius: 14px; padding: 20px; width: min(340px, calc(100% - 32px));
  background: var(--card); color: var(--text);
}
dialog::backdrop { background: rgb(0 0 0 / .4); }
dialog h2 { margin-bottom: 12px; }
dialog input {
  width: 100%; font: inherit; padding: 10px 12px; border-radius: 10px;
  border: 1px solid var(--line); background: var(--bg); color: var(--text);
}
dialog .msg { color: var(--off); font-size: 14px; min-height: 20px; margin: 6px 0; }
dialog .buttons { display: flex; justify-content: flex-end; gap: 8px; }
dialog .buttons button { font: inherit; padding: 8px 16px; border-radius: 10px; border: 0; cursor: pointer; }
dialog .ok { background: var(--accent); color: var(--accent-text); font-weight: 600; }
dialog .cancel { background: none; color: var(--muted); }
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
  <div class="top">
    <h1>Гостевая книга</h1>
    <button class="tool" id="tool" aria-label="Сервис: пасхалки" title="Сервис">
      <svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="2"
        stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
        <path d="M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.77-3.77a6 6 0 0 1-7.94 7.94l-6.91 6.91a2.12 2.12 0 0 1-3-3l6.91-6.91a6 6 0 0 1 7.94-7.94l-3.76 3.76z"/>
        <path d="M4 4l2 .5 5 5"/>
        <path d="M14 14l5.5 5.5" stroke-width="3.5"/>
      </svg>
    </button>
  </div>
  <div class="status" id="status">Загрузка…</div>
  <div class="warn" id="sd" hidden>SD-карта XIAO не читается, записи не копируются и не показываются.
    Проверь, что карта вставлена в слот платы Sense до щелчка. XIAO пробует снова каждые 5 секунд.</div>
  <div class="warn" id="clock" hidden>Часы телефона не выставлены, у новых записей не будет даты.
    Они выставятся автоматически по времени этого устройства.</div>
  <div class="error" id="error"></div>
  <section id="service" hidden>
    <div class="svc-head"><h2>Пасхалки</h2><button class="link" id="svcClose">Закрыть</button></div>
    <div id="eggs"></div>
    <div class="svc-head second"><h2>Журнал набора</h2></div>
    <div id="dials"></div>
  </section>
  <a class="zip disabled" id="zip" href="/guestbook.zip" download>Скачать всё<small></small></a>
  <div id="list"></div>
</main>
<dialog id="login">
  <form id="loginForm">
    <h2>Сервис</h2>
    <input type="password" id="password" placeholder="Пароль" autocomplete="current-password" required>
    <div class="msg" id="loginMsg"></div>
    <div class="buttons">
      <button type="button" class="cancel" id="loginCancel">Отмена</button>
      <button class="ok">Войти</button>
    </div>
  </form>
</dialog>
<div class="player" id="player"><div>
  <div class="now" id="now"></div>
  <audio id="audio" controls preload="none"></audio>
</div></div>
<script>
const $ = id => document.getElementById(id);
const BYTES_PER_SEC = 88200; // 44.1 kHz, 16 bit, mono
const MONTHS = ['янв','фев','мар','апр','мая','июн','июл','авг','сен','окт','ноя','дек'];
let playing = null, lastJson = '', lastEggsJson = '', eggsTimer = null;

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
    $('sd').hidden = s.sdOk;
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
    <li data-key="r${r.n}" data-src="/api/rec?n=${r.n}"${'r' + r.n === playing ? ' class="playing"' : ''}>
      <button class="play" aria-label="Слушать">▶</button>
      <div class="info">
        <div class="title">№${r.n}${r.t ? ' · ' + date(r.t) : ''}</div>
        <div class="meta">${duration(r.size)} · ${size(r.size)}</div>
      </div>
      <a class="dl" href="/api/rec?n=${r.n}&dl=1" download="${r.name}">Скачать</a>
    </li>`).join('') + '</ul>';
}

function play(e) {
  const btn = e.target.closest('.play');
  if (!btn) return;
  const li = btn.closest('li');
  playing = li.dataset.key;
  document.querySelectorAll('li.playing').forEach(x => x.classList.remove('playing'));
  li.classList.add('playing');
  $('now').textContent = li.querySelector('.title').textContent;
  $('player').classList.add('on');
  $('audio').src = li.dataset.src;
  $('audio').play();
}
$('list').addEventListener('click', play);
$('eggs').addEventListener('click', play);

// Service view with the easter eggs guests recorded for other numbers. The XIAO checks the password
// and keeps the session in a cookie until it restarts.
async function loadEggs() {
  let res;
  try { res = await fetch('/api/eggs'); } catch (e) { return false; }
  if (!res.ok) return false;
  const json = await res.text();
  if (json === lastEggsJson) return true;
  lastEggsJson = json;
  const eggs = JSON.parse(json);
  $('eggs').innerHTML = !eggs.length ? '<div class="empty">Пасхалок пока нет</div>' : '<ul>' + eggs.map(g => `
    <li data-key="e${g.n}" data-src="/api/egg?n=${g.n}"${'e' + g.n === playing ? ' class="playing"' : ''}>
      <button class="play" aria-label="Слушать">▶</button>
      <div class="info">
        <div class="title">☎ ${g.n}</div>
        <div class="meta">${g.t ? date(g.t) + ' · ' : ''}${duration(g.size)} · ${size(g.size)}</div>
      </div>
      <a class="dl" href="/api/egg?n=${g.n}&dl=1" download="egg-${g.n}.wav">Скачать</a>
    </li>`).join('') + '</ul>';
  return true;
}
// Dial log from the Teensy: time, digits, pause before each digit [s], result (tab-separated lines)
const esc = s => s.replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
function dialResult(r) {
  let m;
  if (r === 'last recording') return 'последняя запись (1337)';
  if (r === 'easter egg played') return 'сыграла пасхалка';
  if (r === 'no easter egg, offered to record one') return 'пасхалки нет, предложено записать';
  if (r === 'hung up before the number was complete') return 'трубку положили, номер не закончен';
  if ((m = r.match(/^ignored dial movement, (\d+) pulses$/))) return `не засчитано: ${m[1]} импульсов`;
  if (r.endsWith('.wav')) return 'трек ' + r;
  return r;
}
async function loadDials() {
  let text;
  try {
    const res = await fetch('/api/dials');
    if (!res.ok) return;
    text = await res.text();
  } catch (e) { return; }
  const lines = text.split('\n').filter(l => l).reverse(); // newest first
  $('dials').innerHTML = !lines.length ? '<div class="empty">Номеров пока не набирали</div>' :
    '<ul class="log">' + lines.map(l => {
      const [time, number, pauses, result] = l.split('\t');
      return `<li><div class="num">${esc(number || '')}</div>
        <div class="meta">${esc(time || '')} · ${esc(dialResult(result || ''))}</div>
        ${pauses ? `<div class="meta">паузы перед цифрами, с: ${esc(pauses)}</div>` : ''}</li>`;
    }).join('') + '</ul>';
}

function showService(on) {
  $('service').hidden = !on;
  $('tool').classList.toggle('on', on);
  clearInterval(eggsTimer);
  if (on) {
    loadDials();
    eggsTimer = setInterval(() => { loadEggs(); loadDials(); }, 10000);
  }
}
$('tool').addEventListener('click', async () => {
  if (!$('service').hidden) { showService(false); return; }
  if (await loadEggs()) { showService(true); return; }
  $('password').value = '';
  $('loginMsg').textContent = '';
  $('login').showModal();
});
$('svcClose').addEventListener('click', () => showService(false));
$('loginCancel').addEventListener('click', () => $('login').close());
$('loginForm').addEventListener('submit', async e => {
  e.preventDefault();
  $('loginMsg').textContent = '';
  let res;
  try {
    res = await fetch('/api/service/login', { method: 'POST', body: new URLSearchParams({ p: $('password').value }) });
  } catch (err) {
    $('loginMsg').textContent = 'Нет связи с сервером';
    return;
  }
  if (res.status === 403) { $('loginMsg').textContent = 'Неверный пароль'; return; }
  if (!res.ok) { $('loginMsg').textContent = 'Сервисный вход не настроен'; return; }
  $('login').close();
  lastEggsJson = '';
  await loadEggs();
  showService(true);
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
