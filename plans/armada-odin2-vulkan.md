# План Fable 2 для Armada OS на Odin 2

Дата аудиту: 2026-10-17. Цільовий пристрій: AYN Odin 2, 16 ГБ RAM.
Armada вже встановлена; власник може запускати тестові збірки й передавати логи.
Статус: реалізація триває; поточні результати запуску та обмеження —
у [звіті реалізації](armada-implementation-status.md). Аудит нижче фіксує
вихідний стан до реалізації.

## Ціль і критерії результату

Основний результат — нативний Linux ARM64 executable з Vulkan, запуском зі
Steam у Game Mode, вбудованим контролером, звуком та робочими збереженнями.
Windows x64 через FEX/Proton — допоміжний діагностичний варіант і можливий
проміжний спосіб пограти. Він не замінює перевірку нативного варіанта.

Розрізняємо три рівні готовності:

- Технічний запуск: справжній Vulkan GPU на Odin, меню та перша керована сцена.
- Робоча тестова версія: щонайменше година гри, переходи між локаціями,
  діалоги/бій, звук, три цикли збереження/повного виходу/завантаження без втрати
  прогресу, стабільна робота керування у Desktop і Game Mode.
- Версія для проходження: перевірене проходження основної кампанії на цій
  збірці або явно позначений незавершений статус такого тестування. Заяву
  автора про Windows-версію не переносимо на ARM64 без перевірки.

Продуктивність оцінюємо окремо від правильності. Ціль — рівні 30 FPS за
реальної внутрішньої роздільності 1280×720 і нормальної швидкості симуляції.
Це ціль оптимізації, не прогноз. Якщо вона недосяжна, фіксуємо виміряну
швидкість і конкретне обмеження; нижчий масштаб рендерингу перевіряємо окремо.

## Перевірений вихідний стан

| Компонент | Факт та наслідок |
| --- | --- |
| Репозиторій | `/home/rhonstin/Projects/Fable-2-Recomp`, гілка `armada/odin2-vulkan`, база `b615a5e913165cf2a5a285fd84278dcf3276d511` |
| SDK | Gitlink `thirdparty/rexglue-sdk` зафіксований на `3c02ca0bf994bfff771307e04209d8753342c97d`; checkout і вкладені залежності ще не ініціалізовані |
| Git | Локальна копія містить один shallow-коміт; `upstream` вказує на himdo. Remote fork відсутній |
| Доступ | Термінал не резолвить GitHub; поточна авторизація `gh` недійсна. GitHub-конектор читає код, але не створює fork |
| Робоча машина | Linux x86_64, Clang 22.1.8, CMake 4.3.0, Ninja і Podman доступні. Це не ARM64-тестовий пристрій |
| Базові перевірки | Каталог версій актуальний; `python3 -m unittest discover -s tests -p 'test_*.py'` — 7 тестів успішно. Повна компіляція не виконувалась |
| Підтримка ОС | Odin 2 SM8550 є в офіційному списку протестованих пристроїв Armada; це не свідчення сумісності Fable 2 |

Виявлені технічні точки роботи:

- `tools/vulkan_sdk/CMakeLists.txt`: примусове ввімкнення обох графічних
  backend, безумовні `-msse4.2` і Windows linker option `/DEBUG`.
- `src/vulkan_smoke.cpp`: Win32-вікно, `vulkan-1.dll`, Windows surface;
  його успішний запуск не можна вважати нативним Linux-тестом.
- `src/main.cpp` безумовно включає Windows-залежні
  `fable2_av_probe.h` і `fable2_stall_dump.h`. Перевірити весь ланцюг include,
  а не лише файли, які безпосередньо компілює CMake.
- `tools/stage_renderer_sdk.py`: staging орієнтований на Windows DLL/LIB
  і офіційний `rexglue.exe`; для Linux потрібен окремий шлях.
- `generated/rexglue.cmake`: вже підтримує SDK з вихідного дерева через
  `REXSDK_DIR`, але codegen є залежністю збірки host. Автогенеровані файли
  не редагувати вручну без перевірки механізму їх відтворення.
- У зафіксованому SDK є Linux ARM64 signal context, ARM64 MMIO та NEON/SIMDe.
  Наявність реалізації не доводить коректності Fable на конкретному ядрі.
- SDK `tests/ppc/CMakeLists.txt` містить безумовні `-msse4.1 -mssse3`;
  ARM64-набір тестів також потребує адаптації прапорців.
- Vulkan SDK вимагає `independentBlend`, `fragmentStoresAndAtomics`,
  `vertexPipelineStoresAndAtomics`, а за типовими Linux-налаштуваннями також
  `geometryShader` і `fillModeNonSolid`. Перевірити всі перевірки capabilities,
  розширення, формати, ліміти й вимоги renderer, не лише цей короткий список.

