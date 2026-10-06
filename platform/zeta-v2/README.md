# Zeta V2 platform layer

Это слой, отвечающий за низкоуровневую адаптацию к Zeta SBC V2.

## Обязанности слоя

- инициализация bank registers
- paging enable
- mapping kernel pages
- mapping user process pages
- сохранение/восстановление текущего mapping
- безопасное copyin/out
- interrupt-safe switch paths

## Базовый контракт

- bank registers управляют физической страницей, не процессом
- kernel имеет отдельную карту отображения
- user mapping хранится отдельно
- копирование между процессами должно работать через логический адрес и перевод на физические страницы
