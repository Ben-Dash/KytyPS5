# Yōtei: новые направления оптимизации, раунд 50

14 октября 2026, native Windows, `yotei-integration-native`.
Начальный HEAD `52fef513`, production source `42c6c601`.
Три параллельных исследования завершены. Production не изменён; игровой
прирост этим раундом не измерен. Добавлен ручной CPU cost regression.

## Существенная помеха в предыдущем сравнении

Read-only проверка обнаружила Dota 2, PID35348, запущенную в21:46:30 по Минску
(18:46:30 UTC). Предыдущий diagnostic575166 начался в18:32 UTC, а clean49 —
после19:08 UTC. Значит, чужая игра появилась между этими наблюдениями.

Без эмулятора первое чтение NVIDIA показывало37% GPU и4907 MiB из16311 MiB.
Повторные чтения в19:48:20–21 UTC показывали33–36% и5455 MiB. Это общий
GPU фон, не независимое измерение вклада Dota. Пять последних чтений занимают
около0,32 секунды; это короткий snapshot, не длительный мониторинг.
Артефакт: `cycle50/background-observation/gpu-without-emulator.json`.

Нельзя приписывать всю разницу3,1→0,95 FPS эмулятору, DrainStats или Dota:
такой причинный эксперимент не проведён. OFF/ON очереди чтения остаётся
неудачной попыткой при тогдашних условиях; это не чистое сравнение с прежними
диагностическими запусками. Пользователь решил закрыть Dota самостоятельно.
Другие процессы не останавливались; пока она работает, GPU/FPS замеры отложены.

## Три конкретных кандидата

| Направление | Что найдено | Что уже доказано | Следующая проверка |
|---|---|---|---|
| Запись nonindexed indirect draw | `vkCmdDrawIndirect` попадает в fallback и вызывает `CommandStream::Drain`; indexed-вариант уже копируется в очередь | Реальный host-only regression подтверждает один producer drain при правильных аргументах, порядке и cleanup | Частота именно этой native-команды в Yōtei, затем unchanged GREEN и соседи |
| Двухбайтовый tiler | Наш fallback делает `atomicAnd` + `atomicOr` на каждый элемент; GTA-форк использует 16-битные storage load/store | Код донора проверен, нужная feature у нас уже включена; минимальный proposal подготовлен | Реальная причина direct-upload refusal и finite numeric/cost RED для выбранной BPE=2 специализации |
| Доказанный BDA/DMA read footprint | Глобальная синхронизация может загружать CPU-dirty страницы вне области чтения текущего shader | В нашей реализации нет полного read footprint; существующие ranged uploads/HotWrites можно использовать | Сначала объём BDA upload и страницы, которые реально можно исключить; затем A/D regression |

Наличие лишней работы ещё не определяет её долю кадра. Никакой кандидат
не выбирает поведение по названию игры, shader hash или адресу захвата.

## 1. Nonindexed indirect: подтверждённый CPU cost RED

В `commandHooks.cpp` команда находится в `KYTY_FALLBACK_COMMANDS`; fallback
дожидается своего recorder перед вызовом native API. В `renderDraw.cpp` есть
реальный call site для GPU args, неиндексируемого draw и выключенного mesh path.
Команда имеет пять копируемых аргументов; GPU читает draw parameters из buffer
при исполнении, а не CPU при записи. [Контракт Vulkan](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawIndirect.html).

Добавлен `tests/CommandStreamDrawIndirectRecordingTests.inc`, ручной режим
`bindless_lifecycle_tests --command-stream-draw-indirect-recording-cost`.
Он выполняется до Config/Vulkan initialization, использует настоящие production
hooks и CommandStream, контролируемый consumer и forwarding stub. Проверяет
все пять аргументов, включая64-битный offset, порядок1→2→3, forwarding после
предыдущей работы, завершение обоих workers, Drain/destructor и восстановление
environment. GPU data/execution этим тестом не моделируются.

Native build194126 PASS; первая reproduction194322 достигла правильного
финального cost assert, но buffered stdout не сохранился. После единственного
logging изменения `fflush(stdout)` native build194503 PASS; logged RED194639,
EXE `EABD645F623E045189900C12D69FBC10EAEA5A6B8A18EBFA00915C197708AA68`:
`fiveArguments=1`, `order=1`, `forwardAfterGate=1`, оба workers joined,
`streamDrained=1`, `timeouts=0`, `envRestored=1`, `actualProducerDrains=1`.
Все функциональные и cleanup controls проходят перед ожидаемым count1→0
cost failure. Exit-1073740791 — явный последний Require/abort, не driver hang.

Это ожидаемый RED ручного исследовательского режима, он не добавлен в CTest.
Production proposal с opt-in copied hook остаётся неприменённым. GREEN,
native GPU drawing и FPS ещё не проверены. Старые indirect/meshargs totals
не дают число именно `vkCmdDrawIndirect`: actual Yōtei hit пока неизвестен.
Сначала отдельный bounded producer-side счётчик этой API, без consumer double
count и per-call logging. При нулевых вызовах это не кандидат для текущего FPS.

