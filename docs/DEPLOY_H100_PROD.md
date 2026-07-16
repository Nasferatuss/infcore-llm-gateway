# infcore — прод-развёртывание на сервере с H100

Дополнение к [DEPLOY.md](DEPLOY.md): сборка под H100, TLS, ротация журнала,
мониторинг. Общие вещи (состав, модели, порты, дымовой тест) — там.

---

## 1. Сборка под H100

**H100 — это Hopper, `sm_90`.** Не `sm_89`.

```bash
cmake -S infcore -B build -C infcore/cmake/profile-h100.cmake
cmake --build build -j"$(nproc)"
```

> ⚠️ **Не копируйте команду сборки из [WINDOWS_WSL_MSI_TEST_PLAN_RU.md](WINDOWS_WSL_MSI_TEST_PLAN_RU.md).**
> Тест-план писался под RTX 4070 Laptop и задаёт `-DCMAKE_CUDA_ARCHITECTURES=89`
> (Ada). Собранный так бинарь **не несёт CUDA-ядер для H100**.

Проверить, что внутри действительно `sm_90`:

```bash
cuobjdump build/bin/llama-server | grep -oE 'arch = sm_[0-9]+' | sort -u
# ожидаем: arch = sm_90
```

`profile-h100.cmake` отличается от `profile-rf.cmake` только тем, что собирает
одну архитектуру (`90` вместо `75;80;86;89;90`) и выключает Vulkan. Если парк GPU
разнородный — берите `profile-rf.cmake`, он покрывает и H100.

### n_gpu_layers на H100

Модель на 25 GiB целиком помещается в 80 GiB VRAM, поэтому:

```json
{ "n_gpu_layers": -1 }
```

`-1` = не передавать флаг бэкенду, llama.cpp сам подгонит offload под свободную
VRAM. Любое явное значение эту авто-подгонку **отключает**
(`n_gpu_layers already set by user ... abort`), а на GPU, куда модель не влезает
целиком, ручной подбор проигрывает авто-режиму почти вдвое.

---

## 2. TLS

TLS терминируется на reverse-proxy (Angie/nginx), а не в шлюзе:

* перевыпуск сертификата = `reload` прокси, а не рестарт шлюза. Рестарт означает
  выгрузку модели и повторную проверку sha256 — на 25 GiB это десятки секунд простоя;
* TLS-стек не попадает в security-critical путь самого шлюза;
* шлюз остаётся HTTP на loopback и наружу не публикуется вовсе.

Конфиг: [`deploy/angie/infcore.conf`](../deploy/angie/infcore.conf) →
`/etc/angie/http.d/`. Сертификат — от **внутреннего CA**: ACME/Let's Encrypt в
offline-контуре невозможен.

```bash
angie -t && systemctl reload angie
```

### ⚠️ trusted_proxies — обязательно

Шлюз пишет в аудит IP пира соединения. За прокси пиром всегда оказывается **сам
прокси**, поэтому без настройки у **каждой** записи журнала будет `client_ip`
прокси, и аудит потеряет измерение «откуда»:

```json
"server": { "trusted_proxies": ["10.0.0.0/8"] }
```

Только для запросов с этих адресов `client_ip` берётся из `X-Real-IP` /
`X-Forwarded-For`. Остальным заголовкам не верят — иначе любой клиент подделает
себе IP в аудите одним лишним заголовком. Пустой список (по умолчанию) = не
доверять никому.

Берётся **правый** элемент `X-Forwarded-For`: nginx/Angie дописывают своего пира
справа, а левые элементы пришли от клиента и подделываются тривиально.

---

## 3. Ротация audit-журнала

Конфиг: [`deploy/logrotate/infcore`](../deploy/logrotate/infcore) → `/etc/logrotate.d/infcore`.

Шлюз держит fd журнала открытым всю жизнь процесса, поэтому режим ротации
принципиален:

| Режим | Итог |
|---|---|
| `copytruncate` | ❌ события теряются между copy и truncate |
| `create` без сигнала | ❌ шлюз пишет в переименованный файл, новый пустой — аудит молча утекает в архив |
| **`create` + SIGHUP** | ✅ единственный вариант без потерь |