## Послідовність робіт

### P0. Зафіксувати середовище Odin — без файлів гри

Власник передає результати `uname -a`, `uname -m`, `cat /etc/os-release`,
`getconf PAGESIZE`, `vulkaninfo --summary` і повний `vulkaninfo` або JSON-звіт,
якщо інструмент доступний. Відсутність vulkaninfo не дорівнює відсутності Vulkan:
тоді використати тест capabilities з P3. Зафіксувати версію образу Armada,
GPU/driver, Vulkan loader, режим живлення, дисплея та доступний диск/RAM.

Результат: звіт про реальний пристрій і таблиця «вимога renderer → підтримка
драйвером». Не використовувати llvmpipe/lavapipe як доказ працездатності GPU.
SSH необов'язковий: достатньо архівів з логами та результатами тестів.

### P1. Відновити повне дерево залежностей — без файлів гри

Відновити звичайний мережевий доступ для Git або підготувати переносимий bundle
з точно зафіксованими залежностями. Ініціалізувати SDK на gitlink-коміті,
завантажити його submodules і перевірити SHA. Не оновлювати SDK до latest
одночасно з портом. Перевірити, які runtime-патчі вже включені у fork;
`tools/prepare_runtime_sdk.py` застосовувати лише після перевірки його
accepted states. Не накладати старий `rexglue-sdk-local.patch` поверх нових
виправлень навмання. Окремо перевірити libmspack і Vulkan/SPIR-V залежності.

Створити GitHub fork після відновлення авторизації, додати його як `origin`,
зберегти `upstream` для оригіналу і опублікувати робочу гілку. Fork бажаний для
резервування й обміну змінами; локальна розробка не має чекати саме на нього.

Результат: відтворювана карта версій game/SDK/dependencies/toolchain і повний
checkout. Вкладені зміни SDK зберігати як окремі патчі/коміти, а не втрачати
за оновленим gitlink. Історію shallow-копії розширювати за потреби.

### P2. Розділити host tools і ARM64 runtime — без файлів гри

Спочатку зібрати native x86_64 codegen на робочій машині з тієї самої
узгодженої версії SDK. Для ARM64 використовувати окремий build tree і
Fedora/Armada-сумісний sysroot та бібліотеки. Альтернатива першій збірці —
нативна компіляція у ARM64 development container/на пристрої з достатнім диском.
Не встановлювати development packages у базовий образ Armada без потреби.

Забезпечити, щоб cross-build запускав host `rexglue`, а не ARM64 executable.
Перевірити C++23 standard library, Clang, LLVM tools, linker, SDL3,
FFmpeg, glslang/SPIR-V, VMA та залежності SDK. Зафіксувати точні версії
контейнера/пакетів після першої успішної конфігурації. Наявний пресет
`linux-arm64-release` сам не створює cross toolchain і sysroot.

Результат: збираються `rexruntime` і `rexgpu-xenos` під Linux ARM64;
архітектура підтверджена `file`/`readelf`, залежності та GLIBC/GLIBCXX сумісні
з Odin. Жодного змішування x86_64 `.so` з ARM64 або різних ABI runtime/plugin.

### P3. Перевірити Vulkan до запуску гри — без файлів гри

Зробити Linux-сумісний SDL3/Vulkan smoke test з окремою CMake-точкою входу,
яка не залежить від codegen і `default.xex`. Прибрати Win32 loader/surface
для цього target. Тест має створити device/swapchain, відмалювати кадри,
пережити resize/recreate і коректно завершитись у Desktop і Game Mode.

Після загального тесту перевірити створення provider/plugin саме ReXGlue,
повний перелік required features, memory types, texture/depth formats і
SPIR-V pipeline. Логувати причину відмови. Тест кольорового вікна сам по собі
не доводить готовність емуляції Xenos.

Результат: обидва рівні тесту працюють на фізичному GPU Odin.
Якщо потрібної можливості немає, визначити конкретний fallback чи сумісну
версію драйвера й перевірити правильність кадру. Вимкнення перевірки capability
без реалізації її функції не вважати виправленням.

### P4. Перенести обв'язку Fable на Linux ARM64 — основа без ROM

Платформні/архітектурні compile і link options задавати умовно. Для Linux
використовувати Vulkan і вимкнути D3D12; Windows-поведінку зберегти.
Перевірити SDK helper targets, precompiled headers, version generation,
debug/release ABI, шляхи `.so`, plugin loading, rpath `$ORIGIN` і символи.

Windows-only діагностику ізолювати за platform guard або замінити Linux
логуванням/crash dumps. Не замінювати заглушками необхідну логіку гри,
пам'яті, синхронізації чи рендерингу. Перевірити include graph, файлові шляхи,
регістр назв, пошук assets, clocks/threads, SDL input та доступ до звуку.
WPF launcher не є залежністю нативного запуску: потрібен shell launcher.

