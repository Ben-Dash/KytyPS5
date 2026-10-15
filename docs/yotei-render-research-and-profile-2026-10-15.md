# Yōtei: открытые реализации и измерение узких мест, 15 октября 2026

## Результат исследования

Профиль сузил проблему до затрат на подготовку/передачу ресурсов и зависимости
CPU↔GPU. Одна функция, объясняющая весь низкий FPS, не установлена. Основной
поток действительно занимает почти одно ядро, но свежие снимки показывают
преимущественно активное ожидание в коде игры. Это не основание переписывать
дорогую HLE-функцию или просто убирать ожидания.

В поздних GPU-интервалах измерены примерно **135 мс занятости render-очереди и
140 мс промежутков между её командными буферами на кадр**. Атрибуция GPU-зон
относит **32,59 мс/кадр к buffer-copy и 20,65 мс к tiler**. Есть около **385
native submissions, 131 readback request и 21,06 packed MiB на кадр**.
Это конкретные цели следующей проверки. Между количеством копий, паузами и
конечным потребителем ещё нужно установить причинную связь; все readbacks
нельзя объявить лишними. Packed bytes — не измерение пропускной способности PCIe.

Чистый контроль: **3,80 FPS по заголовку окна**. Оптимизационная правка в этой
серии не применялась; повышения FPS, удвоения или стабильных 60 FPS не заявляю.

## Условия и запуски

Native Windows, `yotei-integration-native`, record HEAD `ce620a1f`, production
`b1a9698e`. Один EXE во всех четырёх запусках:
`E46E5CC99DAFA8B4E67177C854FA7A7DE8E8CA1391DE6AB43169F562212E8ABE`.
Текущий source `src/CMake/3rdparty` совпадает с b1. Targeted publication OFF,
остальные экспериментальные алгоритмы отсутствуют. Выход 1920×1080,
Performance preset, compute chain 1, readback queue 0, GPU core 1, recorder spin
2000, label flush 2000 us. Neutral live input, без нажатий и переходов по меню.
Совместимые pipeline caches сохранены, но одинаковые неизменяемые snapshots
cache для каждого запуска не создавались.

WSL остановлен по прежнему разрешению пользователя. Shutdown helper превысил
20 с, но затем vmmem завершился; pre-run проверка подтвердила отсутствие WSL,
Dota и другого native запуска. Профилирование не совмещалось с build/GPU tests.

| Запуск | Что включено | Начало окна | Длительность | FPS заголовка |
|---|---|---|---|---:|
| `073924/b2e5c9` | Чистый контроль | frame652, 114,62 с | 20 с | 3,80 |
| `074305/ee1163` | FunctionProfile + отдельные CPU/GPU counters | frame653, 119,53 с | 20 с; сам profiler 10,185 с | 3,59, диагностический |
| `074724/0053ef` | DrainStats10 + GPU zones, без FunctionProfile | frame650, 123,48 с | 20 с | 3,67, диагностический |
| `075115/b77dcb` | Отдельный PSS сэмплер Main, без предыдущих профайлеров | frame564, 90,89 с | 25 с; PSS 5,015 с | 3,72, диагностический |

Все четыре game runs завершились exit0, без timeout/memory guard. Профилируемые
FPS не заменяют чистый контроль; различие окон не даёт точной цены профайлера.
Проверенный runtime: `_Build/analysis/yotei-performance-cycle53-20261014/runtime-selected-targeted-publication`.

Сцену определил самостоятельно через **PrintWindow**, без переключения фокуса
или ввода. На снимке функционального запуска frame557 и снимках до/после PSS
видны персонаж, NPC, деревня, огонь, растительность и HUD. Это игровой мир.
Снимки хранятся в соответствующих run directories. Отдельный снимок чистого
контроля и GPU-zone запуска не получен; одинаковая сцена этих двух запусков
следует из одинакового сохранения/neutral input и окружавших их проверенных
запусков, а не из изображения внутри каждого измерительного окна.
Пользователь просит далее самому проверять сцену, не спрашивая его.

## Время функций и CPU потоков

