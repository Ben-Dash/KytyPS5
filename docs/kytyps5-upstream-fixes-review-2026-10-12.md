# Исправления main и форков KytyPS5: исследование 12 октября 2026

Проверена текущая ветка `yotei-integration-native` на `72e3568c9b64fe736f67ecf403e0ac07cc97756a`.
Новая просьба пользователя разрешает исследование. Предыдущий лимит трёх игровых
попыток исчерпан: код эмулятора, настройки, сборки, тесты и игра в этом исследовании
не изменялись и не запускались. Сохранение этого отчёта не разрешает четвёртую попытку.

**Полезные отсутствующие изменения найдены. Подтверждённого готового исправления
оставшихся 192 нулевых строк матриц и стабильной картинки не найдено.**

## Что проверено

- Официальный [main на dad900c61d6c7ff8aefc9e666ca7c3de4bbf4176](https://github.com/KytyPS5/KytyPS5/commit/dad900c61d6c7ff8aefc9e666ca7c3de4bbf4176),
  опубликованный 12 октября в 05:51 по Минску. GitHub API и git ls-remote согласуются.
  Просмотрены последние 60 коммитов, включая 19 после прежнего среза `730e1a3c`,
  и связанные более старые изменения.
- Получены метаданные 847 публичных прямых форков. API сообщает forks_count=927;
  полное покрытие сети не предполагается. Ветки проверены у восьми выбранных форков,
  код и история — у семи; одинаковые main Jetsku и JonJon1992 не считались двумя фиксами.
- TheCruZ/KytyPS5-GTA, Jetsku/KytyPS5, JonJon1992/KytyPS5-fork, Almo7aya/KytyPS5,
  modhollow/KytyPS5, QualityControled/KytyPS5, ClGratton/KytyPS5-gratton,
  Franseven/KytyPS5. Проверялись конкретные ветки и коммиты, а не описания проектов.
- `peepostarexx/KytyPS5` в текущем API отвечает 404. Исторический локальный
  `research-peepo/YOTEI-BRANCH` сохранён на `6d04103525b24559a496612ef8959a7666ce9a20`.
  Новые изменения этой недоступной ветки не подтверждены.
- Сравнение проводилось по содержимому текущего кода. Уже перенесённые R8 SNORM,
  packed-prefix, BC-R64 и NGG-count повторно не предлагались.

## Приоритетные кандидаты

| Приоритет | Изменение | Чего не хватает в нашей ветке | Проверка перед возможным переносом |
| --- | --- | --- | --- |
| 1: текстуры | [main dad900c6: BC alias writes](https://github.com/KytyPS5/KytyPS5/commit/dad900c61d6c7ff8aefc9e666ca7c3de4bbf4176) | При смене raw integer и BC-представлений разных mip/layout GPU-байты публикуются перед удалением старого backing. CopyImageMip также обновляет изменённый через буфер destination. У нас нет BcAlias и этой проверки destination. | Независимый GPU producer → BC4/BC5/BC7 consumer, три слоя/восемь mip, packed tail, сохранение padding и нетронутого слоя; частичное копирование и CPU-dirty/неподдерживаемый download. |
| 1: выбор ресурсов | [TheCruZ d5259de5: scalar-branch V# bases](https://github.com/TheCruZ/KytyPS5-GTA/commit/d5259de53c911b390177e4f1056d0d19a4d90418) | LowerDescriptorPhi у нас запрещён при любых shader writes; вложенные выражения над Phi не обрабатываются через LowerDescriptorWord. Форк разрешает условия из user-data, сохраняет отказ для условий, читающих память. | Synthetic shader выбирает два различных valid buffer через обе ветки, nested Phi/OR/carry; независимый readback, неизменность соседей и отказ для memory-derived/write-alias условий. |
| 2: первая доступность данных | [TheCruZ 19cc54d0: DMA base preparation](https://github.com/TheCruZ/KytyPS5-GTA/commit/19cc54d05f497c80f960c2773565f84837664d03) | Форк трассирует фактические DMA address operands к соседним user-data SGPR и готовит ограниченное 16 KiB окно базы. Это не эквивалент нашему сбору успешных SRT memory reads: DMA-only базы могут не участвовать в CPU materialization. | Первая FLAT загрузка через известную базу без SRT-чтения, реальные mapped bounds и границы окна, GPU-written сосед, once-only counter; установить происхождение реального адреса DA4. |
| 3: владение памятью | [Franseven 1a65d263: TakeGpuPagesForCpu](https://github.com/Franseven/KytyPS5/commit/1a65d2638a78c4ad950bb640964156c1b9c598bc) | Переход GPU → CPU выполняется под одним region lock вместо двух операций. Такой helper отсутствует; upstream-тест в патче отсутствует. | Детерминированное воспроизведение межпоточного interleaving, dirty-state инвариант и сохранность реально записанных GPU-данных. |

Это кандидаты на отдельные общие семантические исправления, а не доказанная причина
именно нашего кадра. BC alias относится к текстурам и владению байтами; выбор V# и DMA
базы ближе к compute-ресурсам. Подменять условия, ограничивать шейдер по хешу,
санитизировать нулевые матрицы или повторять dispatch с побочными эффектами нельзя.

## Дополнительные изменения main

- [488c0aca: writable descriptor Phi с immutable условиями](https://github.com/KytyPS5/KytyPS5/commit/488c0acae9a2300b4a53acd8d611d23a80c33486)
  предлагает другую реализацию близкой возможности. У нас нет классификации
  RuntimeValueType::ImmutableInteger. Нужно выбрать одну совместимую реализацию
  вместе с тестами, а не накладывать main и TheCruZ вслепую.
- [df0412af: shared predicates при extraction](https://github.com/KytyPS5/KytyPS5/commit/df0412afb510213be45021ce4d3e73fd91085643)
  сохраняет общие Boolean-подграфы, включая невычисляемые ветви, и предотвращает
  разрастание DAG. Наш generic Clone уже memoized, а ResourceControlFlow консервативно
  отказывается от shader writes. Патч требует другой модели частичных условий;
  прямой перенос нескольких строк не исправит GPU EXEC.
- [4c87dbeb: indirect ImageGatherRaw](https://github.com/KytyPS5/KytyPS5/commit/4c87dbebb2cd58d402b1eda55c051b4e5fea3aba)
  выбирает конечные candidate descriptors. В текущем gather такого выбора нет.
- [984cfceb: texel offsets с учётом EXEC](https://github.com/KytyPS5/KytyPS5/commit/984cfcebd141b211aa9e0a8cde63b0ebe652d07a)
  раскрывает константу offset в активном контексте. Наш dynamic coordinate fallback
  нужно сохранить; проверять masked Select/Phi, signed offsets, texels/LOD и sparse EXEC.
- [f28c0fcc: DCC metadata comparison depth aliases](https://github.com/KytyPS5/KytyPS5/commit/f28c0fccb22fd6b323a0d9dc7265dc3f6da092c1)
  сохраняет guest metadata при изменении host-view на depth. Возможный texture/depth
  кандидат, без установленной зависимости матриц DA4.
- [f848d276: native SRT walker](https://github.com/KytyPS5/KytyPS5/commit/f848d2761bd2184534b9cbeaac63f1489768714d)
  является CPU/x86 fast path. Он применяется к ограниченным raw-read планам без callback;
  у нас уже compiled evaluator, memo, callbacks и residency observer. Это исследование
  CPU стоимости, не новая реализация GPU BDA и не доказанный выигрыш FPS.

## Что не следует считать новым решением

- Часть dad900c6 переносит GC с command lists на SuspendPoint. **Наш возраст ресурсов
  уже считается по TickFrame** в buffer/texture cache, несмотря на частые вызовы GC.
  Ускоренного старения на каждый список здесь уже нет. Сокращение частоты обхода может
  иметь другой эффект, но требует отдельной проверки pressure/retirement; нельзя
  объявить этот кусок исправлением ещё существующей ошибки старения.
- Полное wave64 ветвление [a8beac19](https://github.com/KytyPS5/KytyPS5/commit/a8beac190f13903ea7cc7a1a5e0c335fad0127aa)
  уже доступно через KYTY_WAVE_BRANCH_FULL. Оно использовалось в третьей попытке,
  которая не дала захвата DA4. В свежем main EmitConditionRef совпадает с прежним
  срезом: нового подтверждённого wave/EXEC/LDS/loop исправления не найдено.
- [Jetsku BDA-store log](https://github.com/Jetsku/KytyPS5/commit/066384e4de2c0dbf87d3c3aeb1fc0c64b7803992)
  полезен как образец диагностики, но ограничен 4096 записями и не закрывает atomics,
  overflow и конфликтующее GPU/image ownership. Это не готовая общая memory-семантика.
- [Jetsku wave32/native64 LDS](https://github.com/Jetsku/KytyPS5/commit/d47cc5f9725cd1842dae513003a7181cfbd5b01c)
  имеет направление эмуляции, обратное нашему guest64/native32. Его LDS scope следует
  сравнивать отдельно по контракту, не переносить из-за сходного слова wave.
- Almo7aya model-workaround расширяет нулевой NUM_RECORDS до фиксированного 64 KiB.
  Похожий визуальный симптом не доказывает допустимость изменения descriptor bounds.
  Broad renderer/EOP cleanup main также не заменяет наши проверенные ordered writes.

## Связь с текущим блокером

В точном ранее сохранённом DA4-коде 7 889 инструкций и 351 conditional scalar branch;
242 из них EXECZ. IMAGE-инструкций нет. Поэтому gather, sample offsets и missing ISA
features BCNT0/FF0/signed64 GT/GE не являются прямым исправлением этого producer.
Это статическое сравнение кода, не доказательство его executed masks.

После наших фиксов первый проблемный producer заполняет 943 из 1135 строк.
Оставшиеся 192 — первые 64 строки каждого блока из 512. В reviewed main/forks не
найдено независимого подтверждения, что новый патч закрывает именно этот случай.
Для дальнейшего решения нужны фактические входы/EXEC/LDS/store witnesses первой
волны; текущий простой strided-loop тест уже проходит.

**Рекомендация:** сохранить текущую интеграционную ветку и накопленные доказательства.
При новой отдельной инструкции на реализацию рассматривать BC ownership и V#/DMA
кандидаты отдельными изменениями с intended RED → unchanged GREEN, затем соседями.
Не менять базу целиком и не обещать исправление картинки по названию коммита.
Исследование не разрешает новые игровые попытки после лимита 3/3.

## Сохранённые доказательства и проверки

`_Build/analysis/upstream-forks-research-20261012/main-agent/main-review.md` содержит
полные pins, API commits и сравнение main. `forks-agent/report.md` содержит список
проверенных веток, bare-repository истории, публичные fork metadata и выбранные patches.
Артефакты лежат вне tracked source; shared handoffs сохраняют результат между сессиями.

Выполнены только Git/API/raw-source чтения, сравнение файлов и git diff --check.
Native tests/builds/GPU/game не выполнялись. Source compatibility, RED/GREEN,
cross-game execution, стабильная картинка и FPS по этим кандидатам пока не проверены.