Узгодити автогенерацію: доступність generated sources на етапі CMake,
повторна конфігурація після codegen, порядок `apply_recomp_patches.py` і
компіляції hotfunc overrides. Не припускати, що один configure/build покриває
чистий checkout. Додати source-only перевірки й ARM64 unit/PPC тести.

Результат: підтримуваний шлях конфігурації без Windows/x86-only залежностей;
повна компіляція саме гри завершується після P5, коли з'явиться XEX.

### P5. Перевірити файли гри та виконати codegen — потрібен XEX

Витягнути `default.xex` і повну `data/` з відповідної копії гри. Для CLI
у репозиторії є cross-platform .NET 8 `tools/x360extract`. Звірити SHA-256
XEX і content guards з `constants/game_versions.json`, не лише назву ISO.
Поточний каталог містить, зокрема, GOTY USA/EU, German і Russian та інші
профілі; конкретний dump приймаємо тільки після відповідних перевірок.
Не використовувати `--skipHash` для оголошення непідтримуваної ревізії сумісною.

Запустити узгоджений codegen на host, застосувати recomp-патчі та побудувати
Release і окрему збірку з символами. Порівняти manifests, hook anchors і
hotfunc overrides з обраною ревізією. Різний код XEX потребує власної валідації.

Результат: ARM64 executable, runtime і Vulkan-plugin збираються з чистого
дерева за описаною процедурою. XEX/data не включати у публічний пакет.

### P6. Перевірити runtime на ARM64

Виконати тести PPC/SIMDe, byte order, FP-конверсій, atomics, guest memory
reservation/commit/protection, MMIO та обробки fault на фактичному page size
Armada. Адаптувати x86-only прапорці наявних PPC tests. Для cross-build
тестів також відокремити host генератори від ARM64 test executables.

Запустити гру послідовно до XEX load, ініціалізації kernel/runtime,
ініціалізації GPU/audio, меню та першої сцени. Зберігати логи кожного етапу.
При fault перевіряти host і guest PC, стек, guest address та причину сигналу.
QEMU/компіляторні перевірки не замінюють MMIO і GPU тести на Odin.

Результат: керована перша сцена без зациклення exception, corrupted guest
state, відсутнього звуку або нульової швидкості симуляції.

### P7. Довести Vulkan-рендеринг Fable

Перевірити shaders/SPIR-V, render targets/resolve, depth/stencil, textures,
hero/dog morphing, UI/text, відео, освітлення, прозорість та синхронізацію.
Порівняти однакову сцену/save з відомою робочою збіркою. За потреби Windows
або Linux x86_64 Vulkan використовувати для відокремлення загальної помилки
backend від ARM64 чи Adreno. Контрольний Windows D3D12 результат корисний,
але не доводить правильність Vulkan.

Для кожної помилки зберігати dump/log, точні параметри, сцену і кроки
відтворення; validation layers увімкнути на діагностичному запуску, не для
фінальних FPS-вимірювань. Shader/pipeline cache перевірити на холодному і
повторному запуску та версіонувати при зміні renderer/driver.

Результат: немає графічних дефектів, які блокують гру; залишкові дефекти
перераховані з відтворенням. Безумовне вимкнення ефектів не приховує помилку.

### P8. Перевірити керування, звук і збереження

Перевірити обидва стики, dead zones, analog triggers, D-pad, меню, паузу,
комбінації Steam і повернення з overlay. Виявити дублювання input між SDL,
Steam Input та пристроєм. Перевірити звук/відео sync, audio pacing та
поведінку при зміні аудіовиходу. Перевірити кирилицю/локалізацію обраного dump.

Зберігати saves/config/cache у записуваних user directories або чітко
описаному переносимому каталозі, окремо від data і бінарників. Перевірити
три цикли save → вихід → запуск → load, перезапуск ОС і оновлення пакета.
Відтестувати сон/пробудження та повторне створення swapchain/audio/input;
невиправлену проблему явно позначити, не обіцяти підтримку suspend.

Результат: година гри й матриця сценаріїв без втрати прогресу або блокування
керування. Desktop і Steam Game Mode перевірені окремо.

### P9. Оптимізувати за вимірами

Виміряти CPU/GPU frame times, guest simulation FPS/швидкість, median/p95/p99,
RAM, cache growth і thermal throttling. Рахувати реальні кадри та швидкість
гри: лічильник presentation FPS не доводить нормальної симуляції.
Використовувати однакові saves, маршрути, версії драйвера і power profile.

