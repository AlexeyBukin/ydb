# Соединение с БД

Для соединения с БД {{ ydb-short-name }} из {{ ydb-short-name }} CLI или приложения, использующего {{ ydb-short-name }} SDK, необходимо указать [эндпоинт](#endpoint) и [путь базы данных](#database).

## Эндпоинт {#endpoint}

Эндпоинт (endpoint) - строка в формате `protocol://host:port`, предоставляемая владельцем кластера {{ ydb-short-name }} для корректной маршрутизации запросов клиента к его базам данных через сетевую инфраструктуру и установки сетевого соединения. Для облачных баз данных endpoint показывается в консоли управления на странице требуемой БД, а также обычно может быть получен через CLI облачного поставщика. В корпоративных окружениях имена эндпоинтов {{ ydb-short-name }} предоставляются командой администраторов или также могут быть получены в консоли управления внутренним облаком.

{% include [overlay/endpoint.md](_includes/connect_overlay/endpoint.md) %}

Примеры:

* `grpc://localhost:7135` — протокол обмена данными без шифрования (gRPC), сервер запущен на том же хосте что и клиент, принимает соединения на порту 7135.
* `grpcs://ydb.example.com` — протокол обмена данными с шифрованием (gRPCs), сервер запущен на хосте ydb.example.com в изолированной корпоративной интрасети, принимает соединения на порту YDB по умолчанию 2135.
* `grpcs://ydb.serverless.yandexcloud.net:2135` — протокол обмена данными с шифрованием (gRPCs), публичный сервер Serverless YDB {{ yandex-cloud }} ydb.serverless.yandexcloud.net, порт 2135.

## Путь базы данных {#database}

Путь базы данных (`database`) - строка, определяющая где на кластере {{ ydb-short-name }} находится база данных, к которой адресован запрос. Имеет структуру [пути к файлу]{% if lang == "en" %}(https://en.wikipedia.org/wiki/Path_(computing)){% endif %}{% if lang == "ru" %}(https://ru.wikipedia.org/wiki/Путь_к_файлу){% endif %}, с использованием символа `/` в качестве разделителя. Начинается всегда с символа `/`.

На кластере {{ ydb-short-name }} может быть развернуто множество баз данных, и их пути определяются конфигурацией кластера. Как и эндпоинт, для облачных баз данных `database` показывается в консоли управления на странице требуемой БД, а также обычно может быть получен через CLI облачного поставщика.

При использовании облачных решений базы данных на кластере {{ ydb-short-name }} создаются и размещаются в режиме самообслуживания, не требуя участия владельца и администраторов кластера.

{% include [overlay/database.md](_includes/connect_overlay/database.md) %}

{% note warning %}

Приложения не должны как-либо интерпретировать количество и значение каталогов в `database`, так как они определяются конфигурацией кластера {{ ydb-short-name }}. Например, при работе с сервисом {{ ydb-short-name }} в {{ yandex-cloud }} `database` сейчас имеет структуру `region_name/cloud_id/database_id`, однако этот формат может быть изменен в будущем для новых БД.

{% endnote %}

Примеры:

* `/ru-central1/b1g8skpblkos03malf3s/etn01q5ko6sh271beftr` — база данных {{ yandex-cloud }} с идентификатором `etn01q3ko8sh271beftr` в облаке `b1g8skpbljhs03malf3s`, развернутая в регионе `ru-central1`.
* `/local` — база данных по умолчанию при самостоятельном развертывании [с использованием Docker](../getting_started/self_hosted/ydb_docker.md).

## Строка соединения {#connection_string}
Строка соединения (connection string) -- строка в виде URL, задающая одновременно эндпоинт и путь базы данных, в следующем формате:

\<endpoint\>?database=\<database\>

Примеры:
- grpc://localhost:7135?database=/local
- grpcs://ydb.serverless.yandexcloud.net:2135?database=/ru-central1/b1g8skpblkos03malf3s/etn01q5ko6sh271beftr

Соединение с помощью строки соединения является альтернативой раздельному указанию эндпоинта и пути базы данных, и может применяться в тех инструментах, которые его поддерживают.

## Корневой сертификат для TLS {#tls-cert}

При использовании протокола с шифрованием ([gRPC over TLS](https://grpc.io/docs/guides/auth/), или grpcs) продолжение сетевого соединения возможно только в том случае, если клиент уверен в том, что ему действительно отвечает подлинный сервер, с которым он пытается установить соединение, а не кто-то посередине, перехвативший запрос в сети. Это обеспечивается проверкой через [цепочки доверия]{% if lang == "en" %}(https://en.wikipedia.org/wiki/Chain_of_trust){% endif %}{% if lang == "ru" %}(https://ru.wikipedia.org/wiki/Цепочка_доверия){% endif %}, для работы которой на клиенте должен быть установлен т.н. корневой сертификат.

В состав операционных систем, на которых запускается клиент, уже входит набор корневых сертификатов от основных мировых центров сертификации. Однако, владелец кластера {{ ydb-short-name }} может использовать свой центр сертификации, не связанный ни с одним из мировых, что часто встречается в корпоративных окружениях, и почти всегда применяется при самостоятельном развертывании кластеров с поддержкой шифрования на соединениях. В этом случае владелец кластера должен каким-либо образом передать свой корневой сертификат для использования на стороне клиента. Такой сертификат может быть установлен в хранилище сертификатов операционной системы, где запускается клиент (вручную пользователем или корпоративной командой администраторов ОС), или встроен в состав самого клиента (как это сделано для {{ yandex-cloud }} в {{ ydb-short-name }} CLI и SDK).