FunctionProfile: frames650→686, **10,1853901 с**, 27 зарегистрированных потоков,
174 строки. Root sampling 1/64; дочерние scope наследуют решение родителя.
Независимая проверка и строгий parser подтвердили **0 query errors,
exclusive-invalid, truncated, outstanding, depth overflow и registration overflow**.

Выбранные функции `Thread_Gpu`; собственное wall-время вычитает только
размеченных детей того же потока и включает оставшиеся расходы измерения:

| Функция | Вызовы за окно | Измеренных samples | Inclusive wall, мкс/вызов | Selected-child exclusive wall, мкс/вызов |
|---|---:|---:|---:|---:|
| DrawIndex | 170797 | 2638 | 41,69 | **8,91** |
| DispatchDirect | 119429 | 1871 | 27,14 | **6,86** |
| PrepareGraphicsBindings | 164504 | 2538 | 13,25 | **4,84** |
| MaterializeResources | 263523 | 4116 | 4,29 | **3,73** |
| GetGraphicsPrograms | 171515 | 2655 | 14,20 | **3,48** |
| RebindBuffers | 382185 | 5935 | 5,32 | **2,97** |

Эти кандидаты заметно выше измерительной пробы. `SynchronizeBuffer`,
`UploadCopies` и `RecorderPayload` очень часты, но отдельные значения близки
к уровню пробы; миллион вызовов сам по себе не доказывает миллион дорогих копий.
Вложенные времена и разные потоки нельзя складывать в бюджет одного кадра.
Сырые QueryThreadCycleTime сохраняются как cycles и не переводятся в мс.

Отдельный 20,037-секундный GetThreadTimes/ProcessThread замер:

| Поток | CPU-время | Занятость одного логического ядра |
|---|---:|---:|
| Thread_Main | 17391 мс | 86,80% |
| Thread_Gpu | 9391 мс | 46,87% |
| DrawSpeculator | 6485 мс | 32,37% |
| GpuQueueSubmit | 5156 мс | 25,73% |
| Весь процесс | 66500 мс | 331,89%, то есть около 3,32 ядер |

Это около 10,37% всей 32-thread машины. Role `Other` в FunctionProfile не
подменялся названием Main: имя Main подтверждено отдельным OS thread sampler.

## Что делает загруженный Main

Self-test PSS: 39/39 корректных контекстов. Отдельный game diagnostic:
**36/36 валидных снимков за 5,015 с**, все Capture/Walk/MarkerFree/SnapshotFree
успешны, максимальный capture3,067 мс. CPU Main за это окно: 4421,875 мс,
**88,18% одного ядра**.

| Положение Main в момент снимка | Снимков |
|---|---:|
| Пять гостевых RDTSC/deadline polling loops | **29/36** |
| Native GPU synchronization wait | 3/36 |
| Native alert/thread wait | 3/36 |
| Другой гостевой код | 1/36 |

Это доли **позиций выполнения во времени наблюдения**, не доли CPU cycles и не
обещание прироста FPS. Проверен hash неизменного SELF, заново разобраны
ограниченные окна инструкций и текущие native DLL с реальными module bases.
Гостевой decode использует текущий loader base и согласованные sampled offsets;
отдельный live dump гостевого кода не снимался. PSS кратко фиксирует контексты
всех потоков, поэтому использован отдельный диагностический запуск.

Загрузка Main сама по себе не указывает на тяжёлую функцию эмулятора. Простая
замена spin на sleep может снизить CPU load, но не ускорит ещё не исполненную
зависимость. Нужна связь между producer, завершением GPU, публикацией данных
и продолжением гостевого кода.

Первая попытка применить PSS к уже завершившемуся GPU-run дала early-exit;
это **не измерение** и не ошибка эмулятора. Валидны только данные отдельного
`pss-owned-main-dedicated`.

## GPU: измеренные категории

Выбраны последние три завершённые 10-секундные периодические записи,
**109 кадров**, cumulative diagnostic frame counts607→716. Это поздние
интервалы после400; cumulative counters не объявляются точными title frames.
Средние взвешены по числу кадров, а не усреднены без весов.

