# stas_ha — текущее состояние работ

> Снимок на 25.10.2026. Проект переведён с PlatformIO/Arduino на локальный
> **ESP-IDF 6.0** (тот же тулчейн, что у рабочего проекта `D:\src\appsDM\testspi`).
> Сборка и прошивка проверены на железе.

---

## 1. Цель

Контроль водоснабжения и теплоснабжения частного дома:

- контроль **давления воды** (своевременная подкачка гидроаккумулятора);
- (в перспективе) контроль **температуры воздуха** против замерзания воды.

ПО верхнего уровня — **Home Assistant (HA)**, обмен по MQTT с автообнаружением.
Устройство — **ESP32-C3** (LuatOS CORE ESP32-C3, 4 MB flash), датчик давления
через **АЦП ADS1115**.

---

## 2. Состояние железа

| Параметр | Значение |
|---|---|
| Плата | LuatOS CORE ESP32-C3 (пин-аут: `docs/LuatOS-CORE-ESP32-C3-pinout-low-1024x748.jpg.webp`) |
| Чип | ESP32-C3, silicon rev **v0.4**, 1 ядро, 160 MHz |
| Flash | 4 MB (gd, DIO, 80 MHz) |
| USB-UART | CH343 `USB\VID_1A86&PID_55D3\5B14142211` → **COM6** (115200) |
| Консоль | UART0, GPIO20 (TX) / GPIO21 (RX) |

### 2.1. Пины

| Сигнал | GPIO ESP32-C3 | Комментарий |
|---|---|---|
| I2C **SDA** | **GPIO5** | на гребёнке справа, рядом с GPIO4; не strapping, не flash, не USB; JTAG MTDI (JTAG не используется) |
| I2C **SCL** | **GPIO6** | рядом с GPIO7; JTAG MTCK |
| ADS1115 `VDD` | `3.3v` | питание АЦП 3,3 В |
| ADS1115 `GND` | `GND` | |
| ADS1115 `ADDR` | `GND` | адрес I2C = **0x48** |
| ADS1115 `ALRT` | — | не используется |
| Датчик `VCC` | `5v` | пин 5 В на гребёнке |
| Датчик `GND` | `GND` | общая земля |

Strapping-пины C3 — GPIO2, GPIO8, GPIO9: их для I2C лучше не использовать
(GPIO9 — кнопка BOOT, GPIO8 — strap + светодиод). Альтернатива при занятых
GPIO5/6: `SDA = GPIO8`, `SCL = GPIO9` (правится в `main/config.h`).

### 2.2. Делитель напряжения

Датчик: 3-проводной, питание **5 В**, выход **0,5…4,5 В** = **0…1,2 МПа (0…12 бар)**.

```
Датчик OUT ──[ R1 3k0 ]──┬── ADS1115 A0
                          │
                      [ R2 8k2 ]
                          │
                         GND
```

- `V_ads = V_sensor × R2/(R1+R2) = V_sensor × 0,7321` → при 4,5 В на датчике
  на входе АЦП **3,295 В** (усиление `GAIN_ONE` ±4,096 В, предел входов при
  VDD = 3,3 В ≈ 3,6 В).
- Обратный коэффициент `DIVIDER_RATIO = 11,2/8,2 = 1,3659`.

---

## 3. Структура проекта

```
stas_ha/
├── CMakeLists.txt              # ESP-IDF проект (project(stas_ha))
├── sdkconfig.defaults          # 4 MB flash, SINGLE_APP_LARGE, UART0 115200, log INFO
├── sdkconfig                   # генерируется idf.py set-target esp32c3 (в .gitignore)
├── idf.ps1                     # обёртка: активирует IDF и запускает idf.py
├── .vscode/
│   ├── settings.json           # espIdfPathWin / toolsPathWin / pythonBinPathWin / portWin=COM6
│   └── tasks.json              # Build, Set target, Full clean, Flash, Monitor, Flash+Monitor, Erase, Menuconfig, Size
├── main/
│   ├── CMakeLists.txt          # idf_component_register(SRCS main.cpp ads1115.cpp)
│   ├── idf_component.yml       # espressif/mqtt ^1.0.0 (Component Registry)
│   ├── config.h                # пины, АЦП, делитель, датчик, тайминги, топики
│   ├── secrets.h               # WiFi/MQTT (не коммитится)
│   ├── secrets.h.example       # шаблон
│   ├── ads1115.h/.cpp          # драйвер ADS1115 на driver/i2c_master.h
│   └── main.cpp                # WiFi + MQTT + discovery + чтение давления
├── docs/                       # пин-аут платы
└── managed_components/         # скачивается менеджером компонентов (в .gitignore)
```

---

## 4. Логика прошивки (`main/main.cpp`, `main/ads1115.cpp`)

