folder-share
===

[![asciicast](https://asciinema.org/a/gBDH0ciu8qwF1FfklpdZcTxlt.svg)](https://asciinema.org/a/gBDH0ciu8qwF1FfklpdZcTxlt)

Транслируйте свою директорию другим людям по вебсокетам, используя read-only файловую систему на основе [FUSE](https://wiki.archlinux.org/title/FUSE_(%D0%A0%D1%83%D1%81%D1%81%D0%BA%D0%B8%D0%B9)) (filesystem in userspace).

- Асинхронный клиент, написанный на C++, используя библиотеку boost::beast.
- Асинхронный сервер на Python.
- Принимающий клиент монтирует директорию хост-системы в указанную mountpoint-директорию с помощью FUSE.
- Покрыто тестами используя pytest

Эта работа выполнена в рамках задания по курсу "Операционные системы" за 4-й семестр. Лабораторная #6. Работа с модулем FUSE

![fuse](https://upload.wikimedia.org/wikipedia/commons/thumb/0/08/FUSE_structure.svg/1280px-FUSE_structure.svg.png)

Идея очень похожа на [ssfs](https://github.com/libfuse/sshfs).

### Требования

Только linux

### Детали реализации.

Современный C++ 20 Coroutines TS и boost::beast для асинхронных вебсокетов.

Пришлось использовать более сложный интерфейс для определения операций `FUSE` из-за очень странных системных вызовов fuse_main, которые удаляют все потоки(

Клиент создает сессию с fuse_lowlevel_ops, монтирует её к директории mountpoint и запускает два потока:
- Один для выполнения асинхронных задач контекста boost.
- Другой для выполнения `fuse_session_loop(se)`.

### Установка

1. Создайте виртуальное окружение:

   `python3 -m venv env`

2. Активируйте виртуальное окружение:

   `source env/bin/activate`

3. Установите зависимости Python:

   `pip3 install -r requirements.txt`

4. Установите зависимости C++ с помощью Conan:

   `conan install conanfile.txt --build=missing`

Note: Установка boost может занять много времени.

5. Соберите проект с помощью CMake:

   `cmake .`

   `cmake --build .`

Note: Билд проекта может занять несколько десятков секунд.

### Запуск локально

Откройте 3 консоли.

1. Запустите сервер:

   ` python3 server.py --pytest`

   Note: Опция --pytest генерирует uuid равный 123 и слушает 0.0.0.0:8765

2. Запустите клиента для взаимодействия с сервером:

   `./client`

   Скопируйте сгенерированный uuid для запуска принимающего клиента.

3. Запустите принимающий клиент:

   `./client 123`

   Note: Или используйте другой uuid, сгенерированный сервером.