По SIGHUP шлюз переоткрывает журнал по прежнему пути. Сигнал приходит из
`postrotate` через `systemctl reload` → `ExecReload=/bin/kill -HUP $MAINPID`.

Переоткрытие происходит перед записью следующего события: если событий нет,
терять нечего.

Проверить, что ротация действительно доезжает:

```bash
curl -s localhost:8080/metrics | grep audit_reopen
# infcore_gateway_audit_reopens_total         должен расти
# infcore_gateway_audit_reopen_failures_total должен быть 0
```

> Без ротации журнал растёт до упора в лимит размера файла, после чего аудит
> уходит в **fail-closed** и шлюз перестаёт отдавать трафик. Это вопрос времени,
> а не «если».

---

## 4. Ротация API-ключей

Ключи задаются через `env:VAR` и читаются **на старте**. Горячей перечитки
конфига нет: `SIGHUP` переоткрывает только журнал.

Процедура без простоя (два ключа одновременно):

1. добавить новый principal с новым ключом в конфиг, старый оставить;
2. `systemctl restart infcore-gateway` (простой = загрузка модели + sha256);
3. перевести клиентов на новый ключ; следить за
   `infcore_gateway_errors_total{type="unauthorized"}` — всплеск означает, что
   кто-то остался на старом;
4. когда всплесков нет — удалить старый principal и снова `restart`.

Кто именно ходил каким ключом, видно в аудите по `subject`.

---

## 5. Мониторинг

* scrape: [`deploy/monitoring/scrape.yml`](../deploy/monitoring/scrape.yml)
* алерты: [`deploy/monitoring/alerts.yml`](../deploy/monitoring/alerts.yml)

> ⚠️ `/metrics` в шлюзе отдаётся **без авторизации** (он рассчитан на loopback).
> Наружу выпускать нельзя: счётчики ошибок и endpoint'ов — операционная разведка.
> В `angie/infcore.conf` на `/metrics` стоит allow-список; поправьте под свою сеть.

Что снимается:

| Метрика | Зачем |
|---|---|
| `requests_total{endpoint,decision}` | трафик и решения политики |
| `request_duration_seconds` (histogram) | p95/p99; деградация под нагрузкой |
| `audit_failed` (gauge) | **1 = шлюз режет весь трафик fail-closed** |
| `audit_reopens_total` / `audit_reopen_failures_total` | работает ли ротация |
| `models_configured`, `backends_loaded` (gauge) | поднялся ли бэкенд |
| `errors_total{type}` | 401, backend_*, rate_limited и пр. |

**Пороги в `alerts.yml` требуют калибровки под ваше железо.** Порог p95 = 30 с
осмыслен для крупной модели с частичным offload; на H100 с моделью целиком в VRAM
он должен быть в разы ниже. Снимите базовую линию:

```bash
python3 infcore/tests/load/loadtest.py --url http://127.0.0.1:8080 \
    --key-file admin.key --model <модель> --concurrency 8 --requests 32
```

и правьте пороги, а не отключайте алерты.

---

## 6. Чек-лист перед боем

- [ ] `cuobjdump` показывает `arch = sm_90`
- [ ] `n_gpu_layers: -1`; после первого запроса `nvidia-smi` показывает модель в VRAM
- [ ] `server.trusted_proxies` заполнен → в аудите реальные IP клиентов, а не прокси
- [ ] `angie -t` проходит; по HTTP идёт редирект на HTTPS; сертификат от внутреннего CA
- [ ] `/metrics` снаружи **не** доступен, изнутри сети мониторинга — доступен
- [ ] `logrotate -d /etc/logrotate.d/infcore` без ошибок; после `-f`
      `audit_reopens_total` вырос, `audit_reopen_failures_total` = 0
- [ ] `require_model_integrity: true`, sha256 и size_bytes проставлены;
      проверено негативно (битый sha256 → шлюз не стартует)
- [ ] нагрузочный прогон снял базовую линию, пороги алертов откалиброваны
- [ ] алерты доезжают до дежурного (проверить `InfcoreAuditFailed` — это простой)
