# Ghost of Yōtei: исследование прыгающих текстур

Состояние на 2026-10-11, ветка `yotei-integration-native`.
Пользователь разрешил тесты; игру запускать только после отдельного указания.

## Что уже проверено

При переполнении translation-буфера прежний allocator сбрасывал все таблицы,
включая активные привязки. Исправление `6d08d859` сохраняет активные диапазоны,
освобождая завершённые незакреплённые регионы. Настоящий native-тест прошёл
RED → GREEN; отдельно проверены рост, отмена подготовки и старые feedback.

После этой правки изображение всё ещё искажалось. Отключение автоматического
ввода и compute-chain не устранило артефакты: сохранены кадры512/704 запуска
`_Build/runs/game-20261011-185229-integration-manual-conservative-8902fe`.
Это ограничение результата; рабочая картинка пока не подтверждена.

Следующий независимый дефект воспроизведён через настоящий `CommitBindings`:
повторная bindless-привязка оставляла access tick1 при текущем34, тогда как
обычная привязка обновляла его до34. Отметка используется при освобождении
перекрывающихся ресурсов. Один вызов общего `MarkImagesAccessed` после
существующих проверок исправляет access tick и LRU. Неизменённый тест и
соседние случаи прошли; визуальный эффект этой второй правки ещё не проверялся.

## Что можно использовать из других репозиториев

| Источник и проверенная ревизия | Полезный механизм | Вывод для нашей ветки |
|---|---|---|
| [YOTEI-BRANCH, 6d041035](https://github.com/peepostarexx/KytyPS5/blob/6d04103525b24559a496612ef8959a7666ce9a20/src/graphics/host_gpu/renderer/pipeline/descriptors.cpp) | Тот же bindless lookup и переход layout без обновления access tick в commit. | Готовой правки этого дефекта там нет. Запуск на другом ПК не доказывает корректность каждого случая lifetime. |
| [SharpEmu PR926, 95648c2b](https://github.com/foufouadi/sharpemu/blob/95648c2b047fce1686345d09dce1a546fe0d35ee/src/SharpEmu.Libs/Gpu/Pipelines/BindlessImageHeap.cs#L344) | При invalidation новый lookup уже запрещён, но очистка дескриптора и повторное использование слота ждут completion. | Конкретный образец для отдельного исправления pending-consumer lifetime. Перенос должен учитывать и наши изменяемые translation keys. |
| [AnyPS5, d70b8998](https://github.com/boykopovar/AnyPS5/blob/d70b89989473ba1f6ae13e44e079e67f1f8a44b0/core/libs/prx/libSceAgcDriver/Graphics/src/TextureCache.cpp) | MRU на cache hit и сохранение объекта при внешнем shared ownership; [ShaderResources](https://github.com/boykopovar/AnyPS5/blob/d70b89989473ba1f6ae13e44e079e67f1f8a44b0/core/libs/prx/libSceAgcDriver/Graphics/src/ShaderResources.cpp) учитывает поколения и содержимое источника. | Полезны инварианты идентичности и владения. Точный recorder/fence lifetime в этом сравнении не подтверждён; прямой перенос backend не обоснован. |

Сравнение относится к указанным ревизиям. Подробные первичные ссылки и
ограничения сохранены в
`_Build/analysis/yotei-integration-20261011/fork-texture-use-retirement-review-20261011.md`.

## Следующие исправления и проверки

1. Проверить отдельным bounded-тестом, что записанный GPU-потребитель продолжает
   читать свою текстуру после invalidation до завершения его submission.
   Нужны реальный sampled readback и положительный контроль. Затем переносить
   принцип отложенной очистки и reuse из SharpEmu с проверкой поколений.
2. Проверить protection во время подготовки: более поздняя стадия или target
   acquisition не должна уничтожить ранее подготовленную bindless-текстуру.
   Только после RED расширять общий путь `BindImage`/rebind и cleanup при abort.
3. После указания пользователя собрать актуальный native core в новый отдельный
   runtime и проверить несколько последовательных кадров. Автоввод оставлять
   отключённым; текущий установленный `runtime-bindless-lifecycle` содержит
   только первую lifecycle-правку и не годится для проверки второй.

Запуски, ревизии, SHA256 и текущие ограничения ведутся в
[основном плане](ghost-of-yotei-windows-plan.md) и
[долге тестирования](emulator-test-debt.md).