| Измерение/зона | мс на кадр |
|---|---:|
| Интервалы занятости render queue | **135,23** |
| Промежутки между её command buffers | **140,00** |
| Game compute dispatch, зона | 52,57 |
| Buffer copy, зона | **32,59** |
| Tiler, зона | **20,65** |
| Game draw, зона | 15,31 |
| Image copy, зона | 6,99 |

Зоны вложены в GPU work; нельзя прибавлять их ещё раз к busy. Tick waits разных
CPU потоков также перекрываются с GPU work. GPU gap этой очереди не доказывает
простой всех engines устройства. NVML coarse sampling дал около50% GPU utilization,
SM около2,87–2,89GHz, температуры44–52°C и около47–48W за короткие окна. Это
поддерживает наблюдение неполной загрузки, но не является аппаратным ALU/PCIe trace.
VRAM residency/paging этим набором данных не исключались.

Наиболее дорогие устойчивые ключи:

- `fc6f8c56eb7e168f`: около9 мс/кадр суммарно на примерно10 compute dispatch.
- `d42889155b0a6b9f`: около6–7 мс/кадр на один compute dispatch.
- Upload-side tiler поверхностей `5067be0000` и `506bba0000`: примерно13 и6,4
  мс/кадр, по2 события. По исходникам это переупаковка полного описанного
  subresource; dirty rectangles здесь не используются.

У горячих поверхностей лог показывает несколько форматов/alias и размеры
3840×2160, затем2304×1296 и1920×1080. Ключи зон не кодируют view/extent;
нельзя приписать каждое событие конкретному alias или объявить всю игру4K.
Оба tiler ключа upload-side, tile→detile roundtrip не доказан. Отдельного
ColorTransform прохода для этих форматов код не выбирает.

## Что нашлось на GitHub и в статьях

