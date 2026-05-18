# mytop

Приложение для отображения загрузки ресурсов ПК

## Требования к сборке

- Linux 3.18 (или WSL)
- gcc / Clang (C++20)
- CMake 3.14

## Сборка

```bash
# Клонировать репозиторий
git clone https://github.com/ayzee7/YADRO-kvadraOS-2
cd YADRO-kvadraOS-2

# Настроить сборку (Release)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Скомпилировать
cmake --build build --config Release --parallel
```

## Запуск

```bash
./build/mytop
# Приложение теперь доступно по адресу http://localhost:8080
```
