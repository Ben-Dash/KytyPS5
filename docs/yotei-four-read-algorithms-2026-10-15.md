# Четыре эксперимента обработки команд и чтения GPU

Серия завершена: прежний эксперимент и три новых варианта от трёх агентов.
Дальнейшие алгоритмы и запуски игры в эту серию не входят. Все изменения
проверялись отдельно, с настоящим RED до изменения производственного кода.

## Результат

| Вариант | Проверка механизма | FPS по заголовку окна | Решение |
|---|---|---|---|
| Запись indirect draw без остановки producer | CPU RED/GREEN; native ON/OFF: количество вершин, все пиксели и lifetime корректны | Контроль: среднее 3,58; ON отсечён при 3,4 | Откат e3f22a9c |
| Пакетное чтение выбранных страниц разных владельцев | Неизменённый GREEN: точные байты, один staging/token/host barrier, отдельные native copies по владельцам | ON отсечён при 3,5 против контроля 3,58 | Откат a1c414dc |
| Адресная публикация завершённого чтения | RED/GREEN; новые записи, изображения, смешанные страницы и старый широкий publisher проверены | Одна завершённая выборка ON: 3,8333; OFF на той же сборке: 3,60 | Сохранён отдельно, default OFF |
| Приостановка чтения GPU-owned PM4 до настоящей публикации | Неизменённый GREEN: другая очередь работает, header/payload и маркеры корректны, waits 0/0/0; четыре соседних режима PASS | ON отсечён при 3,6 против последнего контроля 3,60 | Откат 3dfd4664 |

Значения отсечения — отдельные наблюдения, **не средний FPS** незавершённой
выборки. Разница адресного варианта около 6,5% получена из одного короткого
окна на режим. Она пока не доказывает устойчивое ускорение. Удвоение исходных
3,7 до 7,4 FPS и стабильные 60 FPS не достигнуты.

## Условия измерения и ограничения

Native Windows, RTX 5060 Ti / Ryzen 9 5950X, выход 1920×1080, полный renderer,
compute chain 1, readback queue 0, GPU core 1, recorder spin 2000, label flush
2000 us, Performance preset, neutral live input. Меню и кнопки не использовались.
Профилировщики, AGE/BDA/GPU-zone дампы и Vulkan/shader validation отключены.
WSL и Dota отсутствовали перед последним запуском. Совместимые pipeline caches
сохранены; неизменяемые снимки cache для каждого режима не делались.

Окно измерения: frame >= 400, минимум 90 секунд от старта, затем 6 секунд;
при первом FPS <= контролю эксперимент прекращался. Все game runs exit 0,
без memory guard или timeout. Эта задержка нужна для исключения ранней загрузки.
Автоматического изображения и доказательства одинаковой игровой сцены для
этой серии нет. 1080p окна не доказывает внутреннее разрешение. Сообщение
пользователя о персонаже/мире относится к предыдущему recovery, не этой серии.

## Источники и артефакты

Все пути ниже относительно G:\repos\KytyPS5. UTC-время артефактов 14 октября
соответствует местной дате 15 октября для последних запусков.

- `_Build/runs/game-20261014-202651-perf-cycle52-indirect-record-control0-6a668b` — старый контроль.
- `_Build/runs/game-20261014-203006-perf-cycle52-indirect-record-on1-2cfcd6` — indirect ON, candidate c7c199fd, EXE 9806136746FA2D90788D0CD27AD5B9648A06A5015BCFBDC3302B7C7CC51AFA63.
- `_Build/runs/game-20261014-205520-perf-cycle53-packed-readback-on1-beb2d6` — packed ON, f9fc0a9e, EXE E9BB9F9905147F3728ADC203F00EFD920D563931F96F4FD562F57614EB480FD3.
- `_Build/runs/game-20261014-211350-perf-cycle53-targeted-publication-on1-87a248` и `game-20261014-211625-perf-cycle53-targeted-publication-off-control-320d63` — targeted 941fc65f ON/OFF; оба EXE 0A95CCA6A82FA9AA458B66DCE0E6A614CE5A11FD2D3105253B35B31A4D97D275.
- `_Build/runs/game-20261014-212312-perf-cycle53-pm4-nonblocking-fetch-on1-117d59` — PM4 532a54db, EXE D9F916EBBDA5698CC71704EE93F0CADC5573FC186AD98BA11728B0C6ABC8B12D.
- Native тесты: `_Build/analysis/yotei-performance-cycle1-20261012/checks/`; сборки: `_Build/analysis/yotei-integration-20261011/checks/`. Команды, environment, hash, stdout/stderr и exit сохранены в run.json и соседних файлах.

Targeted: RED203705 → неизменённый GREEN210105/C20BDE31; neighbors210637,
210638,210710,210711 PASS; older-wider211230/FD7CD216 PASS. Initial210941
ошибочно требовал общего owner ещё и для независимого B: это ошибка fixture,
не RED эмулятора. Подготовка owner A+C исправлена; числовые и ordering oracles
сохранены. Native A2 и CPU backing A2 совпадают после всех поздних callbacks.

PM4: initial203703 device loss вызван неверным VkBufferCopy в самом fixture,
не считается RED. CPU-clean ObtainBuffer вернул ActiveStream, а helper потерял
returned offset. Добавлены проверки границ **до** copy и acquisition настоящего
зарегистрированного owner. Resource plan205059 PASS; один исправленный bounded
GPU retry205131/2A82 дал настоящий RED. Неизменённый GREEN212021/83AF97CB,
queued WAIT212038, image aliases212039, fresh parser212147/212148 PASS.

## Сохранённое состояние

Production b1a9698e совпадает с протестированным targeted941fc65f по
src/CMake/3rdparty. Вариант включается только точным
`KYTY_READBACK_TARGETED_PUBLICATION=1`; штатный default OFF. Остальные три
production эксперимента удалены. Коммиты и reusable synthetic fixtures сохранены.
Сочетание нескольких новых алгоритмов не проверялось и не оставлено в core.

Архив проверенного targeted runtime:
`_Build/analysis/yotei-performance-cycle53-20261014/runtime-targeted-publication`.
Его EXE 0A95 сохранён. Финальный core212529/install212808 на b1a9698e PASS;
runtime `runtime-selected-targeted-publication`, EXE
E46E5CC99DAFA8B4E67177C854FA7A7DE8E8CA1391DE6AB43169F562212E8ABE
проверен сборкой, в игре не запускался; замер выше относится к архивному941/0A95.
Untracked пользовательский `tests/ComputePrefetchPermutationTests.inc` сохранён.
Публикаций на GitHub и push не было.

Остаётся доказать, насколько часто реальные кадры Yotei используют эти пути и
какова их устранимая стоимость. Logical readback bytes и перекрывающиеся waits
не доказывают главный bottleneck. Concurrent once-claim, GPU-owned nested/
predicated/legacy payload и другие игры остаются [test debt](emulator-test-debt.md).
Публичные SDK/engine материалы собраны в [отдельной заметке](yotei-public-developer-references.md).
