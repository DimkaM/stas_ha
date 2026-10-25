# stas_ha

## Цель проекта

Контроль водоснабжения и теплоснабжения частного дома.

В частности:

- Контроль давления воды, для своевременной подкачки гидроаккумулятора.
- Контроль температуры воздуха для избежания замерзания воды (в перспективе).

В качестве ПО верхнего уровня используется Home Assistant (HA), обмен —
по MQTT с автообнаружением (MQTT Discovery).

**Подробное состояние работ, схема подключения и инструкции по сборке —
в [PROGRESS.md](PROGRESS.md).**

## Стек

| Что | Чем |
|---|---|
| Контроллер | **ESP32-C3** (плата LuatOS CORE ESP32-C3, 4 MB flash) |
| Прошивка | **ESP-IDF 6.0** (`espidf`, C++), сборка `idf.py` |
| АЦП | **ADS1115** по I2C (0x48), gain `±4.096 В` |
| Датчик | 3-проводной датчик давления 5 В, выход 0,5–4,5 В = 0–1,2 МПа |
| Мост USB-UART | CH343 (`1a86:55d3`), под Windows — **COM6**, под Linux — `/dev/ttyACM0` |

## Схема подключения

### ESP32-C3 ↔ ADS1115

| ESP32-C3 (пин на гребёнке) | ADS1115 |
|---|---|
| `3.3v` | `VDD` |
| `GND` | `GND` |
| **GPIO5** (SDA) | `SDA` |
| **GPIO6** (SCL) | `SCL` |
| — | `ADDR` → `GND` (адрес 0x48) |
| — | `ALRT` — не подключён |

GPIO5/6 не являются strapping-пинами, не заняты USB (GPIO18/19) и не заняты
SPI-flash (GPIO14–17). Да, это JTAG-пины (MTDI/MTCK), но JTAG не используется —
прошивка идёт по UART через CH343. Альтернатива при необходимости:
`SDA = GPIO8`, `SCL = GPIO9` (правится в `main/config.h`).

### Датчик давления

| Датчик | Куда |
|---|---|
| `VCC` | 5 В (пин `5v` платы) |
| `GND` | `GND` (общая земля с ESP32 и ADS1115) |
| `OUT` | делитель R1/R2 → `A0` ADS1115 |

Делитель напряжения (R1 = 3,0 кОм последовательно, R2 = 8,2 кОм на землю):

```
Датчик OUT ──[ R1 3k0 ]──┬── ADS1115 A0
                          │
                      [ R2 8k2 ]
                          │
                         GND
```

Максимум 4,5 В на выходе датчика → 3,30 В на входе АЦП (предел входов ADS1115
при VDD = 3,3 В ≈ 3,6 В).

## Структура проекта

```
stas_ha/
├── CMakeLists.txt              # ESP-IDF проект (project(stas_ha))
├── sdkconfig.defaults          # 4 MB flash, single app large, UART0 115200
├── idf.ps1                     # обёртка: активирует ESP-IDF и запускает idf.py
├── .vscode/
│   ├── settings.json           # пути ESP-IDF, порт COM6
│   └── tasks.json              # Build / Flash / Monitor / Erase / Menuconfig
├── main/
│   ├── CMakeLists.txt          # компонент main (main.cpp, ads1115.cpp)
│   ├── idf_component.yml       # зависимость espressif/mqtt (Component Registry)
│   ├── config.h                # пины, параметры АЦП/делителя/датчика, топики MQTT
│   ├── secrets.h               # WiFi/MQTT-креды (НЕ коммитится)
│   ├── secrets.h.example       # шаблон secrets.h
│   ├── ads1115.h/.cpp          # минимальный драйвер ADS1115 (driver/i2c_master)
│   └── main.cpp                # вся логика узла
└── docs/                       # пин-аут платы
```

## Что сделано

- Проект переведён с PlatformIO/Arduino на **ESP-IDF 6.0** — тот же локальный
  тулчейн, что и у рабочего проекта `D:\src\appsDM\testspi`.
- WiFi STA (`esp_wifi` + reconnect), MQTT (`esp-mqtt` из Component Registry) с
  LWT, HA MQTT Discovery на 4 сущности.
- Драйвер ADS1115 на `driver/i2c_master.h`, медианный фильтр из 9 отсчётов.
- Сборка и прошивка проверены на железе (COM6): устройство стартует, получает
  IP по DHCP, читает давление через ADS1115 (`0.015 bar` при атмосферном
  давлении — ноль в норме).
- MQTT проверен на тестовом брокере (Mosquitto в WSL): публикуются
  `availability`, 4 discovery-конфига (retained) и `state` каждые 5 с.

## Быстрый старт (Windows)

```powershell
# 0. один раз на чистом клоне: sdkconfig не коммитится, целевую плату надо задать
.\idf.ps1 set-target esp32c3

# 1. креды
copy main\secrets.h.example main\secrets.h   # вписать SSID/пароль и адрес MQTT

# 2. сборка / прошивка / лог
.\idf.ps1 build
.\idf.ps1 -p COM6 flash
.\idf.ps1 -p COM6 monitor
```

Либо задачи VS Code: `ESP-IDF: Build`, `ESP-IDF: Flash`, `ESP-IDF: Monitor`,
`ESP-IDF: Flash + Monitor`.

### Требования

- ESP-IDF **6.0** в `C:\Users\User\esp\v6.0\esp-idf`, инструменты в
  `C:\Users\User\.espressif` (пути заданы в `idf.ps1` и `.vscode/settings.json`).
- Первая сборка требует интернета: `esp-mqtt` подтягивается менеджером
  компонентов в `managed_components/`.

## Ограничения

- OTA (в Arduino-версии — `ArduinoOTA`) не портирован: сейчас обновление только
  по USB. Планируется `esp_https_ota`.
- MQTT сейчас направлен на **тестовый Mosquitto в WSL** этого ПК
  (`MQTT_HOST "192.168.1.46"`, см. PROGRESS.md §6.1): цепочка
  устройство → брокер → discovery/state проверена. Когда появится HA —
  заменить `MQTT_HOST` на адрес его брокера.