1. `app_main()`: лог версии IDF, `device id = stas_ha_pressure_<mac>` (из
   `esp_read_mac`), `nvs_flash_init()` (с erase при смене версии NVS).
2. Инициализация ADS1115 (`driver/i2c_master.h`, порт `I2C_NUM_0`, SDA=GPIO5,
   SCL=GPIO6, 400 кГц, адрес 0x48): создаётся шина и устройство, затем
   читается регистр конфигурации как проверка ACK. Если АЦП не отвечает —
   повторная попытка каждые **10 с** (без перезагрузки).
3. Чтение канала A0: **single-shot**, `OS=1`, `MUX=AIN0-GND`, `PGA=±4.096 В`,
   `DR=128 SPS` (регистр конфигурации `0x8383`); ожидание 10 мс; чтение
   регистра конверсии. Затем 9 замеров → **медианный фильтр**.
4. `V_sensor = V_adc × 1,3659`; диагностика обрыва: если `V_sensor < 0,25 В`
   или `> 5,0 В` — датчик считается неподключённым, давление публикуется `null`
   (ключ `connected: false`).
5. `pressure_bar = (V_sensor − 0,5) / (4,5 − 0,5) × 12`, ограничение 0…20 бар.
6. WiFi STA: `esp_netif` + `esp_event`, hostname `stas-ha-pressure`,
   автопереподключение при `WIFI_EVENT_STA_DISCONNECTED`.
7. MQTT (`esp-mqtt`, запускается сразу, реконнект каждые 5 с):
   - LWT: `offline` в топик availability (retain, QoS 1), keepalive 30 с;
   - при подключении: `online`, `subscribe` на командный топик, публикация
     HA discovery (retain);
   - команды: `restart` (перезагрузка), `discovery` (переотправить discovery).
8. Каждые **5 с**: датчик читается и логируется всегда (даже без брокера —
   это единственный способ проверить железо); публикация состояния в MQTT
   выполняется только при активном подключении к брокеру.

### 4.1. MQTT-топики

| Топик | Направление | Payload |
|---|---|---|
| `stas_ha/pressure/state` | ESP → HA | `{"pressure_bar":4.213,"sensor_voltage":2.654,"adc_voltage":1.9431,"connected":true}` |
| `stas_ha/pressure/availability` | ESP → HA | `online` / `offline` (LWT) |
| `stas_ha/pressure/cmd` | HA → ESP | `restart` или `discovery` |
| `homeassistant/<component>/stas_ha_pressure_<mac>/<obj>/config` | ESP → HA | discovery-конфиг (retained) |

### 4.2. Сущности HA (одно устройство «stas_ha pressure»)

| Компонент | Имя | Класс |
|---|---|---|
| `sensor` | Давление воды | `pressure`, bar |
| `sensor` | Напряжение датчика | `voltage`, diagnostic |
| `sensor` | ADC напряжение | `voltage`, diagnostic |
| `binary_sensor` | Датчик давления | `connectivity`, diagnostic |

---

## 5. Статус проверки

| Пункт | Статус |
|---|---|
| ESP-IDF 6.0 + riscv32-esp-elf 15.2.0 локально | ✅ `ESP-IDF v6.0-beta1-567-gdaa1d34909` |
| Каркас ESP-IDF-проекта (CMake, sdkconfig.defaults, .vscode) | ✅ |
| `esp-mqtt` из Component Registry | ✅ скачан в `managed_components/espressif__mqtt` |
| **Сборка** `idf.py build` | ✅ `stas_ha.bin` 957 328 Б (0xE9B90), партиция 1,5 МБ, свободно 38 % |
| **Прошивка** `idf.py -p COM6 flash` | ✅ 957 328 Б, hash verified |
| Загрузка и лог | ✅ `SYS: device id: stas_ha_pressure_d885ac2bf6e0`, heap 215 196 Б |
| WiFi | ✅ подключился (`n53v`, WPA2-PSK, IP 192.168.1.89) |
| ADS1115 по I2C | ✅ `ADS: ADS1115 initialised at 0x48, gain +-4.096 V, 128 SPS` |
| Чтение A0 | ✅ стабильно (разброс ~1 мВ) |
| Датчик давления «в железе» | ✅ подключён: `adc=0.3697 V`, `sensor=0.505 V`, `pressure=0.015 bar` при атмосферном давлении (расчёт для 0 бар: 0,37 В / 0,50 В) — сходится, смещение нуля датчика всего +5 мВ |
| MQTT / HA discovery | ✅ проверено на тестовом брокере (Mosquitto в WSL): `MQTT: connected`, `availability=online`, 4 retained discovery-конфига, `state` каждые 5 с; за 30 с — 6 публикаций, разрывов нет |
| OTA | ❌ не портировано |

