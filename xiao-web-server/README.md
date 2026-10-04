# Веб-сервер гостевой книги (XIAO ESP32S3 Sense)

Копирует новые записи с Teensy по UART на свою microSD и раздаёт их по Wi-Fi.
Общее описание — в [../REQUIREMENTS.md](../REQUIREMENTS.md).

## Прошивка

1. Arduino IDE → Boards Manager: **esp32 by Espressif** (проверено на 3.3.12).
   URL для Board Manager: `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. Library Manager: **ESP Async WebServer** и **Async TCP**, обе от ESP32Async
   (проверено на 3.12.1 и 3.5.0). Библиотека «ESPAsyncWebServer» от lacamera — другая, она не нужна.
3. Плата: **XIAO_ESP32S3**, настройки по умолчанию (USB CDC On Boot: Enabled).
4. Открыть `xiao-web-server.ino` и загрузить.

Из командной строки:

```bash
arduino-cli compile -b esp32:esp32:XIAO_ESP32S3 --upload -p /dev/cu.usbmodemXXXX xiao-web-server
```

Teensy прошивается как раньше, но с **USB Type: «Serial + MTP Disk (Experimental)»**.

## Подключение

| Teensy    | XIAO             |
|-----------|------------------|
| 0 (RX1)   | D6 / GPIO43 (TX) |
| 1 (TX1)   | D7 / GPIO44 (RX) |
| GND       | GND              |
| Vin (5 В) | 5V               |

Если обе платы подключены к компьютеру по USB, провод 5 В отсоединить.

## Wi-Fi

- **Подключение к существующей сети.** Скопировать `wifi_secrets.example.h` в `wifi_secrets.h`,
  вписать имя сети и пароль, прошить. Файл `wifi_secrets.h` в `.gitignore` и в репозиторий не попадает.
  Адрес выдаёт роутер: XIAO печатает его в монитор порта (`Wi-Fi: connected, open http://…`)
  и по команде `status`. На iPhone и Mac работает и http://guestbook.local/.
- **Своя точка доступа.** Если `wifi_secrets.h` нет или к сети не удалось подключиться за 20 секунд,
  XIAO поднимает открытую сеть **Guestbook**, адрес http://192.168.4.1/
- Адрес набирать полностью, с `http://`: по `https://` сервер не отвечает.

## Использование

- При открытии страница передаёт Teensy время телефона: у Teensy нет батарейки часов.
- Монитор порта (115200): `status` — состояние, `sync` — проверить Teensy сейчас,
  `trace on`/`off` — обмен с Teensy, `http on`/`off` — входящие запросы.
- На SD-карте XIAO всё лежит в папке `/guestbook`: записи `00017.wav` и индекс `index.csv`.
  Если индекс удалить, он восстановится при следующем запуске (даты придут от Teensy).

## Устройство кода

| Файл | Что делает |
|------|------------|
| `xiao-web-server.ino` | запуск: SD, точка доступа, mDNS, консоль |
| `teensy_link.*` | клиент протокола из `../xiao_link.ino` |
| `sync.*` | фоновая задача: опрос Teensy, докачка в `.part`, проверка CRC |
| `catalog.*` | список скопированных записей и `index.csv` |
| `web.*`, `web_page.h` | HTTP API, отдача WAV с `Range`, страница |
| `zip_stream.*` | ZIP «на лету» без сжатия, размер известен заранее |
