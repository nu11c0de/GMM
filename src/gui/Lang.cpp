#include "Lang.h"

#include <QAbstractButton>
#include <QAction>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QTabWidget>
#include <QWidget>

namespace gtamm::lang {

namespace {

Language g_lang = Language::Russian;

// Russian source -> English. Keys must match the literals used in the UI exactly
// (including leading/trailing spaces, punctuation and '&' mnemonics).
const QHash<QString, QString>& dict()
{
  static const QHash<QString, QString> d = {
      // --- menus ---
      {"&Инстанс", "&Instance"},
      {"&Сменить / управление инстансами…", "&Switch / manage instances…"},
      {"Сменить папку &игры…", "Change &game folder…"},
      {"Открыть папку &игры", "Open &game folder"},
      {"Открыть папку игры", "Open game folder"},
      {"Папка игры не найдена.", "Game folder not found."},
      {"&Открыть папку инстанса", "&Open instance folder"},
      {"&Файл", "&File"},
      {"Импорт &папки…", "Import &folder…"},
      {"Импорт &архива…", "Import &archive…"},
      {"Импорт &сборки (из папки игры)…", "Import &build (from a game folder)…"},
      {"&Выход", "&Quit"},
      {"&Профиль", "&Profile"},
      {"&Новый профиль…", "&New profile…"},
      {"&Переименовать профиль…", "&Rename profile…"},
      {"&Копировать профиль…", "&Copy profile…"},
      {"&Удалить профиль", "&Delete profile"},
      {"&Менеджер профилей…", "&Profile manager…"},
      {"Менеджер профилей", "Profile manager"},
      {"Переименовать…", "Rename…"},
      {"Экспорт в zip…", "Export to zip…"},
      {"Импорт из zip…", "Import from zip…"},
      {"Добавить &разделитель…", "Add &separator…"},
      {"Свои &сохранения для профилей", "Per-profile &saves"},
      {"Каждый профиль получает свою папку GTA San Andreas User Files (сейвы + "
       "настройки), подменяемую на время игры",
       "Each profile gets its own GTA San Andreas User Files folder (saves + "
       "settings), swapped in for the game session"},
      {"Открыть папку сохранений профиля", "Open profile saves folder"},
      {"Экспорт мода в zip…", "Export mod to zip…"},
      {"Экспорт мода в zip", "Export mod to zip"},
      {"Экспорт мода", "Export mod"},
      {"Моды GMM (*.zip)", "GMM mods (*.zip)"},
      {"Мод экспортирован:", "Mod exported:"},
      {"&Экспорт сборки в zip…", "&Export build to zip…"},
      {"Импорт сборки из &zip…", "Import build from &zip…"},
      {"Экспорт сборки", "Export build"},
      {"Экспорт сборки в zip", "Export build to zip"},
      {"Сборки GMM (*.zip)", "GMM builds (*.zip)"},
      {"Что включить в архив сборки (профиль и моды включены всегда):",
       "What to include in the build archive (profile and mods are always "
       "included):"},
      {"Сохранения профиля (*.b)", "Profile savegames (*.b)"},
      {"Настройки игры (gta_sa.set)", "Game settings (gta_sa.set)"},
      {"Упаковка сборки…", "Packing build…"},
      {"Сборка экспортирована:", "Build exported:"},
      {"Импорт сборки из zip", "Import build from zip"},
      {"Распаковка сборки…", "Unpacking build…"},
      {"Профиль для импорта (новый или существующий — будет перезаписан):",
       "Target profile (new, or existing — it will be overwritten):"},
      {"Профиль «%1» уже существует. Перезаписать его моды и порядок?",
       "Profile \"%1\" already exists. Overwrite its mods and order?"},
      {"Импортирована сборка в профиль «%1»: %2 модов добавлено, "
       "%3 переиспользовано.",
       "Imported build into profile \"%1\": %2 mod(s) added, %3 reused."},
      {"Настройки &SA-MP (мультиплеер)…", "&SA-MP (multiplayer) settings…"},
      {"Настройки SA-MP", "SA-MP settings"},
      {"Настройки SA-MP (мультиплеер)", "SA-MP (multiplayer) settings"},
      {"Запускать игру через SA-MP (samp.exe)",
       "Launch the game through SA-MP (samp.exe)"},
      {"samp.exe внедряет samp.dll в gta_sa.exe и завершается; менеджер ждёт "
       "именно игру, поэтому моды не выдёргиваются до выхода из игры.",
       "samp.exe injects samp.dll into gta_sa.exe and exits; the manager waits on "
       "the game itself, so mods are not rolled back until you quit the game."},
      {"Сервер:", "Server:"},
      {"IP или хост (пусто — главное меню)", "IP or host (empty — main menu)"},
      {"Обзор серверов…", "Browse servers…"},
      {"Обзор серверов SA-MP", "SA-MP server browser"},
      {"Фильтр по названию или режиму игры…", "Filter by name or gamemode…"},
      {"Игроки", "Players"},
      {"Режим", "Mode"},
      {"Загрузка списка серверов…", "Loading server list…"},
      {"Подключиться", "Connect"},
      {"Да", "Yes"},
      {"Нет", "No"},
      {"Не удалось загрузить список серверов: ",
       "Failed to load the server list: "},
      {"Сервер отдал неожиданный ответ (не список серверов).",
       "The server returned an unexpected response (not a server list)."},
      {"Найдено серверов: %1", "Servers found: %1"},
      {"Все серверы", "All servers"},
      {"Избранное", "Favorites"},
      {"Пароль", "Password"},
      {"Нет избранных серверов — нажмите ★ у сервера в списке.",
       "No favorite servers yet — click the ★ next to a server in the list."},
      {"Избранных серверов: %1", "Favorite servers: %1"},
      {"Нажмите, чтобы добавить/убрать из избранного",
       "Click to add/remove from favorites"},
      {"Порт:", "Port:"},
      {"Пароль:", "Password:"},
      {"Пароль сервера (если нужен)", "Server password (if required)"},
      {"Ник:", "Nick:"},
      {"Имя игрока (никнейм)", "Player name (nickname)"},
      {"Launcher SA-MP:", "SA-MP launcher:"},
      {"Путь к samp.exe (относительно папки игры или абсолютный)",
       "Path to samp.exe (relative to the game folder or absolute)"},
      {"Обзор…", "Browse…"},
      {"Выберите samp.exe", "Select samp.exe"},
      {"samp.exe;;Все файлы (*.exe)", "samp.exe;;All files (*.exe)"},
      {"⚠ Файл не найден и ни один включённый мод его не разворачивает. Если "
       "samp.exe входит в сборку — проверь, что нужный мод включён; если нет "
       "— установи клиент SA-MP в папку игры (обычно кладёт туда samp.exe сам "
       "установщик).",
       "⚠ File not found, and no enabled mod deploys it either. If samp.exe is "
       "part of the build, make sure the mod that carries it is enabled; "
       "otherwise install the SA-MP client into the game folder (its own "
       "installer usually puts samp.exe there)."},
      {"Сохранение настроек SA-MP", "Saving SA-MP settings"},
      {"SA-MP включён для профиля", "SA-MP enabled for the profile"},
      {"SA-MP выключен", "SA-MP disabled"},
      {"Держать папку игры &чистой", "Keep game folder &clean"},
      {"Файлы, создаваемые модами/игрой в папке игры, после выхода убираются в "
       "стор профиля и возвращаются перед следующим запуском — папка игры "
       "остаётся в исходном состоянии",
       "Files a mod or the game creates in the game folder are moved to the "
       "profile store on exit and restored before the next launch — the game "
       "folder stays in its original state"},
      {"Открыть папку созданных файлов профиля", "Open profile generated-files folder"},
      {"Авто-установка карт (экспериментально)",
       "Map auto-install (experimental)"},
      {"Вставлять новые модели/текстуры карт в gta3.img и регистрировать .ide/.ipl "
       "в gta.dat. По умолчанию ВЫКЛ: может ронять новую игру при конфликте ID "
       "объектов мода. Без неё файлы карт просто кладутся как есть.",
       "Inject new map models/textures into gta3.img and register .ide/.ipl in "
       "gta.dat. OFF by default: it can crash a new game when a mod's object IDs "
       "clash with vanilla. Without it, map files just deploy as-is."},
      {"Авто-установка карт", "Map auto-install"},
      {"Авто-установка карт включена (экспериментально). Переразверните "
       "профиль. Если новая игра не запускается — выключите обратно.",
       "Map auto-install enabled (experimental). Redeploy the profile. If a new "
       "game won't start, turn it back off."},
      {"Авто-установка карт выключена. Переразверните профиль.",
       "Map auto-install disabled. Redeploy the profile."},
      {"Чистая папка игры", "Clean game folder"},
      {"Папка игры будет очищаться после игры; созданные файлы — в стор профиля",
       "The game folder will be cleaned after play; created files go to the "
       "profile store"},
      {"Очистка папки игры выключена", "Game-folder cleanup disabled"},
      {"Созданные файлы профиля", "Profile generated files"},
      {"&Сборка", "&Build"},
      {"&Развернуть", "&Deploy"},
      {"&Откатить", "&Roll back"},
      {"&Запустить игру", "&Launch game"},
      {"&Вид", "&View"},
      {"Тема", "Theme"},
      {"Тёмная", "Dark"},
      {"Светлая", "Light"},
      {"Язык", "Language"},
      {"Открыть папку тем", "Open themes folder"},
      {"Обновить список тем", "Refresh theme list"},
      {"&Справка", "&Help"},
      {"&О программе", "&About"},
      // --- toolbar ---
      {" Профиль: ", " Profile: "},
      {"Новый", "New"},
      {"Переименовать", "Rename"},
      {"Копировать", "Copy"},
      {"Удалить", "Delete"},
      {"Импорт папки", "Import folder"},
      {"Импорт архива", "Import archive"},
      {"Выше", "Up"},
      {"Ниже", "Down"},
      {"Повысить приоритет", "Raise priority"},
      {"Повысить приоритет (выше в списке)", "Raise priority (move up)"},
      {"Понизить приоритет", "Lower priority"},
      {"Развернуть", "Deploy"},
      {"Откатить", "Roll back"},
      {" Запуск: ", " Run: "},
      {"Играть", "Play"},
      {"Обновить", "Refresh"},
      // --- run panel ---
      {"Запуск", "Run"},
      {"Ярлык", "Shortcut"},
      {"Развернуть → запустить игру → откат после выхода (Ctrl+R)",
       "Deploy → launch the game → roll back on exit (Ctrl+R)"},
      {"Создать ярлык на рабочем столе, который сразу запускает игру этого "
       "инстанса (текущий активный профиль), без открытия менеджера",
       "Create a desktop shortcut that launches this instance's game "
       "directly (current active profile), without opening the manager"},
      {"GMM — быстрый запуск игры (", "GMM — quick launch ("},
      {"Не удалось определить папку рабочего стола.",
       "Could not determine the desktop folder."},
      {"GMM уже запущен", "GMM is already running"},
      {"GMM уже запущен для этого инстанса. Закройте другое окно перед тем "
       "как открыть его снова.",
       "GMM is already running for this instance. Close the other window "
       "before opening it again."},
      {"Ярлык создан на рабочем столе: ", "Shortcut created on the desktop: "},
      {"Не удалось создать ярлык.", "Could not create the shortcut."},
      {"Создание ярлыков доступно только в Windows.",
       "Creating shortcuts is only available on Windows."},
      // --- central ---
      {"Фильтр модов…", "Filter mods…"},
      {"Мод", "Mod"},
      {"Файлы", "Files"},
      {"Инфо", "Info"},
      {"Конфликты", "Conflicts"},
      // --- sort controls ---
      {"Сортировка:", "Sort:"},
      {"Порядок загрузки", "Load order"},
      {"Название", "Name"},
      {"Кол-во файлов", "File count"},
      {"Поле сортировки списка модов", "Field to sort the mod list by"},
      {"Направление сортировки (по возр./убыв.)",
       "Sort direction (ascending/descending)"},
      {"Показать панель запуска", "Show the run panel"},
      // --- log viewer ---
      {"Логи", "Logs"},
      {"Логи ▴", "Logs ▴"},
      {"Логи ▾", "Logs ▾"},
      {"Показать/скрыть панель логов", "Show/hide the log panel"},
      {"Файл лога:", "Log file:"},
      {"Авто", "Auto"},
      {"Автоматически обновлять лог в реальном времени",
       "Refresh the log automatically in real time"},
      {"Открыть в редакторе", "Open in editor"},
      {"В сборке не найдено файлов .log.\nЗапустите игру — моды (CLEO/ASI/"
       "modloader) создадут логи в папке игры.",
       "No .log files found in the build.\nRun the game — mods (CLEO/ASI/"
       "modloader) will create logs in the game folder."},
      {"Не удалось открыть файл лога: ", "Could not open the log file: "},
      {"… (показаны последние 2 МБ из ", "… (showing the last 2 MB of "},
      {" КБ) …\n\n", " KB) …\n\n"},
      // --- mod description ---
      {"Редактировать описание…", "Edit description…"},
      {"Описание мода", "Mod description"},
      {"Что делает этот мод, зачем он тут — просто заметка для себя.",
       "What this mod does, why it's here — just a note to yourself."},
      // --- build info (Markdown notes) ---
      {"Редактировать сборку", "Edit build notes"},
      {"Вставить изображение…", "Insert image…"},
      {"Размер изображения под курсором (% от исходного файла)",
       "Size of the image under the cursor (% of the source file)"},
      {"Сохранить", "Save"},
      {"Описание сборки в формате Markdown.\n\n"
       "# Заголовок\n**Жирный**, *курсив*, [ссылка](https://...)\n\n"
       "Изображение: ![подпись](images/screenshot.png)  — кнопка «Вставить "
       "изображение…» скопирует файл и вставит ссылку.",
       "Build notes in Markdown.\n\n"
       "# Heading\n**bold**, *italic*, [link](https://...)\n\n"
       "Image: ![caption](images/screenshot.png)  — the \"Insert image…\" button "
       "copies the file and inserts the reference."},
      {"Инфо о сборке", "Build info"},
      {"_Профиль не выбран._", "_No profile selected._"},
      {"# ", "# "},
      {"_Нет описания сборки. Нажмите «Редактировать сборку», чтобы добавить текст "
       "в формате Markdown и изображения._",
       "_No build notes yet. Click \"Edit build notes\" to add Markdown text and "
       "images._"},
      {"Выберите изображение", "Select an image"},
      {"Изображения (*.png *.jpg *.jpeg *.gif *.bmp *.webp);;Все файлы (*)",
       "Images (*.png *.jpg *.jpeg *.gif *.bmp *.webp);;All files (*)"},
      {"Вставка изображения", "Insert image"},
      {"Описание сборки сохранено", "Build notes saved"},
      // --- play lock / unlock ---
      {"Игра запущена", "Game running"},
      {"Дождитесь выхода из игры — лаунчер заблокирован.",
       "Wait for the game to exit — the launcher is locked."},
      {"Дождитесь выхода из игры — действие недоступно",
       "Wait for the game to exit — action unavailable"},
      {"▶  Игра запущена\n\nЛаунчер заблокирован до выхода из игры",
       "▶  Game running\n\nThe launcher is locked until the game exits"},
      {"▶  Игра запущена", "▶  Game running"},
      {"Лаунчер заблокирован до выхода из игры.\nОкно можно перемещать, "
       "а логи — смотреть внизу.",
       "The launcher is locked until the game exits.\nYou can move the window and "
       "watch the logs below."},
      {"Разблокировать", "Unlock"},
      {"Пользоваться менеджером, пока игра запущена (изменения станут доступны "
       "после выхода из игры)",
       "Use the manager while the game runs (changes become available after the "
       "game exits)"},
      {"Игра запущена в фоне — изменения станут доступны после выхода из игры",
       "Game running in background — changes become available after it exits"},
      {"Игра запущена — лаунчер заблокирован…", "Game running — the launcher is locked…"},
      {"Убить игру", "Kill game"},
      {"Принудительно завершить процесс игры (несохранённый прогресс "
       "будет потерян)",
       "Forcibly terminate the game process (unsaved progress will be lost)"},
      {"Принудительно завершить процесс игры?",
       "Forcibly terminate the game process?"},
      {"Несохранённый прогресс будет потерян. Лаунчер сам развернёт откат "
       "после завершения процесса, как при обычном выходе из игры.",
       "Unsaved progress will be lost. The launcher will still roll back "
       "normally once the process ends, same as a regular exit."},
      {"Не удалось завершить процесс игры (возможно, он уже закрылся)",
       "Could not terminate the game process (it may have already exited)"},
      // --- status bar ---
      {"  Игра: ", "  Game: "},
      {"<не задана>", "<not set>"},
      {"Модов: ", "Mods: "},
      {"  ○ не развёрнуто  ", "  ○ not deployed  "},
      {"  ● РАЗВЁРНУТО: ", "  ● DEPLOYED: "},
      {"  ● развёрнуто: ", "  ● deployed: "},
      {" (выбрано: ", " (selected: "},
      {") — нажмите «Развернуть»  ", ") — click “Deploy”  "},
      {"нет игры", "no game"},
      // --- conflict marks ---
      {"Перекрывает и перекрыт", "Overrides and overridden"},
      {"Перекрывает другие моды", "Overrides other mods"},
      {"Перекрыт модом выше", "Overridden by a mod above"},
      // --- details panel ---
      {"Источник", "Source"},
      {"Импортирован", "Imported"},
      {"Хеш", "Hash"},
      {"Файлов", "Files"},
      {"</table><hr><b>Файлы</b><ul>", "</table><hr><b>Files</b><ul>"},
      {" <i>(перекрывает:", " <i>(overrides:"},
      {" <i>(модом ", " <i>(by "},
      {"<h3>Перекрывает</h3>", "<h3>Overrides</h3>"},
      {"<h3>Перекрыт</h3>", "<h3>Overridden</h3>"},
      {"<p>Нет.</p>", "<p>None.</p>"},
      // --- dialogs / messages ---
      {"Выберите папку GTA San Andreas", "Select the GTA San Andreas folder"},
      {"Выберите папку GTA San Andreas (с gta_sa.exe)",
       "Select the GTA San Andreas folder (with gta_sa.exe)"},
      {"Инициализация", "Initialization"},
      {"gta_sa.exe не найден", "gta_sa.exe not found"},
      {"В этой папке нет gta_sa.exe. Всё равно использовать?",
       "There is no gta_sa.exe in this folder. Use it anyway?"},
      {"Смена папки игры", "Change game folder"},
      {"Новый профиль", "New profile"},
      {"Название:", "Name:"},
      {"Переименовать профиль", "Rename profile"},
      {"Новое название:", "New name:"},
      {"Переименование профиля", "Profile rename"},
      {"Копировать профиль", "Copy profile"},
      {"Копирование профиля", "Profile copy"},
      {"Название копии:", "Copy name:"},
      {"Скопировать сохранения (*.b)", "Copy savegames (*.b)"},
      {"Скопировать настройки (gta_sa.set)", "Copy settings (gta_sa.set)"},
      {"Профиль скопирован: ", "Profile copied: "},
      {"Удаление профиля", "Delete profile"},
      {"Удалить профиль «%1»?", "Delete profile “%1”?"},
      {"Сохранения профилей", "Profile saves"},
      {"Свои сохранения для профилей включены — подменяются на время игры",
       "Per-profile saves enabled — swapped in for the game session"},
      {"Свои сохранения для профилей выключены", "Per-profile saves disabled"},
      {"Сохранения профиля", "Profile saves"},
      {"Сначала создайте или выберите профиль.", "Create or select a profile first."},
      {"Импорт", "Import"},
      {"Уже в пуле как «%1»", "Already in the pool as “%1”"},
      {"Импортирован «%1»", "Imported “%1”"},
      {"Импортировано модов: %1", "Imported %1 mods"},
      {"Выберите папку мода", "Select the mod folder"},
      {"Выберите архив мода", "Select the mod archive"},
      {"Выберите папку игры со сборкой (модами)",
       "Select the game folder with the build (mods)"},
      {"Импорт сборки", "Import build"},
      {"Имя нового профиля для сборки:", "Name for the build's new profile:"},
      {"Сравнение сборки с ванилью… (первый раз может занять минуту)",
       "Diffing the build against vanilla… (the first run may take a minute)"},
      {"Импортирована сборка в профиль «%1»: %2 модов "
       "(%3 loose, %4 в IMG; %5 пропущено).",
       "Imported the build into profile “%1”: %2 mods "
       "(%3 loose, %4 in IMG; %5 ignored)."},
      {"Пропущенные файлы (%1):", "Skipped files (%1):"},
      {"Архивы модов (*.zip *.7z *.rar *.tar *.gz *.bz2 *.xz);;Все файлы (*)",
       "Mod archives (*.zip *.7z *.rar *.tar *.gz *.bz2 *.xz);;All files (*)"},
      {"Разделитель", "Separator"},
      {"Добавить разделитель", "Add separator"},
      {"Новый раздел", "New section"},
      {"Развёртывание", "Deployment"},
      {"Развёрнуто файлов: %1 (+ IMG) в игру", "Deployed %1 files (+ IMG) to the game"},
      {"Откат", "Rollback"},
      {"Откат выполнен — папка игры восстановлена",
       "Rolled back — the game folder is restored"},
      {"Откат выполнен, но посторонние файлы НЕ убраны — нет "
       "эталона ванили (Настройки → Развёртывание)",
       "Rolled back, but stray files were NOT removed -- no vanilla baseline "
       "yet (Settings → Deployment)"},
      {"Запуск", "Launch"},
      {"О программе GMM", "About GMM"},
      {"Переименовать разделитель…", "Rename separator…"},
      {"Удалить разделитель", "Delete separator"},
      {"Включить", "Enable"},
      {"Выключить", "Disable"},
      {"Открыть папку мода", "Open mod folder"},
      {"Удалить из пула…", "Remove from pool…"},
      {"Удаление мода", "Delete mod"},
      {"Удалить мод «%1» из пула? Действие необратимо.",
       "Remove mod “%1” from the pool? This cannot be undone."},
      {"Сменить язык", "Change language"},
      {"Язык изменится после перезапуска программы.",
       "The language will change after you restart the program."},
      // --- instances dialog ---
      {"Инстансы", "Instances"},
      {"Выберите инстанс или создайте новый, указав папку GTA San Andreas:",
       "Select an instance or create a new one by choosing a GTA San Andreas folder:"},
      {"Создать…", "Create…"},
      {"Удалить…", "Delete…"},
      {"Выбрать", "Select"},
      {"Отмена", "Cancel"},
      {"<папка игры не задана>", "<game folder not set>"},
      {"Новый инстанс", "New instance"},
      {"Название инстанса:", "Instance name:"},
      {"Удаление инстанса", "Delete instance"},
      {"Убрать инстанс «%1» из списка?\n\nДа = также удалить его данные (пул, "
       "профили).\nНет = оставить файлы на диске, только убрать из списка.",
       "Remove instance “%1” from the list?\n\nYes = also delete its data (pool, "
       "profiles).\nNo = keep the files on disk, only drop it from the list."},
      {"Сначала создайте инстанс (кнопка «Создать…»).",
       "Create an instance first (the “Create…” button)."},
      {"Построить эталон ванили для этого инстанса сейчас? Он нужен, "
       "чтобы откат мог убирать посторонние файлы из папки игры. "
       "Стройте, только если папка игры сейчас ДЕЙСТВИТЕЛЬНО чистая — "
       "в неё ещё ничего не разворачивали.",
       "Build the vanilla baseline for this instance now? It's needed for "
       "rollback to be able to remove stray files from the game folder. "
       "Only build it if the game folder is genuinely clean right now — "
       "nothing has been deployed into it yet."},
      {"Эталон ванили построен.", "Vanilla baseline built."},
      // --- Mod Loader integration ---
      {"Папка рантайма &Modloader…", "Mod Loader &runtime folder…"},
      {"Деплоить через Modloader", "Deploy via Mod Loader"},
      {"Modloader", "Mod Loader"},
      {"Импорт мода", "Import mod"},
      {"Как интегрировать этот мод?", "How should this mod be integrated?"},
      {"Менеджер сам инжектит файлы в IMG/loose (как обычно).",
       "The manager injects files into IMG/loose itself (as usual)."},
      {"Мод кладётся в modloader/<имя>/ и грузится рантаймом Modloader "
       "(нужен modloader.asi).",
       "The mod is placed in modloader/<name>/ and loaded by the Mod Loader "
       "runtime (needs modloader.asi)."},
      {"Имя мода:", "Mod name:"},
      // --- MoonLoader scripts: per-script edit/split ---
      {"Редактировать Lua-скрипт…", "Edit Lua script…"},
      {"Редактировать Lua-скрипт", "Edit Lua script"},
      {"Разбить на отдельные моды по скрипту…", "Split into per-script mods…"},
      {"Каждый moonloader/*.lua станет отдельным модом со своим "
       "вкл/выкл — как уже сделано для CLEO-скриптов и ASI-плагинов.",
       "Each moonloader/*.lua becomes its own mod with its own on/off toggle — "
       "the same way CLEO scripts and ASI plugins already work."},
      {"Скрипт:", "Script:"},
      {"Создано модов: %1. Обновлено профилей: %2.",
       "Mods created: %1. Profiles updated: %2."},
      {"Файл не найден в моде.", "File not found in the mod."},
      {"Не удалось открыть файл.", "Could not open the file."},
      {"Сохранение скрипта", "Saving script"},
      {"Скрипт сохранён. Если мод уже развёрнут через жёсткую ссылку — "
       "изменение уже видно в игре; иначе потребуется Развернуть заново.",
       "Script saved. If the mod is deployed via a hard link, the change is "
       "already visible in-game; otherwise you'll need to Deploy again."},
      {"Кодировка:", "Encoding:"},
      {"Кодировка", "Encoding"},
      {"Windows-1251 (кириллица)", "Windows-1251 (Cyrillic)"},
      {"Кодировка файла. Скрипт сохраняется в той же кодировке, в "
       "которой был открыт.",
       "The file's encoding. The script is saved back in the same encoding it "
       "was opened in."},
      {"Файл будет перечитан в выбранной кодировке. "
       "Несохранённые изменения будут потеряны. Продолжить?",
       "The file will be re-read in the selected encoding. Unsaved changes "
       "will be lost. Continue?"},
      {"В тексте есть символы, которых нет в Windows-1251 — при "
       "сохранении они станут «?». Сохранить файл в UTF-8?",
       "The text contains characters Windows-1251 cannot represent - they "
       "would be saved as '?'. Save the file as UTF-8 instead?"},
      {"Несохранённые изменения", "Unsaved changes"},
      {"Скрипт изменён, но не сохранён. Сохранить перед выходом?",
       "The script has unsaved changes. Save before closing?"},
      {"Напрямую", "Directly"},
      {"Через Modloader", "Via Mod Loader"},
      {"Рантайм Modloader", "Mod Loader runtime"},
      {"Папка рантайма Modloader:", "Mod Loader runtime folder:"},
      {"Папка рантайма Modloader", "Mod Loader runtime folder"},
      {"Рантайм найден (modloader.asi).", "Runtime found (modloader.asi)."},
      {"Рантайм не найден. Положите сюда modloader.asi и ASI-loader "
       "(напр. dinput8.dll) — менеджер установит их в игру при "
       "развёртывании мода через Modloader.",
       "Runtime not found. Put modloader.asi and an ASI loader (e.g. dinput8.dll) "
       "here — the manager installs them into the game when deploying a mod via "
       "Mod Loader."},
      {"Открыть папку", "Open folder"},
      {"Выбрать другую…", "Choose another…"},
      // --- Settings dialog ---
      {"Версия", "Version"},
      {"⚙ &Настройки", "⚙ &Settings"},
      {"&Открыть настройки…", "&Open settings…"},
      {"Текущий инстанс", "Current instance"},
      {"Папка игры:", "Game folder:"},
      {"Папка данных:", "Data folder:"},
      {"Активный профиль:", "Active profile:"},
      {"(не задана)", "(not set)"},
      {"(не выбран)", "(not selected)"},
      {"Настройки", "Settings"},
      {"Настройки программы", "Program settings"},
      {"Общие", "General"},
      {"Развёртывание", "Deployment"},
      {"Внешний вид", "Appearance"},
      {"Тема:", "Theme:"},
      {"Язык:", "Language:"},
      {"Тема применяется сразу. Язык — после перезапуска.",
       "The theme applies immediately. The language changes after a restart."},
      {"Сохранения и чистота", "Saves & cleanliness"},
      {"Свои сохранения для профилей", "Per-profile saves"},
      {"Каждый профиль получает свою папку GTA San Andreas User Files (сейвы + "
       "настройки), подменяемую на время игры.",
       "Each profile gets its own GTA San Andreas User Files folder (saves + "
       "settings), swapped in for the duration of the game."},
      {"Открыть папку сохранений профиля", "Open profile saves folder"},
      {"Открыть папку &сохранений профиля", "Open profile &saves folder"},
      {"Держать папку игры чистой", "Keep the game folder clean"},
      {"Файлы, создаваемые модами/игрой в папке игры, после выхода убираются в "
       "стор профиля и возвращаются перед следующим запуском — папка игры "
       "остаётся в исходном состоянии.",
       "Files a mod or the game creates in the game folder are moved into the "
       "profile's store on exit and restored before the next launch — the game "
       "folder stays in its original state."},
      {"Открыть папку созданных файлов профиля",
       "Open profile generated-files folder"},
      {"Открыть папку созданных &файлов профиля",
       "Open profile generated-&files folder"},
      {"Эталон ванили есть — откат убирает и посторонние файлы, "
       "не относящиеся ни к одному деплою.",
       "Vanilla baseline present — rollback also sweeps up stray files that "
       "belong to no deploy."},
      {"Эталон ванили не построен — откат НЕ убирает файлы, "
       "оставшиеся вне манифеста деплоя (см. кнопку ниже).",
       "No vanilla baseline yet — rollback does NOT remove files left outside "
       "the deploy manifest (see the button below)."},
      {"Построить эталон ванили сейчас", "Build the vanilla baseline now"},
      {"Эталон ванили", "Vanilla baseline"},
      {"Стройте эталон, ТОЛЬКО когда папка игры сейчас действительно "
       "чистая: ничего не задеплоено и нет посторонних файлов. Он "
       "запоминает текущее состояние папки как «ваниль» — если в ней "
       "уже что-то лишнее, откат будет считать это нормой и не "
       "уберёт. Продолжить?",
       "Only build this while the game folder is genuinely clean right now: "
       "nothing deployed, no stray files. It remembers the folder's current "
       "state as \"vanilla\" -- if something unwanted is already in there, "
       "rollback will treat it as normal and leave it. Continue?"},
      {"Настройки при первом запуске", "First-run settings"},
      {"Шаблон сохранён — новые профили получат его "
       "автоматически на первом запуске.",
       "Template saved — new profiles will get it automatically on first launch."},
      {"Шаблон не задан — новые профили получат настройки "
       "игры по умолчанию.",
       "No template set — new profiles will get the game's own default settings."},
      {"Один раз настрой разрешение экрана и управление в самой игре, затем "
       "нажми «Сохранить». Дальше при первом запуске КАЖДОГО нового профиля "
       "или инстанса эти настройки (gta_sa.set) подставятся сами до старта "
       "игры — мастер настройки экрана не появится.",
       "Set your screen resolution and controls in the game once, then click "
       "\"Save\". From then on, every new profile or instance will get these "
       "settings (gta_sa.set) seeded automatically before the game even "
       "starts — the resolution setup wizard won't appear."},
      {"Сохранить текущие настройки как шаблон", "Save current settings as template"},
      {"Очистить шаблон", "Clear template"},
      {"Шаблон настроек", "Settings template"},
      {"Карты (экспериментально)", "Maps (experimental)"},
      {"Авто-установка карт", "Auto-install maps"},
      {"Steam", "Steam"},
      {"Показывать в Steam, что я играю в GTA San Andreas",
       "Show in Steam that I'm playing GTA San Andreas"},
      {"Пока идёт игра, GMM подключается к запущенному Steam под настоящим "
       "AppID Grand Theft Auto: San Andreas — как это делает Steam Achievement "
       "Manager. У друзей и в клиенте видно «играет в Grand Theft Auto: San "
       "Andreas», время идёт в счёт настоящей страницы игры. Библиотеку Steam "
       "(steam_api64.dll) GMM подхватывает сам — из установленных у вас игр "
       "Steam, так что настраивать обычно нечего. Ничего в файлах "
       "Steam и в папке игры не меняется. Работает ТОЛЬКО если GTA San "
       "Andreas куплена на этом аккаунте Steam — иначе клиент не примет "
       "сессию, и игра просто запустится без статуса. Оверлея (Shift+Tab, "
       "скриншоты F12) не будет: его Steam добавляет только тем процессам, "
       "которые запустил сам. По умолчанию выключено.",
       "While the game is running, GMM connects to the running Steam client "
       "under Grand Theft Auto: San Andreas's real AppID — the same thing "
       "Steam Achievement Manager does. Friends and the client see \"playing "
       "Grand Theft Auto: San Andreas\", and the time counts towards the real "
       "store page. GMM picks up the Steam library (steam_api64.dll) by "
       "itself, from a game you already have installed through Steam, so there "
       "is usually nothing to set up. Nothing in Steam's files or in the game "
       "folder is changed. "
       "This works ONLY if GTA San Andreas is owned on this Steam account — "
       "otherwise the client refuses the session and the game simply starts "
       "with no status. There is no overlay (Shift+Tab, F12 screenshots): "
       "Steam only adds it to processes it launched itself. Off by default."},
      {"Тип", "Type"},
      {"Данные", "Data"},
      {"Прочее", "Other"},
      {"Редактировать Lua-скрипт мода", "Edit this mod's Lua script"},
      {"В этом моде нет Lua-скрипта для правки.",
       "This mod has no Lua script to edit."},
      {"Steam найден и запущен.", "Steam found and running."},
      {"Steam найден (сейчас не запущен — запустите его до начала игры).",
       "Steam found (not running — start it before playing)."},
      {"Steam на этом компьютере не найден.", "Steam was not found on this machine."},
      {"Библиотека Steam найдена: ", "Steam library found: "},
      {"Библиотека Steam взята автоматически из установленной игры: ",
       "Steam library picked up automatically from an installed game: "},
      {"Указать steam_api64.dll…", "Choose steam_api64.dll…"},
      {"Выберите steam_api64.dll", "Select steam_api64.dll"},
      {"Библиотека Steam (steam_api64.dll)", "Steam library (steam_api64.dll)"},
      {"Нет steam_api64.dll — положите её сюда: ",
       "steam_api64.dll is missing — put it here: "},
      {"Статус в Steam включён: пока идёт игра, в Steam будет «играет в Grand "
       "Theft Auto: San Andreas».",
       "Steam status enabled: while the game runs, Steam will show \"playing "
       "Grand Theft Auto: San Andreas\"."},
      {"Статус в Steam включён, но нет steam_api64.dll — путь показан в "
       "настройках, без неё статус не появится.",
       "Steam status enabled, but steam_api64.dll is missing — the path is "
       "shown in Settings; without it no status will appear."},
      {"Статус в Steam выключен.", "Steam status disabled."},
      {"Интеграция со Steam", "Steam integration"},
      {"Мультиплеер", "Multiplayer"},
      {"Настройки SA-MP (мультиплеер)…", "SA-MP (multiplayer) settings…"},
      {"Внешние программы", "External tools"},
      {"Sanny Builder 4:", "Sanny Builder 4:"},
      {"Папка Sanny Builder 4 (например C:\\...\\SannyBuilder-v4.2.0)",
       "Sanny Builder 4 folder (e.g. C:\\...\\SannyBuilder-v4.2.0)"},
      {"Папка Sanny Builder 4", "Sanny Builder 4 folder"},
      {"Показывать Sanny Builder на панели",
       "Show Sanny Builder on the toolbar"},
      {"Путь не указан.", "No path set."},
      {"Sanny Builder не найден по этому пути (ожидается sanny.exe).",
       "No Sanny Builder at this path (sanny.exe expected)."},
      {"Найден: ", "Found: "},
      {"Sanny Builder не входит в GMM и не участвует в развёртывании — "
       "программа только запоминает, где лежит ваша копия, и добавляет кнопку "
       "запуска на панель (рядом с SAMP). Кнопка появляется, только если "
       "галочка включена и по указанному пути действительно есть sanny.exe.",
       "Sanny Builder is not part of GMM and plays no role in deployment - the "
       "program only remembers where your own copy lives and adds a button to "
       "the toolbar (next to SAMP). The button appears only when the checkbox "
       "is on and the path really does hold sanny.exe."},
      {"Sanny Builder", "Sanny Builder"},
      {"Запустить Sanny Builder", "Launch Sanny Builder"},
      {"Путь к Sanny Builder не указан или в нём нет исполняемого файла. "
       "Укажите его в Настройках.",
       "The Sanny Builder path is unset or holds no executable. Set it in "
       "Settings."},
      {"Не удалось запустить Sanny Builder.", "Could not launch Sanny Builder."},
      {"Sanny Builder запущен.", "Sanny Builder started."},
      {"Sanny Builder: ", "Sanny Builder: "},
      {"Компилировать CLEO в мод:", "Compile CLEO into mod:"},
      {"— не менять (папка игры) —", "— leave alone (game folder) —"},
      {"При запуске по кнопке GMM передаёт в Sanny Builder папку игры этого "
       "инстанса (settings.ini → GamePath — оттуда он берёт gta.dat, "
       "american.gxt и подсказки) и, если выбран мод, перенаправляет вывод "
       "компиляции CLEO в mods/<мод>/root/CLEO (mode.xml текущего режима). "
       "Компилировать прямо в папку игры не стоит: она создаётся "
       "развёртыванием, и откат такой скрипт удалит, а перезапись "
       "задеплоенного файла рвёт жёсткую ссылку — мод в пуле останется со "
       "старой версией.",
       "When started from that button, GMM passes this instance's game folder "
       "to Sanny Builder (settings.ini -> GamePath, where it reads gta.dat, "
       "american.gxt and the hints from) and, if a mod is selected, redirects "
       "CLEO compile output into mods/<mod>/root/CLEO (the current edit mode's "
       "mode.xml). Compiling straight into the game folder is a bad idea: that "
       "folder is produced by deployment, so a rollback deletes the script, "
       "and overwriting a deployed file breaks its hard link, leaving the pool "
       "copy stale."},
      {"Включить Mod Loader", "Enable Mod Loader"},
      {"⚠ Mod Loader не установился — рантайм не найден "
       "(см. Настройки → Mod Loader).",
       "⚠ Mod Loader did not install — runtime not found "
       "(see Settings → Mod Loader)."},
      {"Ставит в игру полный рантайм Modloader (сам Modloader, CLEO и его "
       "расширения, снятие DEP, SilentPatch, широкоформатные фиксы, Framerate "
       "Vigilante и др.) при каждом развёртывании — даже если ни один мод не "
       "помечен «через Modloader». Откат убирает всё обратно. Встроено в "
       "программу, ничего скачивать не нужно.",
       "Installs the full Mod Loader runtime into the game (Mod Loader itself, "
       "CLEO and its extensions, DEP removal, SilentPatch, widescreen fixes, "
       "Framerate Vigilante, etc.) on every deploy — even if no mod is flagged "
       "\"via Mod Loader\". Rollback removes it all again. Bundled with the "
       "program, nothing to download."},
      {"Мод можно ТАКЖЕ деплоить «через Modloader» отдельно (правый клик по "
       "моду в списке → «Деплоить через Modloader»): тогда он кладётся в "
       "modloader/<имя>/ и грузится рантаймом Modloader. Иначе — нативно "
       "(инъекция в IMG/loose).",
       "A mod can ALSO be deployed “via Mod Loader” individually (right-click a "
       "mod → “Deploy via Mod Loader”): it is placed in modloader/<name>/ and "
       "loaded by the Mod Loader runtime. Otherwise it deploys natively "
       "(injected into IMG/loose)."},
      {"Рантайм не найден.", "Runtime not found."},
      {"Обзор…", "Browse…"},
      {"Открыть", "Open"},
      {"Сбросить (по умолчанию)", "Reset (default)"},
      {"Рантайм Mod Loader встроен в программу и разворачивается автоматически — "
       "указывать пути обычно НЕ нужно. Поля выше нужны, только если хотите взять "
       "рантайм из своей папки. Менеджер сам ставит его в игру при развёртывании "
       "мода через Modloader и убирает при откате.",
       "The Mod Loader runtime is embedded in the program and unpacked "
       "automatically — you normally don't need to set any paths. The fields above "
       "are only for taking the runtime from your own folder. The manager installs "
       "it into the game when deploying a Mod Loader mod and removes it on rollback."},
      {"Исполняемый файл игры", "Game executable"},
      {"Заменять gta_sa.exe встроенным (чистый 1.0)",
       "Replace gta_sa.exe with the bundled clean 1.0"},
      {"Замена exe", "Exe replacement"},
      {"Встроенный gta_sa.exe не найден — положите его в рантайм "
       "(gtamm/runtime/) и пересоберите, либо в папку рантайма.",
       "No bundled gta_sa.exe found — put it into the runtime (gtamm/runtime/) and "
       "rebuild, or into the runtime folder."},
      {"При развёртывании подменяет gta_sa.exe игры на заведомо чистый GTA SA "
       "1.0 US. Оригинал сохраняется и восстанавливается при откате. Включается "
       "И АВТОМАТИЧЕСКИ, когда включён Mod Loader (выше) — он рассчитан именно "
       "на 1.0, и другой билд exe (Steam/пиратка/другой регион) — частая причина "
       "«Mod Loader не активируется». Этот чекбокс — для случая, когда нужна "
       "только замена exe без Mod Loader.",
       "On deploy, replaces the game's gta_sa.exe with a known-clean GTA SA 1.0 "
       "US. The original is backed up and restored on rollback. Also turned on "
       "AUTOMATICALLY whenever Mod Loader is enabled (above) -- it's built for "
       "1.0, and a different exe build (Steam/cracked/other region) is a common "
       "reason \"Mod Loader doesn't activate\". This checkbox is for when you "
       "want the exe swap without Mod Loader."},

      // --- found by the RU->EN coverage audit: lang::T() calls and static
      // chrome (menus/buttons/tooltips translated via translateUi()) that had
      // no dictionary entry at all, so they stayed in Russian even with
      // English selected ---
      {"Mod Loader", "Mod Loader"},
      {"SilentPatch, ДВА широкоформатных фикса, Framerate Vigilante, RunDLL32 "
       "Fix и Windowed Mode — идут вместе с Modloader отдельным слоем. "
       "Выключите, если сборка/профиль уже содержит СВОИ копии этих же "
       "фиксов: две копии одного и того же (напр. widescreen) плагина, "
       "хукающие один и тот же D3D9/FOV, — частая причина чёрного мира и "
       "рваного меню во время игры.",
       "SilentPatch, TWO widescreen fixes, Framerate Vigilante, RunDLL32 Fix "
       "and Windowed Mode -- installed alongside Modloader as a separate "
       "layer. Turn this off if the build/profile already carries its OWN "
       "copies of these same fixes: two copies of the same (e.g. widescreen) "
       "plugin hooking the same D3D9/FOV is a common cause of a black world "
       "and a glitchy, tearing pause menu during play."},
      {"Архив защищён паролем. Введите пароль:",
       "The archive is password-protected. Enter the password:"},
      {"Встроенный клиент SA-MP не найден — недостающие файлы придётся "
       "переносить вручную (см. runtime-samp/README.txt).",
       "No bundled SA-MP client found -- you'll have to copy the missing "
       "files in manually (see runtime-samp/README.txt)."},
      {"Импорт отменён — нужен пароль архива",
       "Import cancelled -- the archive needs a password"},
      {"Мод можно ТАКЖЕ деплоить «через Modloader» отдельно (правый клик по "
       "моду в списке → «Деплоить через Modloader»): тогда он кладётся в "
       "modloader/<имя>/, а не инжектится нативно в IMG/loose. Но грузит его "
       "именно рантайм Modloader выше — если этот чекбокс выключен, файлы "
       "мода всё равно лягут в modloader/<имя>/, но подхватывать их будет "
       "нечему, и мод молча не заработает.",
       "A mod can ALSO be deployed \"via Modloader\" individually (right-click "
       "the mod in the list -> \"Deploy via Modloader\"): it then lands under "
       "modloader/<name>/ instead of being injected natively into IMG/loose. "
       "But it's the Modloader runtime above that actually loads it -- if "
       "this checkbox is off, the mod's files still land under "
       "modloader/<name>/, but nothing will be there to pick them up, and the "
       "mod will silently not work."},
      {"Не удалось загрузить изображение.", "Could not load the image."},
      {"Неверный пароль. Попробуйте ещё раз:", "Wrong password. Try again:"},
      {"Нет описания сборки. Нажмите «Редактировать сборку», чтобы добавить "
       "текст и изображения.",
       "No build description yet. Click \"Edit build\" to add text and "
       "images."},
      {"Пароль архива", "Archive password"},
      {"Профиль не выбран.", "No profile selected."},
      {"Разбивка на отдельные моды", "Split into separate mods"},
      {"Ставит в игру рантайм Modloader (сам Modloader, CLEO и его "
       "расширения, снятие DEP) при каждом развёртывании — даже если ни один "
       "мод не помечен «через Modloader». Откат убирает всё обратно. "
       "Встроено в программу, ничего скачивать не нужно.",
       "Installs the Modloader runtime into the game (Modloader itself, CLEO "
       "and its extensions, DEP removal) on every deploy -- even if no mod is "
       "flagged \"via Modloader\". Rollback removes it all again. Bundled "
       "with the program, nothing to download."},
      {"Устанавливать встроенные фиксы (_ESSENTIALS)",
       "Install bundled compatibility fixes (_ESSENTIALS)"},
      {"⚠ Файл не найден, ни один включённый мод его не разворачивает, и "
       "встроенного клиента с таким файлом тоже нет. Если samp.exe входит в "
       "сборку — проверь, что нужный мод включён; если нет — установи "
       "клиент SA-MP в папку игры вручную, либо положи полный клиент в "
       "gtamm/runtime-samp/ (см. Mod Loader → статус ниже).",
       "⚠ File not found -- no enabled mod deploys it, and there's no bundled "
       "client with that file either. If samp.exe is part of a mod in this "
       "build, check that mod is enabled; otherwise install the SA-MP client "
       "into the game folder yourself, or drop a full client into "
       "gtamm/runtime-samp/ (see Mod Loader -> status below)."},
      {"✓ Найден встроенный клиент SA-MP — при развёртывании менеджер сам "
       "докладывает недостающие файлы (mouse.png, sampgui.png, архивы "
       "SAMP/), даже если мод с SA-MP импортирован не полностью.",
       "✓ Bundled SA-MP client found -- on deploy the manager fills in any "
       "missing files (mouse.png, sampgui.png, the SAMP/ archives) itself, "
       "even if an imported SA-MP mod is incomplete."},

      // --- Info-tab rich-text formatting toolbar (static chrome, translated
      // via translateUi(), same RU/EN convention as Word/LibreOffice: B/I/U
      // for Bold/Italic/Underline rather than a literal transliteration) ---
      {"Ж", "B"},
      {"Жирный (Ctrl+B)", "Bold (Ctrl+B)"},
      {"К", "I"},
      {"Курсив (Ctrl+I)", "Italic (Ctrl+I)"},
      {"Ч", "U"},
      {"Подчёркнутый (Ctrl+U)", "Underline (Ctrl+U)"},
      {"Слева", "Left"},
      {"По левому краю", "Align left"},
      {"Центр", "Center"},
      {"По центру", "Align center"},
      {"Справа", "Right"},
      {"По правому краю", "Align right"},
      {"Ширина", "Justify"},
      {"По ширине", "Justify"},
      {"Размер текста", "Text size"},
  };
  return d;
}

}  // namespace

void setLanguage(Language l)
{
  g_lang = l;
}

Language language()
{
  return g_lang;
}

bool isEnglish()
{
  return g_lang == Language::English;
}

Language loadSaved()
{
  return QSettings().value("lang", "ru").toString() == "en" ? Language::English
                                                            : Language::Russian;
}

void save(Language l)
{
  QSettings().setValue("lang", l == Language::English ? "en" : "ru");
}

QString T(const QString& ru)
{
  if (g_lang != Language::English)
    return ru;
  const auto it = dict().find(ru);
  return it != dict().end() ? it.value() : ru;
}

void translateUi(QWidget* root)
{
  if (!isEnglish() || !root)
    return;
  root->setWindowTitle(T(root->windowTitle()));
  // Actions cover menus, submenus and toolbar buttons.
  for (QAction* a : root->findChildren<QAction*>()) {
    a->setText(T(a->text()));
    if (!a->toolTip().isEmpty())
      a->setToolTip(T(a->toolTip()));
  }
  for (QMenu* m : root->findChildren<QMenu*>())
    m->setTitle(T(m->title()));
  for (QLabel* l : root->findChildren<QLabel*>())
    l->setText(T(l->text()));
  for (QAbstractButton* b : root->findChildren<QAbstractButton*>()) {
    b->setText(T(b->text()));
    if (!b->toolTip().isEmpty())
      b->setToolTip(T(b->toolTip()));
  }
  for (QTabWidget* t : root->findChildren<QTabWidget*>())
    for (int i = 0; i < t->count(); ++i)
      t->setTabText(i, T(t->tabText(i)));
  for (QLineEdit* e : root->findChildren<QLineEdit*>())
    e->setPlaceholderText(T(e->placeholderText()));
}

}  // namespace gtamm::lang