Оптимізувати спершу виміряне обмеження: hotfunc/SIMDe, synchronization,
shader compile/cache, GPU resolves, render scale. Перевіряти кожне прискорення
на збереження правильної логіки/кадру. Спочатку ціль 720p/30; 60 FPS — окреме
подальше дослідження. Зміна розміру вікна не доводить зміну внутрішнього render
scale. Повторити довгий тест після прогріву, а не лише короткий benchmark.

Результат: звіт з реальними метриками та рекомендованими налаштуваннями.
Підтримуваний рівень продуктивності заявляти тільки на підставі цих вимірів.

### P10. Зібрати переносимий пакет та перевірити установку

Підготувати ARM64 archive з executable, узгодженими runtime/plugin/dependencies,
shell launcher, типовим config, ліцензіями та build manifest/checksums.
Не пакувати власний GPU driver/loader навмання: використовувати перевірену
системну Vulkan-інтеграцію Armada. Перевірити binary dependencies і rpath.

Launcher приймає шлях до game data, працює зі spaces у шляхах, пише зрозумілі
логи і показує помилку неправильної версії/відсутнього plugin. Нативний запуск
у Steam не повинен випадково вмикати Proton/FEX. Зробити зручний спосіб
додати гру до Steam, передати аргументи й знайти saves та troubleshooting logs.

Результат: нова інсталяція пакета на Odin з окремої папки працює без build
tree і development tools; оновлення не псує saves/config.

### P11. Підтвердити готовність та опублікувати результат

Повторити acceptance тести на точно тому пакеті, який буде переданий.
Зафіксувати SHA, ОС/driver, hash XEX, config, пройдені сценарії, лог і FPS.
Вести regression matrix Windows D3D12/Vulkan, Linux x86_64 Vulkan та Linux
ARM64 Vulkan; перші два — контрольні середовища за їх доступності.
Додати відтворювану збірку/CI без game content і source-only tests.
Hardware/gameplay tests лишаються локальними або на окремому runner.

Опублікувати код/патчі у fork та пакет без game content після перевірки.
Заявити рівень готовності за критеріями на початку цього плану. Для статусу
«кампанія пройдена на Odin» потрібен окремий фактичний walkthrough.

## Залежності, відповідальні та точки рішень

| Послідовність | Виконавець | Умова переходу |
| --- | --- | --- |
| P0 та P1 | Власник: логи Odin; агент: аудит і залежності | Відомі реальні capabilities, доступні pinned sources |
| P2 → P3 | Агент: збірки; власник: запуск на Odin | ARM64 SDK і Vulkan device/plugin пройшли перевірку |
| P4 | Агент | Усунені platform/architecture blockers; може йти до появи dump |
| P5 | Власник: game files; агент: валідація/codegen/build | Підтримуваний XEX і повний ARM64 build |
| P6 → P7 → P8 | Агент: виправлення; власник: hardware-тести | Перша сцена, правильний кадр, година гри/save-load |
| P9 → P10 → P11 | Агент і власник | Метрики, чиста установка, підтверджений пакет |

На першій точці рішень (P0/P3) з'ясувати, чи потрібні зміни драйвера/renderer.
На другій (P5/P6) — чи достатня поточна ARM64 реалізація runtime для Fable.
Саме після цих двох перевірок оцінювати строк порту; календарний прогноз до
них був би припущенням. Невдача native-тесту означає діагностику й виправлення
або конкретний опис нездоланного обмеження, не автоматичне оголошення
Proton-варіанта повноцінним нативним портом.

Найближчі дії: зібрати звіт P0; відновити залежності P1; підготувати окрему
збірку SDK і Linux Vulkan capability/smoke test P2–P3. Файлів гри для цих
кроків не потрібно. Планування завершене; перелічені роботи з реалізації
та перевірки залишаються невиконаними, доки немає відповідних артефактів.

## Джерела аудиту

- Локальні `CMakeLists.txt`, `CMakePresets.json`, `.gitmodules`,
  `generated/rexglue.cmake`, `src/main.cpp`, `src/core/fable_2_app.h`,
  `tools/vulkan_sdk/CMakeLists.txt`, `tools/stage_renderer_sdk.py`,
  `tools/prepare_runtime_sdk.py`, `constants/game_versions.json`, `tests/README.md`.
- [SDK за зафіксованим SHA](https://github.com/himdo/rexglue-sdk/tree/3c02ca0bf994bfff771307e04209d8753342c97d):
  CMake, core memory/exception handlers, MMIO, PPC context, Vulkan device/provider,
  thirdparty та tests/ppc CMake. Це source audit, не виконання SDK.
- [Armada: підтримувані пристрої](https://armadaos.dev/devices/supported-devices/).
- [Armada: сумісність](https://armadaos.dev/compatibility/).
- [Armada: відомі проблеми](https://armadaos.dev/troubleshooting/known-issues/).
