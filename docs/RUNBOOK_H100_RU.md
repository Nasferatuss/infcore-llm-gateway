# infcore — пошаговый запуск на сервере с H100 (терминал)

Копируемая последовательность «с нуля до рабочего HTTPS-эндпоинта» для bare-metal/VM
с systemd. Всё выполняется по SSH.

* **Зачем** так, а не иначе — [DEPLOY_H100_PROD.md](DEPLOY_H100_PROD.md).
* **Справочник** по конфигу, портам, аудиту — [DEPLOY.md](DEPLOY.md).
* Docker-путь — `deploy/compose/docker-compose.yml` (нужен внутренний реестр).

Обозначения: `$` — от обычного пользователя, `#` — от root (`sudo -i`).

> **Сервер пока без видеокарты?** Контур можно обкатать на CPU — см.
> [приложение А](#приложение-а--обкатка-на-cpu-без-gpu) в конце. От GPU зависит
> только скорость генерации; конфиг, ключи, RBAC, аудит, TLS, ротация и метрики
> проверяются полностью.

---

## 0. Проверка сервера (до всего)

```sh
nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv
#   ожидаем: NVIDIA H100 ..., 81559 MiB, драйвер 535+

nvcc --version | tail -1      # CUDA toolkit нужен только для СБОРКИ
cmake --version | head -1     # нужен >= 3.21
nproc; free -g; df -h /opt /var/log
```

Места нужно: ~30 ГБ на сборку + размер моделей в `/opt/infcore/models` + запас под
audit-журнал в `/var/log`.

> Если `nvidia-smi` не отвечает — дальше идти бессмысленно, чините драйвер.

---

## 1. Код

```sh
$ git clone --depth 1 https://gitverse.ru/nasferatus/llama.cpp infcore-src
$ cd infcore-src
$ git rev-parse --abbrev-ref HEAD      # ожидаем: infcore
```

`git checkout` не нужен: в репозитории **одна ветка `infcore`**, и HEAD указывает
на неё — клон сразу даёт нужное состояние.

Ветка содержит **весь** исходник llama.cpp плюс каталог `infcore/`; движок в ней
побайтово совпадает с апстримом (правило «обернуть, не трогая ядро»). Отдельно
что-то доносить не нужно — это самодостаточное дерево.

`--depth 1` экономит время и диск: для сборки история не нужна. **Но** для слияния
обновлений апстрима (`infcore/scripts/update-upstream.sh`) нужен полный клон —
делайте его на машине сборки/разработки, а не на боевом сервере.

В закрытом контуре — с внутреннего зеркала или из архива; ветка та же.

---

## 2. Сборка под H100

**Не берите команду сборки из `WINDOWS_WSL_MSI_TEST_PLAN_RU.md`** — там `sm_89`
(RTX 4070), на H100 такой бинарь не несёт CUDA-ядер.

```sh
$ cmake -S infcore -B build -C infcore/cmake/profile-h100.cmake
$ cmake --build build -j"$(nproc)"
```

Занимает десятки минут. Артефакты: `build/bin/{infcore_gateway,llama-server,infcore-cli}`.

**Обязательно проверьте, что внутри Hopper:**

```sh
$ cuobjdump build/bin/llama-server | grep -oE 'arch = sm_[0-9]+' | sort -u
# ожидаем ровно: arch = sm_90
$ ldd build/bin/infcore_gateway | grep -i 'not found' && echo "СЛОМАНО" || echo "зависимости ок"
```

---

## 3. Пользователь и каталоги

```sh
# useradd -r -s /usr/sbin/nologin infcore
# install -d -o infcore -g infcore /opt/infcore/bin /opt/infcore/config /opt/infcore/models
# install -d -o root -g root -m 0750 /etc/infcore
```

`/var/log/infcore` создавать **не нужно** — его делает systemd (`LogsDirectory=`).

Бинарники:

```sh
# install -o root -g root -m 0755 \
    build/bin/infcore_gateway build/bin/llama-server build/bin/infcore-cli \
    /opt/infcore/bin/
```

---

## 4. Модели

### Сначала мелкая

**Не начинайте с боевой модели на десятки ГБ.** Прогоните весь путь (шаги 5–11) на
модели ~1–2 ГБ: сборка, конфиг, systemd, TLS, аудит, метрики проверяются за минуты.
Ошибку в конфиге лучше найти до многочасовой заливки, а не после.

### Как доставить

**А. rsync со своей машины** (интернет на сервере не нужен):

```sh
$ rsync -avP <локальный>.gguf user@server:~/staging/
```

`scp` не годится: он не умеет докачку, а многогигабайтная передача рвётся легко.
`-P` = `--partial --progress`; после обрыва повторите **ту же команду** — продолжит
с места. Запускайте под `tmux`/`screen`, иначе разрыв SSH убьёт передачу.

Ориентир по времени: 27 ГБ ≈ 216 Гбит → при аплоуде 100 Мбит/с ~36 мин, при
20 Мбит/с ~3 часа. Домашний канал асимметричен, упрётесь в него.

**Б. Скачать прямо на сервере** — если у него есть интернет, канал ЦОДа быстрее
вашего аплоуда в разы. Разовое окно наружу не конфликтует с `enforce_no_egress` и
`IPAddressDeny=any`: они ограничивают шлюз, а не `curl` от имени администратора.

**В.** В полностью закрытом контуре — внутреннее зеркало или физический носитель.

### Установка и проверка

```sh
$ df -h /opt                       # места хватает? нужен размер модели + запас
# install -o infcore -g infcore -m 0440 ~/staging/*.gguf /opt/infcore/models/
$ sha256sum /opt/infcore/models/*.gguf
$ stat -c '%n %s' /opt/infcore/models/*.gguf     # size_bytes
```

Отдельный `install` от root нужен потому, что `/opt/infcore/models` принадлежит
`infcore` и SSH-пользователь туда не пишет.

> **Сверьте sha256 с источником.** Совпадение размера целость **не** доказывает:
> оборванный rsync оставляет файл нужной длины. Хеш всё равно нужен для конфига
> (шаг 6), так что шаг неизбежный.

---

## 5. Ключи API

```sh
# umask 077
# cat > /etc/infcore/gateway.env <<EOF
INFCORE_KEY_ADMIN=$(openssl rand -hex 32)
EOF
# chown root:infcore /etc/infcore/gateway.env
# chmod 0640 /etc/infcore/gateway.env
```

Заглушки и слабые значения шлюз отвергает на старте — генерируйте только так.
Свой ключ посмотрите один раз (`cat`) и положите в хранилище секретов.

---

## 6. Конфиг

```sh
# cp infcore/config/gateway.yaml /opt/infcore/config/gateway.yaml
# chown root:infcore /opt/infcore/config/gateway.yaml && chmod 0640 /opt/infcore/config/gateway.yaml
# ${EDITOR:-vi} /opt/infcore/config/gateway.yaml
```

Файл в репозитории — **пример**, не прод-конфиг. Правки, обязательные для H100:

| Поле | Из примера | Ставим | Почему |
|---|---|---|---|
| `models[].n_gpu_layers` | `999` | **`-1`** | `-1` = не передавать флаг, llama.cpp сам подгонит offload. Любое явное число отключает авто-подгонку. |
| `models[].gguf_path` | пример | реальный путь | — |
| `models[].sha256`, `size_bytes` | нет | из шага 4 | без них нечего проверять |
| `offline.require_model_integrity` | `false` | **`true`** | иначе подмена весов не заметится |
| `server.trusted_proxies` | нет | **`["127.0.0.1"]`** | Angie на том же хосте; без этого в аудите у **каждой** записи будет `client_ip` прокси |
| `models[]` лишние | 3 модели | оставить нужные | у ненужных `"enabled": false` |
| `security.principals` | 3 шт. | под свои ключи | лишние удалить вместе с ролями |

`n_ctx` и `runtime.max_loaded_models` — под свою задачу: 80 ГБ VRAM позволяют и
большой контекст, и несколько моделей сразу.

Проверить конфиг **до** установки сервиса (шлюз валидирует его по встроенной
JSON-Schema и при ошибке печатает все проблемы и не стартует):

```sh
# set -a; . /etc/infcore/gateway.env; set +a
# /opt/infcore/bin/infcore_gateway /opt/infcore/config/gateway.yaml
# Ctrl+C после успешного старта
```

---

## 7. systemd

```sh
# cp infcore/deploy/systemd/infcore-gateway.service /etc/systemd/system/
# systemctl daemon-reload
# systemctl enable --now infcore-gateway
# systemctl status infcore-gateway --no-pager
```

> В юните стоит `IPAddressDeny=any` + `IPAddressAllow=localhost`. Для схемы
> «Angie на этом же хосте» этого достаточно. Если клиенты ходят на шлюз напрямую,
> без прокси — допишите их подсеть в `IPAddressAllow`, иначе трафика не будет.

### Дымовой тест

```sh
$ KEY=$(sudo sed -n 's/^INFCORE_KEY_ADMIN=//p' /etc/infcore/gateway.env)
$ curl -s localhost:8080/health
$ curl -s localhost:8080/v1/models -H "Authorization: Bearer $KEY"
$ curl -s localhost:8080/v1/chat/completions -H "Authorization: Bearer $KEY" \
    -H 'Content-Type: application/json' \
    -d '{"model":"<logical_name>","messages":[{"role":"user","content":"2+2?"}]}'
```

Первый запрос поднимает бэкенд и проверяет sha256 — на крупной модели это десятки
секунд, это нормально. Проверьте, что модель уехала в VRAM:

```sh
$ nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv
```

Если пусто — модель считается на CPU, смотрите `n_gpu_layers` и журнал юнита.

---

## 8. TLS

Сертификат — от **внутреннего CA**: ACME/Let's Encrypt в offline-контуре невозможен.

```sh
# install -d -m 0750 /etc/angie/tls
# install -m 0644 infcore.crt /etc/angie/tls/infcore.crt   # fullchain
# install -m 0600 infcore.key /etc/angie/tls/infcore.key
# chown -R root:angie /etc/angie/tls

# cp infcore/deploy/angie/infcore.conf /etc/angie/http.d/
# ${EDITOR:-vi} /etc/angie/http.d/infcore.conf
```

Поправить два места: `server_name` (должен совпадать с CN/SAN сертификата) и
`allow` в `location = /metrics` (подсеть мониторинга).

```sh
# angie -t && systemctl reload angie
$ curl -sI http://<FQDN>/            # ожидаем 308 на https
$ curl -s https://<FQDN>/health --cacert /path/to/internal-ca.crt
$ curl -s https://<FQDN>/metrics --cacert ... -o /dev/null -w '%{http_code}\n'   # ожидаем 403
```

Проверьте, что аудит видит **реального** клиента, а не прокси:

```sh
# tail -1 /var/log/infcore/audit.log | grep -o '"client_ip":"[^"]*"'
```

Если там `127.0.0.1` при запросе с другой машины — не заполнен `trusted_proxies` (шаг 6).

---

## 9. Ротация журнала

```sh
# cp infcore/deploy/logrotate/infcore /etc/logrotate.d/infcore
# logrotate -d /etc/logrotate.d/infcore     # dry-run, без ошибок
# logrotate -f /etc/logrotate.d/infcore     # принудительный прогон
$ curl -s localhost:8080/metrics | grep audit_reopen
#   infcore_gateway_audit_reopens_total          вырос
#   infcore_gateway_audit_reopen_failures_total  = 0
```

Если `reopens_total` не растёт — `postrotate` не доезжает до шлюза, и журнал
продолжит писаться в переименованный файл. Это тихая потеря аудита, чините сразу.

---

## 10. Мониторинг

```sh
# cp infcore/deploy/monitoring/scrape.yml /etc/vm/scrape.yml
# cp infcore/deploy/monitoring/alerts.yml /etc/vm/alerts.yml
```

В `scrape.yml` подставьте адрес шлюза во внутренней сети. `/metrics` наружу не
выпускать: он отдаётся **без авторизации**.

**Пороги алертов надо откалибровать** — они писались под 35B с частичным offload на
8 ГБ. На H100 порог `p95 > 30s` не сработает никогда. Снимите базовую линию:

```sh
$ python3 infcore/tests/load/loadtest.py --url http://127.0.0.1:8080 \
    --key-file <файл с ключом> --model <logical_name> --concurrency 8 --requests 32
```

и правьте `alerts.yml` по факту, а не отключайте алерты.

---

## 11. Эксплуатация

```sh
# systemctl status infcore-gateway
# journalctl -u infcore-gateway -f
# systemctl restart infcore-gateway     # после правки конфига/ключей
# systemctl reload  infcore-gateway     # ТОЛЬКО переоткрытие журнала, конфиг НЕ перечитывается
$ curl -s localhost:8080/metrics | grep -E 'audit_failed|backends_loaded'
```

`infcore_gateway_audit_failed 1` = писатель журнала упал, шлюз режет **весь** трафик
(fail-closed, обычно ENOSPC на `/var/log`). Это простой, а не предупреждение.

### Смена ключей — только через restart

Горячей перечитки конфига нет. Без простоя — в два прохода:

1. добавить нового principal с новым ключом, старого оставить → `restart`;
2. перевести клиентов; следить за `errors_total{type="unauthorized"}`;
3. когда всплесков нет — удалить старого principal → `restart`.

---

## 12. Чек-лист перед боем

- [ ] `cuobjdump` → `arch = sm_90`
- [ ] `n_gpu_layers: -1`, после первого запроса модель видна в `nvidia-smi`
- [ ] `require_model_integrity: true`, sha256/size_bytes проставлены
- [ ] `trusted_proxies` заполнен → в аудите реальные IP клиентов
- [ ] по HTTP редирект на HTTPS; `/metrics` снаружи недоступен (403)
- [ ] `logrotate -f` → `audit_reopens_total` вырос, `reopen_failures_total` = 0
- [ ] базовая линия снята, пороги алертов поправлены
- [ ] алерты доезжают до дежурного (проверьте `InfcoreAuditFailed`)

---

## Приложение А — обкатка на CPU (без GPU)

Пока GPU нет, контур можно пройти целиком на обычном сервере. **От GPU зависит
только скорость генерации.** Конфиг, ключи, RBAC, аудит, systemd-усиление, TLS,
`trusted_proxies`, ротация, метрики и алерты проверяются полностью — и ломаются
там же, где сломались бы на H100.

### Что меняется

| Шаг | На CPU |
|---|---|
| 0. Проверка GPU | **пропустить** — `nvidia-smi` проверять нечего |
| 2. Сборка | `-C infcore/cmake/profile-cpu.cmake`; CUDA toolkit **не нужен** |
| 2. `cuobjdump` | **пропустить** — CUDA-ядер нет по определению |
| 4. Модель | только мелкая (~1–2 ГБ) |
| 6. `n_gpu_layers` | **оставить `-1`** — менять не нужно, см. ниже |
| 7. `nvidia-smi` после запроса | пропустить |

```sh
$ cmake -S infcore -B build -C infcore/cmake/profile-cpu.cmake
$ cmake --build build -j"$(nproc)"
```

Профиль существует ровно ради одной строки — `LLAMA_BUILD_SERVER=ON`. При
встраивании через `add_subdirectory` `llama-server` по умолчанию **не собирается**,
и «голая» команда `cmake -DGGML_CUDA=OFF ...` даёт шлюз без бэкенда: он стартует,
проходит health-check и отвечает **502 на каждый запрос** к модели.

### Почему `n_gpu_layers: -1` менять не надо

`-1` = «не передавать флаг бэкенду». В CPU-сборке GPU-слоёв нет вовсе, llama.cpp
просто считает всё на CPU. Тот же конфиг потом переедет на H100 без единой правки.

### Только мелкая модель

Без GPU **все** веса живут в RAM: боевой модели на 25 ГБ нужно 25+ ГБ свободной
памяти (`free -g`), а скорость будет единицы токенов в секунду. Для проверки
контура это не даёт ничего сверх того, что покажет модель на 1–2 ГБ.

Для Docker-пути: `mem_limit` на CPU = **весь** размер модели плюс запас на KV-кэш,
а не «модель минус VRAM», как в комментарии `docker-compose.yml`.

### Чего этот прогон НЕ проверяет

Здесь честно, чтобы потом не было сюрприза на H100 — там будет **другой бинарь**:

* сборку под `sm_90` и наличие CUDA-ядер;
* авто-подгонку offload (`n_gpu_layers: -1` на CPU не делает ничего);
* реальную скорость и, значит, **пороги алертов** — базовую линию придётся
  снимать заново на GPU;
* объём VRAM и поведение при её нехватке.

Иначе говоря: CPU-прогон закрывает контур и обвязку, но не движок на целевом железе.