---

## 6. Как собрать и прошить

```powershell
# всё через обёртку (сама активирует IDF и PATH)
.\idf.ps1 build
.\idf.ps1 -p COM6 flash
.\idf.ps1 -p COM6 monitor

# или задачи VS Code: ESP-IDF: Build / Flash / Monitor / Flash + Monitor
```

Эквивалент без обёртки (после `C:\Users\User\esp\v6.0\esp-idf\export.ps1`):

```powershell
idf.py build
idf.py -p COM6 -b 921600 flash
idf.py -p COM6 monitor
```

Порт и скорость — в `.vscode/settings.json` (`idf.portWin`, `idf.flashBaudRate`).
Креды — в `main/secrets.h` (шаблон `main/secrets.h.example`).

---

## 6.1. Тестовый брокер MQTT (WSL)

Пока в HA нет брокера, для проверки поднят **Mosquitto 1.6.9 в WSL (Ubuntu-20.04)**:

```powershell
# запуск / статус / остановка брокера
wsl -d Ubuntu-20.04 -u root -- service mosquitto start
wsl -d Ubuntu-20.04 -u root -- service mosquitto status
wsl -d Ubuntu-20.04 -u root -- service mosquitto stop

# смотреть все сообщения
wsl -d Ubuntu-20.04 -u root -- mosquitto_sub -h 127.0.0.1 -t "#" -v
```

Чтобы ESP32 (в LAN) доставал до брокера в WSL, в `C:\Users\User\.wslconfig`:

```ini
[wsl2]
networkingMode=mirrored   ; WSL видит сетевые интерфейсы Windows (192.168.1.46:1883)
firewall=false            ; иначе входящие с LAN режет Hyper-V firewall

[experimental]
hostAddressLoopback=true
```

Изменения применяются после `wsl --shutdown`. Брокер слушает `0.0.0.0:1883`,
анонимный доступ (конфиг Ubuntu по умолчанию). В `main/secrets.h` на время
тестов стоит `MQTT_HOST "192.168.1.46"` — это IP этого ПК в Wi-Fi сети.

Проверено: устройство подключается, публикует `availability=online`,
4 discovery-конфига (retained) и `state` каждые 5 с, например
`{"pressure_bar":0.016,"sensor_voltage":0.505,"adc_voltage":0.3699,"connected":true}`.

---

## 7. Что нужно доделать

- [ ] Вписать реальный адрес MQTT-брокера HA в `main/secrets.h`
      (`MQTT_HOST`), при необходимости логин/пароль; пересобрать и прошить.
- [x] Подключить ADS1115 по таблице п. 2.1 — в логе
      `ADS: ADS1115 initialised at 0x48`, чтение A0 стабильно.
- [x] Подключить датчик давления через делитель — при атмосферном давлении
      лог `SENS: adc=0.3697 V sensor=0.505 V pressure=0.015 bar (ok)`,
      т.е. ноль в пределах 0,02 бар.
- [ ] Когда появится HA: установить/включить MQTT-интеграцию, вписать адрес
      брокера в `main/secrets.h` (`MQTT_HOST`) вместо тестового `192.168.1.46`,
      пересобрать и прошить; убедиться, что в HA появилось устройство
      «stas_ha pressure» с 4 сущностями.
- [ ] Добавить в HA автоматизацию на низкое давление (порог ~2 бар).
- [ ] Портировать OTA (`esp_https_ota`) вместо удалённого `ArduinoOTA`.
- [ ] Ввести поправку нуля датчика (`SENSOR_V_AT_ZERO`) по факту, если
      расхождение > 0,05 В.

---

## 8. История окружения (полезно знать)

Раньше проект собирался PlatformIO + Arduino core. На этой машине сборка
PlatformIO не пошла: пакеты платформы (`toolchain-riscv32-esp` ~154 МБ,
`framework-arduinoespressif32`) качаются с GitHub-миррора
(`dl.registry.platformio.org` → `github.com/ivankravets/pioreg-ci-mirror`) и
обрывались на середине (скачано 161 644 544 Б при ожидаемом sha256
`55c5c365…`, реальный `4d9c2d65…`), а PlatformIO не умеет докачивать.
Кроме того, зависший процесс расширения PlatformIO (`project init --ide vscode`)
держал filelock в `~/.platformio/.cache/downloads/` и блокировал `pio run`.

Поэтому взят уже установленный локальный **ESP-IDF 6.0** (тот же, что у
`D:\src\appsDM\testspi`): сборка полностью локальная, единственная сетевая
зависимость — небольшой `esp-mqtt` из Component Registry при первой сборке.

Прежние файлы PlatformIO (`platformio.ini`, `pio.sh`, `src/main.cpp`,
`include/config.h`) удалены — они остались в истории git.
