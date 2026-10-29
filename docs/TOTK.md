# TOTK Recomp: сборка, запуск и технические заметки

Как собрать и запустить TOTK Recomp на Linux, на примере Steam Deck (SteamOS), и что уже пришлось изменить в
Savage Wind ради TOTK. Пути ниже — для примера: подставьте свои.

```
T="/home/deck/Games/The Legend of Zelda Tears of the Kingdom"   # папка проекта игры
# "$T/Game Files/exefs"  - ExeFS с обновлением (main, main.npdm, rtld, sdk, subsdk0)
# "$T/Game Files/romfs"  - RomFS с применённым обновлением
```

## 1. Исходники и генератор

```
git clone https://github.com/allians00758/totk-recomp ~/Downloads/SavageWindRecomp
cd ~/Downloads/SavageWindRecomp
python3 -m venv tools/.venv && tools/.venv/bin/pip install capstone==5.0.9 unicorn==2.1.4 lz4==4.4.5
```

## 2. Перевод кода игры в C

```
tools/.venv/bin/python tools/gen.py all "$T/Game Files/exefs" "$T/recomp" \
    --hle runtime/hle/hle_symbols.txt --no-sites 2>&1 | grep -v progress
```

`--no-sites`: без адресов и патчей, которые Savage Wind ищет в коде BotW (меню настроек, пауза, частота кадров).
Около полутора минут на Steam Deck; ~1 ГБ C в `"$T/recomp"`. Неизменившиеся файлы генератор не перезаписывает.

## 3. Компиляция

Компилятор — zig из AppImage Savage Wind (`--appimage-extract`): на SteamOS нет системных заголовков C, а zig
приносит свои. Компилируются только новые или изменившиеся файлы; ошибки — в `"$T/compile_errors.log"`.

```
Z=~/Downloads/squashfs-root/usr/bin/zig/zig
export ZIG_GLOBAL_CACHE_DIR="$T/zig-cache" ZIG_LOCAL_CACHE_DIR="$T/zig-cache"
mkdir -p "$T/recomp/obj"
find "$T/recomp" -name "*.c" -not -path "*/obj/*" -print0 | \
  xargs -0 -P 6 -I{} sh -c '
    o="$1/recomp/obj/$(basename "{}" .c).o"; [ -f "$o" ] && [ "$o" -nt "{}" ] && exit 0
    "$2" cc -target x86_64-linux-gnu.2.35 -mcpu=x86_64_v3 -O1 -fno-strict-aliasing -fvisibility=hidden -fPIC \
      -Wno-return-type -Wno-unused-label -Iruntime -c "{}" -o "$o.tmp" 2>"$o.err" && mv "$o.tmp" "$o" && rm -f "$o.err" \
      || echo "FAIL {}" >> "$1/compile_errors.log"
    echo -n .' _ "$T" "$Z"
```

Полная компиляция на Steam Deck — около часа (6 потоков, чтобы хватило памяти).

## 4. Рантайм и библиотека игры (в контейнере)

Рантайму нужны SDL3, FFmpeg, shaderc и Vulkan. На SteamOS их удобно ставить в distrobox:

```
distrobox create -n swbuild -i docker.io/library/ubuntu:26.04 -Y
distrobox enter swbuild
sudo apt update && sudo apt install -y clang lld make pkg-config git libsdl3-dev libavformat-dev libavcodec-dev \
  libavutil-dev libswscale-dev libvulkan-dev libshaderc-dev glslc vulkan-tools mesa-vulkan-drivers
```

Дальше, внутри контейнера:

```
cd ~/Downloads/SavageWindRecomp
make -j4
tools/link_gamelib.sh "$T/recomp/obj" "$T/Game Files"     # -> swgame.so и swgame.so.id
build/savagewind --build-romfs "$T/Game Files"             # -> romfs.bin (размер RomFS, ~16 ГБ), один раз
```

Рантайм загружает только библиотеку с тем же идентификатором (`make gamelib-id`), что и у него самого. Идентификатор
зависит от генератора и заголовков, поэтому после их изменения нужно пересобрать и рантайм, и `swgame.so`.

## 5. Запуск

```
SAVAGEWIND_NO_LAUNCHER=1 build/savagewind "$T/Game Files" 2>&1 | tee "$T/run.log"
```

Без стартового экрана Savage Wind: он проверяет версию BotW. Вылет печатает `*** signal ...` или `FATAL: ...`,
регистры гостевого процессора и обратную трассировку; функции игры называются `f_<адрес>`.

## Что изменено ради TOTK

| Что | Почему |
|---|---|
| `gamebuild.c`: явная цель `-target x86_64-linux-gnu.2.35` для zig | На SteamOS нет заголовков glibc: сборка кода игры падала с `LibCStdLibHeaderNotFound` (это и в Savage Wind) |
| `lifter.py`, `cpu.h`: `LDXP`/`LDAXP`/`STXP`/`STLXP` | Две функции `main` пишут 128-битные значения атомарно |
| `gamelib.c`, `framepace.c`, `fs.c`, `game_options.mm`: код без адресов (`GAME_SITES_KNOWN`) | Всё, что лезет во внутренности BotW (меню, пауза, темп кадров, строка версии, голоса), для TOTK выключено |
| `Makefile` `gamelib-id`, `tools/link_gamelib.sh` | Сборка `swgame.so` из объектов, скомпилированных вне `make` |
| `kernel.c`: `GetInfo(16)` = system resource из `main.npdm`; `MapPhysicalMemory`/`UnmapPhysicalMemory` по страницам | Виртуальная память SDK (`nn::os::AllocateAddressRegion`): без неё `VammManager` оставался пустым (`signal 11 accessing 0xc8`) |
| `gen.py`: цели `br` из `adr`/`adrp+add` внутри функции | Распаковщик zstd хранит точку возврата цикла в `x19` (`FATAL: no function at 70001bfabc`) |

## Инструменты для разбора

- `tools/gen.py` — с `--no-sites` работает для любой игры, раскладку модулей даёт `tools/layout.py`.
- Символы SDK есть в самом `sdk` (≈23 тыс. экспортов): по адресу из трассировки видно, какая это функция `nn::`.
- Дизассемблер: `llvm-mc --disassemble -triple=aarch64` по байтам распакованного NSO (`tools/nso.py`).
- Переменные окружения `SAVAGEWIND_*` для отладки — в [SAVAGE-WIND.md](SAVAGE-WIND.md) (заметки Savage Wind, на
  испанском).