Более широкий lead — pool lifecycle hooks с DrainAll для всех recorder —
отложен: [Vulkan синхронизирует конкретный pool](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html#commandbuffers-pools),
но наш registry пока не знает ownership pool. Без него удаление общего Drain
не сохраняет external synchronization. Частота allocations также не измерена.

## 2. Tiler: узкий перенос из GTA

[Pinned TheCruZ cf9a5d97](https://github.com/TheCruZ/KytyPS5-GTA/blob/cf9a5d972d2c3d4fa2b4a8de46b45f3719800572/src/graphics/host_gpu/shaders/gpu_tiler_common.inc)
копирует halfword через `uint16_t` storage arrays. Можно отделить это от его
fused-mip и1D dispatch переписывания. Наш `RequiredVulkan11Features` уже требует
`storageBuffer16BitAccess`; [feature contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDevice16BitStorageFeatures.html)
разрешает16-битные StorageBuffer members. Для простых load/store не нужна
добавленная16-битная арифметика.

Proposal меняет только2B branch. Сохраняет1B атомики,8×8 dispatch, Push ABI,
tile equations, ranges, padding, barriers и lifetime. Prepare уже требует
halfword alignment для внутренних offsets/pitches; Record сохраняет DWORD
ограничения верхнего target offset/capacity. Полуслово внутри target может
начинаться с+2; odd width не требует записи соседнего halfword.

Условно, полный1080p fallback выполняет4 147 200 shader RMW операций на upload.
Это арифметика по исходнику, не hardware counter или доказанное число uploads
Yōtei. Исторический hot key506bba имел2B представление;5067be имел4B и не
ускорится этим устранением atomics. Из-за повторного использования адресов
точный нынешний format/route неизвестен. Sparse upload observer уже подготовлен.

Перед портом нужны реальные byte/numeric/guard controls через TileManager::Detile,
включая+2 offset, odd width, pitch/tails и source preservation. Cost oracle
замораживает ACTUAL BPE=2 VkSpecializationInfo и удаляет мёртвые ветви. Считать
атомики целого модуля нельзя: оставленная1B ветка содержит их законно.
Предложения и optimizer API сохранены в `cycle50/gpu-upload-research/README.md`.
Патч unapplied/uncompiled/unrun. Никакие metadata guards не сняты.

## 3. DMA: безопасный scope и ограничения

[Донор f4899651](https://github.com/TheCruZ/KytyPS5-GTA/commit/f48996517d2263ddaf5742f0c15cbc30ff452fc5)
сообщает о меньших uploads/write faults в GTA. Это внешний результат.
Для нашей версии недопустимы его256 MiB clipping и64-record assumptions без
полного доказательства reachable addresses.

Первый subset — readonly invariant-root windows с конечными offsets.
Indirect buffer, BVH, address writes/atomics и неизвестные pointer chains
оставляют full fallback. Partial sync не подтверждает whole epoch и не
поглощает глобальные dirty hints. Ranged upload работает страницами4 KiB,
существующий HotWrites bitmap уже различает синхронизированные страницы.
Оба pre/post PrepareBda сохраняются из-за rediscovery/image fringe.

Следующий witness использует существующий `KYTY_DEBUG_BDA_STATS=1` в короткой
подтверждённой игровой фазе. OFF — отсутствие переменной, не строка0.
Если объём значителен, observer сравнивает ACTUAL upload callbacks с доказанным
read union. Packed RecordUpload total нельзя считать одним диапазоном.
Регрессия A/D должна сохранить D dirty после A, затем unknown D consumer
в той же epoch обязан получить новые данные. Это ловит неверное общее
epoch acknowledgement и потерю hint в одной region.

Подробная схема сохранена в
`cycle50/cpu-dma-footprint/f489-portability-and-cost-witness.md`.
Нативная реализация и регрессия этого кандидата pending. Тысячи дешёвых вызовов
PrepareBda сами по себе не доказывают возможность удвоить FPS.

## Порядок следующих действий

1. После прекращения чужой GPU нагрузки установить один phase-valid clean
   контроль на текущем EXE, той же игре/save/cache, neutral1080/full effects.
2. За отдельный10-секундный diagnostic собрать actual nonindexed DrawIndirect
   hits, upload refusal tuples и BDA volume; не смешивать это с clean FPS.
3. Выбрать один реально дорогой путь. Genuine RED → unchanged GREEN →
   соседние случаи → clean короткий title A/B послеframe400/min90 и реального мира.
4. Equal/lower FPS — откат; снимки после подтверждённого роста. Current ×2
   от исходных3,7 и60 FPS/fullFX ещё не достигнуты.

Сужение Function LDS arrays уже реализовано у нас; повторно переносить его
как новое ускорение не нужно. Helper-lane atomic loops из новых релизов Jetsku
имеют отдельный correctness интерес, но их текущий вклад в NVIDIA/Yōtei
не доказан и не заменяет три измеримых кандидата выше.