1. [AnyPS5 c92ae6e](https://github.com/boykopovar/AnyPS5/commit/c92ae6e5c122af4fea0758e23f4535eac972d1ce):
   реальная замена CAS-loop узких buffer stores на native byte stores с tests
   и capability fallback. Автор сообщает426→10,7 мс для одного light-culling
   shader в своей версии/сцене. **У нас обычные8/16-bit stores уже native**.
   Dynamic IndirectBuffer formatted partial stores ещё могут использовать
   AtomicAnd/AtomicOr. Перенос не обоснован без текущего горячего SPIR-V.
2. SharpEmu [0e996cf](https://github.com/sharpemu/sharpemu/commit/0e996cf56c0e0fbda0898797e843971af6bc122b)
   сокращает повторные hot-page uploads, а
   [660d2d8](https://github.com/sharpemu/sharpemu/commit/660d2d8a9df8cf413d22f7c5f1f53ac881459a7f)
   ограничивает частоту timeline polls. Аналогичные epochs/synced pages и
   ограничение200us у нас уже есть; чужие47%/12% не являются нашим замером.
3. [SharpEmu f9fe5cb](https://github.com/sharpemu/sharpemu/commit/f9fe5cbeaf48ebc87cea4342afc44582ec4e7a2a)
   вводит feedback для stale eager readbacks: временно прекращает постоянно
   устаревающее чтение. Это конкретный новый reference для нашей очереди eager
   pages; сначала нужно измерить актуальные/перезаписанные токены, байты и время.
4. [AnyPS5 terrain wave64 reduction](https://github.com/boykopovar/AnyPS5/pull/1268)
   полезен для корректности. [BDA helper sharing](https://github.com/boykopovar/AnyPS5/pull/1267)
   ускоряет преимущественно компиляцию, а не доказывает steady-gameplay FPS.
5. [Sucker Punch](https://blog.playstation.com/2025/10/23/ghost-of-yotei-tech-deep-dive/)
   подтверждает compute→CPU readback→command-list assembly. В
   [GDC2026 slides](https://media.gdcvault.com/gdc2026/Slides/Wohllaib_Eric_Ambient_Diffuse_Lighting_in_Ghost_of_Yotei.pdf)
   есть сведения о CPU/GPU geometry paths и максимальных allocations при DRS.
   Проверены session page и индексируемые фрагменты, полный PDF reader/download
   не открыл. Это не источник точных timings нашего запуска.
6. [Khronos GPU culling / indirect draw sample](https://github.com/KhronosGroup/Vulkan-Samples/blob/main/samples/performance/multi_draw_indirect/README.adoc)
   — открытая реализация похожего принципа для независимых backend tests.
   [NVIDIA live GPU Trace](https://docs.nvidia.com/nsight-graphics/UserGuide/gpu-trace-overview.html)
   отделяет ожидания/подачу от фактических GPU passes; API replay не воспроизводит
   исходную CPU нагрузку. Это методический reference, Nsight trace здесь не снимался.

SharpEmu PR926 объединяет сотни изменений; совместимость чужого запуска не
доказывает одинаковый FPS у нас. Публичный `peepostarexx/KytyPS5` main/YOTEI
сейчас возвращает404, свежие commits проверить не удалось. Открытый полный
исходник renderer Yōtei в этом ограниченном наборе источников не найден.

Исторический точный `fc6f…live.spv` от cycle24 содержит12 CAS, все Workgroup/LDS,
и2 StorageBuffer AtomicOr; старый d428 без этих atomics. Это **старые modules**,
тождество текущему запуску не доказано. Их нельзя выдавать за свежий dump или
основание для SSBO-byte-store фикса.

## Следующие приоритеты исправлений

1. Разделить горячие uploads по image/view/extent и dirty generation. На
   synthetic alias/partial-write regression проверить возможность повторного
   использования либо обновления только допустимых областей. Начинать с двух
   измеренных поверхностей как reproductions, исправлять общий ресурсный путь.
2. Добавить диагностический учёт token generations для eager readbacks и
   связать demand wait с producer/native completion/publication. Проверить
   stale-prefetch admission и группировку submissions по реальным зависимостям.
   Обязательные demand reads и актуальность GPU/CPU данных сохраняются.
3. Взять **точные текущие** SPIR-V permutations двух дорогих compute shaders;
   различить narrow SSBO, dynamic formatted stores и LDS atomics. Только после
   независимого ISA regression выбирать соответствующий общий backend fix.

Эти пункты — вывод исследования, в данной серии production исправлений нет.
Кандидат на критическую цепочку: повторная подготовка ресурсов и синхронизация,
мешающие перекрытию CPU/GPU work. Точный устранимый вклад каждого звена и
ожидаемый прирост надо доказать контрольным изменением;135+140 мс сами по себе
не доказывают возможность гарантированного ускорения вдвое.

## Ограничения таймеров и воспроизводимость

- FunctionProfile фиксированно начинается на650-м кадре на10с, независимо от
  wrapper90s gate. `ProcessPm4` и `RecorderIdleSpin` не имеют реальных Scope.
- Exclusive — только размеченные same-thread children; probe overhead остаётся.
- DrainStats frame histogram насыщается100мс: p95=100 означает≥100мс.
- Sync `QueueLockWait` включает предшествующий drain. GPU zones добавляют
  BOTTOM_OF_PIPE timestamps и не являются бесплатным аппаратным trace;
  dropped queries/other queues полностью не покрываются.
- WPR доступен, но текущий shell без Administrator; чужая запись отсутствовала.
  Elevation, driver install и Nsight capture не выполнялись.

Исходные run directories в `_Build/runs/` имеют префиксы таблицы выше.
В каждом сохранены run.json, stdout/stderr и title-fps.jsonl. Собранная сводка:
`_Build/analysis/yotei-performance-cycle54-20261015/measurements.json`; script
`analyze-cycle54.py` проверяет hashes/exit/errors и взвешивает GPU-интервалы.
Полные отчёты трёх агентов в `github-research`, `articles-research`, `timer-audit`;
там же точные source links, native module hashes и bounded offline disassembly.
Исходный SELF или его инструкции в tracked files не добавлялись.

Четыре bounded game runs, PSS self-test и parsers завершены. Task-owned
processes закрыты; untracked пользовательский `ComputePrefetchPermutationTests.inc`
сохранён. Новая сборка не требовалась, production unchanged, push/issue comments
не выполнялись. Проверки сцен далее выполняются самостоятельно по снимкам.
