# Compliance: лицензии и происхождение

Этот документ фиксирует корректное использование открытого ПО (атрибуция
лицензий) и условие развёртывания.

## Условие развёртывания
infcore разворачивается **локально на компьютере/сервере в РФ**, строго offline
(нулевой egress в рантайме). Никакого обязательного обращения к внешней инфраструктуре.

## Происхождение (обязательно раскрывать)
infcore **построен на базе open-source llama.cpp (ggml authors, MIT)**.
- ❌ Нельзя: «полностью собственная разработка», «с нуля» про весь продукт.
- ✅ Корректно: «на базе открытого ПО llama.cpp/ggml (MIT); собственная разработка —
  слой infcore/ (gateway/security/SDK) и сборка/поставка под локальный offline-контур».

## Лицензии
| Компонент | Лицензия | Где |
|---|---|---|
| ggml / llama.cpp (движок) | MIT (The ggml authors) | `THIRD_PARTY_LICENSES/ggml-llama.cpp.txt` |
| nlohmann/json | MIT | `THIRD_PARTY_LICENSES/nlohmann-json.txt` |
| cpp-httplib / stb / miniaudio / sheredom | MIT / Public Domain / Unlicense | `THIRD_PARTY_LICENSES/*` |
| infcore (свой слой) | MIT | `LICENSE`, `NOTICE` |

Артефакты: `LICENSE`, `NOTICE`, `THIRD_PARTY_LICENSES/` (полные тексты
лицензий на каждый компонент), `sbom.cdx.json` (CycloneDX 1.5, компоненты с версиями,
базовый коммит апстрима в purl). Docker runtime image также включает лицензию движка
как `/opt/infcore/LICENSE.llama.cpp`. Пин версии апстрима — см. `NOTICE`.

Для релиза запускается `infcore/scripts/release-manifest.sh`: он генерирует
`release-manifest.json` и `SHA256SUMS` для бинарей, лицензий и SBOM. При
`INFCORE_SIGN=1` дополнительно создаются detached GPG signatures.

## Модель неприкосновенности движка
Файлы вне `infcore/` (движок) руками не редактируются. Обновление — только слиянием
release-тега апстрима (`scripts/update-upstream.sh`). Это сохраняет чистоту границы,
корректность SBOM и возможность drop-in апдейтов.
