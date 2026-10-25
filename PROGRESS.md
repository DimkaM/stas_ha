# stas_ha — текущее состояние работ

> Снимок наработок на момент переезда с Linux на Windows.
> Всё, что описано ниже, уже лежит в репозитории; незавершённые пункты помечены явно.

---

## 1. Цель

Контроль водоснабжения и теплоснабжения частного дома:

- контроль **давления воды** (своевременная подкачка гидроаккумулятора);
- (в перспективе) контроль **температуры воздуха** против замерзания воды.

ПО верхнего уровня — **Home Assistant (HA)**. Устройство — **ESP32-C3**, датчик давления
подключается через **АЦП ADS1115**.

---

## 2. Что уже сделано

### 2.1. Структура проекта (PlatformIO)

```
stas_ha/
├── platformio.ini              # конфигурация PlatformIO (env esp32c3)
├── pio.sh                      # обёртка над локально установленным PlatformIO Core
├── .gitignore                  # исключает .pio, .piotools, .venv, include/secrets.h
├── README.md                   # краткое описание проекта
├── PROGRESS.md                 # этот файл
├── include/
│   ├── config.h                # пины, параметры АЦП/делителя/датчика, топики MQTT
│   ├── secrets.h               # WiFi/MQTT-креды (НЕ коммитится, сейчас заглушки)
│   └── secrets.h.example       # шаблон secrets.h
└── src/
    └── main.cpp                # вся логика прошивки
```

### 2.2. Параметры сборки (`platformio.ini`)

| Параметр | Значение |
|---|---|
| env | `esp32c3` (default) |
| platform | `espressif32` |
| board | `esp32-c3-devkitm-1` |
| framework | `arduino` |
| upload_port / monitor_port | `/dev/ttyACM0` (Linux), на Windows будет `COMx` |
| monitor_speed | 115200 |
| build_flags | `-std=gnu++17 -Wall -Wextra -DCORE_DEBUG_LEVEL=1` |

Библиотеки (`lib_deps`):

- `adafruit/Adafruit ADS1X15@^2.4.0` — работа с ADS1115;
- `knolleary/PubSubClient@^2.8` — MQTT;
- `bblanchon/ArduinoJson@^7.1.0` — JSON для HA discovery и state.

### 2.3. Выбранные пины ESP32-C3

| Сигнал | GPIO ESP32-C3 | Комментарий |
|---|---|---|
| I2C **SDA** | **GPIO5** | не strapping, свободен (JTAG-пин, но JTAG не используется — прошивка через UART-мост) |
| I2C **SCL** | **GPIO6** | то же |
| ADS1115 VDD | 3V3 | питание АЦП 3.3 В |
| ADS1115 GND | GND | |
| ADS1115 ADDR | GND | адрес I2C = **0x48** |
| ADS1115 ALRT | — | не используется |

Не заняты и доступны на будущее: GPIO0, GPIO1, GPIO2 (strapping), GPIO3, GPIO4,
GPIO7, GPIO10. **Не использовать** GPIO18/GPIO19 (USB D-/D+), GPIO20/GPIO21 (UART0,
заняты USB-UART мостом), GPIO12–17 (SPI flash).

Альтернатива, если GPIO5/6 заняты на конкретной плате: SDA=GPIO0, SCL=GPIO1
(правится в `include/config.h` → `PIN_I2C_SDA` / `PIN_I2C_SCL`).

### 2.4. Подключение датчика давления

Датчик: 3-проводной, питание **5 В**, выход **0,5…4,5 В** = **0…1,2 МПа (0…12 бар)**.

Делитель напряжения (уже спаян): R1 = 3,0 кОм (последовательно), R2 = 8,2 кОм (на GND).

```
Датчик OUT ──[ R1 3k0 ]──┬── ADS1115 A0
                          │
                      [ R2 8k2 ]
                          │
                         GND
```

Расчёт: `V_ads = V_sensor × R2/(R1+R2) = V_sensor × 0,7321`
→ при 4,5 В на датчике на входе АЦП **3,295 В** (укладывается в диапазон ±4,096 В,
усиление `GAIN_ONE`; предел входов ADS1115 при VDD=3,3 В ≈ 3,6 В — запас есть).
Коэффициент обратного пересчёта `DIVIDER_RATIO = 11,2/8,2 = 1,3659`.

Полная таблица соединений:

| Откуда | Куда |
|---|---|
| ESP32-C3 `3V3` | ADS1115 `VDD` |
| ESP32-C3 `GND` | ADS1115 `GND`, датчик `GND` (общая земля!) |
| ESP32-C3 `GPIO5` | ADS1115 `SDA` |
| ESP32-C3 `GPIO6` | ADS1115 `SCL` |
| ADS1115 `ADDR` | `GND` (адрес 0x48) |
| ESP32-C3 `5V` (VBUS/USB) | датчик `VCC` (5 В) |
| датчик `OUT` | R1 3k0 → узел A0 → R2 8k2 → GND |

### 2.5. Логика прошивки (`src/main.cpp`)

1. `Wire.begin(5, 6, 400 kHz)`, инициализация `Adafruit_ADS1115` по адресу 0x48,
   `GAIN_ONE`, 128 SPS, continuous mode. Если АЦП не найден — периодический ретрай
   каждые 10 с (устройство не «залипает»).
2. Чтение канала A0: 9 замеров → **медианный фильтр** (устойчивость к выбросам).
3. `V_sensor = V_adc × DIVIDER_RATIO`.
4. Диагностика обрыва: если `V_sensor < 0,25 В` или `> 5,0 В` — датчик считается
   неподключённым, давление публикуется как `null`.
5. `pressure_bar = (V_sensor − 0,5) / (4,5 − 0,5) × 12`, ограничение 0…20 бар.
6. WiFi STA + hostname, авто-реконнект.
7. MQTT: LWT (`offline` в availability-топик), keepalive 30 с, буфер увеличен до 1024
   (payload HA discovery больше дефолтных 256 байт), авто-реконнект.
8. **Home Assistant MQTT Discovery**: при подключении публикуются 4 сущности,
   привязанные к одному устройству (`dev.ids = stas_ha_pressure_<mac>`):
   - `sensor` «Давление воды» — bar, `device_class: pressure`;
   - `sensor` «Напряжение датчика» — V, diagnostic;
   - `sensor` «ADC напряжение» — V, diagnostic;
   - `binary_sensor` «Датчик давления» — connectivity, diagnostic.
9. ArduinoOTA (hostname/пароль из `secrets.h`).
10. Публикация состояния каждые **5 с**.

### 2.6. MQTT-топики

| Топик | Направление | Payload |
|---|---|---|
| `stas_ha/pressure/state` | ESP → HA | `{"pressure_bar":4.213,"sensor_voltage":2.654,"adc_voltage":1.9431,"connected":true}` |
| `stas_ha/pressure/availability` | ESP → HA | `online` / `offline` (LWT) |
| `stas_ha/pressure/cmd` | HA → ESP | `restart` или `discovery` (переотправить discovery) |
| `homeassistant/<component>/stas_ha_pressure_<mac>/<obj>/config` | ESP → HA | discovery-конфиг (retained) |

---

## 3. Статус проверки

| Пункт | Статус |
|---|---|
| Структура проекта PlatformIO | ✅ создана |
| Код прошивки написан | ✅ `src/main.cpp`, `include/config.h` |
| Выбор пинов | ✅ GPIO5 (SDA) / GPIO6 (SCL), ADDR→GND (0x48) |
| PlatformIO Core установлен | ✅ 6.2.0 (локально, `.piotools/`, см. `pio.sh`) |
| Платформа `espressif32` + toolchain riscv32 | ✅ скачаны в `~/.platformio` |
| **Сборка прошивки** | ⚠️ **не завершена/не подтверждена** — сборка прервана переездом на Windows |
| **Прошивка ESP32** | ❌ не выполнялась |
| **Проверка с ADS1115 «в железе»** | ❌ не выполнялась |
| **Проверка появления в HA** | ❌ не выполнялась |
| Заполнение `include/secrets.h` | ❌ заглушки `CHANGE_ME` / `192.168.1.10` |

---

## 4. Как собрать и прошить

### 4.1. Windows (целевая платформа)

1. Установить **Python 3.11+** (python.org, галочка «Add to PATH»).
2. Установить **PlatformIO Core**:
   ```bat
   py -m pip install -U platformio
   ```
   (либо расширение *PlatformIO IDE* в VS Code — тогда CLI доступен как `pio`).
3. Драйвер USB-UART **CH343/CH340** (чип `1a86:55d3`) — обычно подхватывается
   Windows 10/11 автоматически; если нет — поставить с wch.cn.
4. Определить COM-порт (Диспетчер устройств → Порты COM и LPT), например `COM5`.
5. Поправить `platformio.ini`:
   ```ini
   upload_port = COM5
   monitor_port = COM5
   ```
   (или удалить обе строки и добавить `upload_port = auto`).
6. Заполнить `include/secrets.h` (WiFi SSID/пароль, IP MQTT-брокера HA, логин/пароль).
7. Сборка / прошивка / монитор:
   ```bat
   pio run
   pio run -t upload
   pio device monitor -b 115200
   ```

### 4.2. Linux (как было)

```bash
# ВАЖНО: в этой системе отсутствует python3-venv, поэтому PlatformIO поставлен
# локально в каталог проекта (pip --target), а не в venv.
python3 -m pip install --break-system-packages --target .piotools "platformio>=6.1"

./pio.sh run                 # сборка
./pio.sh run -t upload       # прошивка в /dev/ttyACM0
./pio.sh device monitor      # лог 115200
```

Порт: `/dev/ttyACM0`, USB `1a86:55d3` «USB Single Serial», SerialNumber **5B14142211**.
Пользователь должен быть в группе `dialout` (проверено — есть).

---

## 5. Что нужно доделать после переезда на Windows

- [ ] Установить PlatformIO Core, проверить `pio --version`.
- [ ] Определить COM-порт ESP32 и прописать его в `platformio.ini`.
- [ ] Заполнить `include/secrets.h` реальными WiFi/MQTT-данными.
- [ ] **Собрать** (`pio run`) и починить ошибки компиляции, если появятся.
      Наиболее вероятные места: API `Adafruit_ADS1X15` v2.4 (`setMode`, `computeVolts`),
      `ArduinoJson` v7 (`JsonDocument`, `serialized`).
- [ ] Прошить (`pio run -t upload`), снять лог (`pio device monitor`):
      в логе должны быть строки `[ADS] ADS1115 initialised`, `[WiFi] connected`,
      `[MQTT] connected`, `[MQTT] HA discovery published`, затем `[PUB] {...}`.
- [ ] Подключить датчик по таблице из п. 2.4, проверить `[SENS] ... pressure=...`:
      при атмосферном давлении ожидается ~0 бар, `sensor_voltage` ≈ 0,5 В,
      `adc_voltage` ≈ 0,37 В.
- [ ] В HA: MQTT-интеграция должна создать устройство «stas_ha pressure»
      с 4 сущностями (раздел «Настройки → Устройства и службы → MQTT»).
- [ ] Проверить OTA (`pio run -t upload --upload-port <ip>`).
- [ ] Добавить в HA автоматизацию на низкое давление (порог ~2 бар) — уведомление
      о подкачке гидроаккумулятора.

### Возможные уточнения по железу

- Плата может быть не «DevKitM-1», а **ESP32-C3 SuperMini** — тогда `board` лучше
  оставить `esp32-c3-devkitm-1` (совместимо), но проверить, что GPIO5/6 выведены
  на гребёнку и не заняты встроенным LED (у SuperMini LED на GPIO8).
- Датчик 0…1,2 МПа при 0,5…4,5 В — характеристика линейная, но реальный «нуль»
  может отличаться на ±0,05 В. При необходимости добавить в `config.h` калибровочные
  смещения (`SENSOR_V_AT_ZERO` подкрутить по факту).
- Питание датчика 5 В берётся с USB; при переходе на постоянное питание
  предусмотреть отдельный 5 В источник (рабочий ток датчика < 10 мА).

---

## 6. Проблемы окружения, из-за которых переехали на Windows

| Проблема | Детали |
|---|---|
| Нет `python3-venv` / `ensurepip` | `python3 -m venv` падает, `apt install python3.12-venv` требует sudo |
| PEP 668 (externally-managed-environment) | `pip install --user` заблокирован без `--break-system-packages` |
| Нестабильная сеть до PyPI | таймауты `files.pythonhosted.org`, часть пакетов скачивалась с ретраями |
| Ранее: пустой `~/.platformio` | PlatformIO Core не был установлен (было только расширение VS Code) |

Обходной путь, который сработал: `pip install --target .piotools` + обёртка `pio.sh`.

---

## 7. Быстрый чек-лист «с нуля» на новой машине

```bat
git clone git@github.com:DimkaM/stas_ha.git
cd stas_ha
copy include\secrets.h.example include\secrets.h
rem отредактировать secrets.h и platformio.ini (COM-порт)
py -m pip install -U platformio
pio run -t upload
pio device monitor -b 115200