#include <not_implemented.h> // исключение для неподдерживаемых операций (объяснялось ранее)
#include "../include/allocator_red_black_tree.h" // собственный заголовок (объяснялся ранее)
#include <mutex>          // (объяснялся ранее)
#include <memory_resource> // (объяснялся ранее)
#include <stdexcept>      // (объяснялся ранее)
#include <cstddef>        // (объяснялся ранее)
#include <cstring>        // (объяснялся ранее)
#include <vector>         // (объяснялся ранее)

namespace { // безымянное пространство имён (объяснялось ранее)

    constexpr size_t allocator_metadata_size_local =
        sizeof(std::mutex)                         // примитив синхронизации
        + sizeof(std::pmr::memory_resource*)       // указатель на родительский аллокатор
        + sizeof(size_t)                           // общий размер доверенной области
        + sizeof(void*)                            // указатель на корень красно-чёрного дерева
        + sizeof(allocator_with_fit_mode::fit_mode); // режим подбора блока
    static_assert(allocator_metadata_size_local == 108, "unexpected allocator metadata size"); // static_assert — проверка условия на этапе компиляции. если выражение ложно (здесь: если allocator_metadata_size_local != 108), компилятор выдаст ошибку с сообщением "unexpected allocator metadata size". это защита от несоответствия размеров при изменении типов или платформы. 108 — это фиксированный размер заголовка аллокатора (80+8+8+8+4), который используется для вычисления смещений.

    constexpr size_t mtx_offset        = 0;                                    // смещение мьютекса = 0 (начало доверенной памяти)
    constexpr size_t parent_offset     = sizeof(std::mutex);                  // смещение указателя на родителя = размер мьютекса
    constexpr size_t space_size_offset = parent_offset + sizeof(void*);       // смещение поля размера области
    constexpr size_t root_offset       = space_size_offset + sizeof(size_t);  // смещение указателя на корень дерева (root). корень — это указатель на корневой узел красно-чёрного дерева свободных блоков.
    constexpr size_t mode_offset       = root_offset + sizeof(void*);         // смещение режима размещения

    std::mutex& get_mtx(void* trusted) noexcept { // (объяснялось ранее)
        return *reinterpret_cast<std::mutex*>(static_cast<char*>(trusted) + mtx_offset);
    }
    std::pmr::memory_resource*& get_parent(void* trusted) noexcept { // (объяснялось ранее)
        return *reinterpret_cast<std::pmr::memory_resource**>(static_cast<char*>(trusted) + parent_offset);
    }
    size_t& get_space_size(void* trusted) noexcept { // (объяснялось ранее)
        return *reinterpret_cast<size_t*>(static_cast<char*>(trusted) + space_size_offset);
    }
    void*& get_root(void* trusted) noexcept { // возвращает ссылку на указатель корня красно-чёрного дерева. void*& — ссылка на указатель, позволяет изменять корень дерева (например, при вращениях или когда дерево становится пустым).
        return *reinterpret_cast<void**>(static_cast<char*>(trusted) + root_offset);
    }
    allocator_with_fit_mode::fit_mode& get_mode(void* trusted) noexcept { // (объяснялось ранее)
        return *reinterpret_cast<allocator_with_fit_mode::fit_mode*>(static_cast<char*>(trusted) + mode_offset);
    }
    void* region_start(void* trusted) noexcept { // (объяснялось ранее)
        return static_cast<char*>(trusted) + allocator_metadata_size_local;
    }

    // ---------- безопасное чтение/запись через memcpy (msvc debug) ----------
    void* read_ptr(void* addr) noexcept { // (объяснялось ранее)
        void* v;
        std::memcpy(&v, addr, sizeof(v));
        return v;
    }
    void write_ptr(void* addr, void* v) noexcept { // (объяснялось ранее)
        std::memcpy(addr, &v, sizeof(v));
    }

    allocator_red_black_tree::block_data read_bd(void* block) noexcept { // функция читает структуру block_data (1 байт с bit fields) из начала блока через memcpy. возвращает по значению: создаётся локальная копия block_data на стеке, которая заполняется байтом из памяти block.
        allocator_red_black_tree::block_data bd; // локальная переменная типа block_data.
        std::memcpy(&bd, block, sizeof(bd)); // копируем sizeof(bd) (1 байт) из памяти block в переменную bd.
        return bd; // возвращаем копию. так безопасно, т.к. block может быть невыровнен для структуры, а memcpy работает с любыми адресами.
    }
    void write_bd(void* block, allocator_red_black_tree::block_data bd) noexcept { // запись block_data в начало блока через memcpy.
        std::memcpy(block, &bd, sizeof(bd)); // копируем 1 байт из bd в память block.
    }

    bool blk_occupied(void* block) noexcept { // читает флаг занятости из bit fields. возвращает true, если блок занят.
        return read_bd(block).occupied; // read_bd возвращает block_data, обращаемся к полю occupied.
    }
    allocator_red_black_tree::block_color blk_color(void* block) noexcept { // читает цвет узла в дереве. особенность: если block == nullptr (т.е. несуществующий узел, «nil»), считаем его чёрным. это соответствует свойствам красно-чёрного дерева: все листья (nil-узлы) чёрные.
        if (block == nullptr) return allocator_red_black_tree::block_color::BLACK; // nil-узел — чёрный по определению.
        return read_bd(block).color; // для реального блока читаем цвет из bit fields.
    }
    void set_bd(void* block, bool occ, allocator_red_black_tree::block_color col) noexcept { // установка обоих полей bit fields одновременно. block_data bd{occ, col} — агрегатная инициализация: заполняем поля occupied и color в фигурных скобках в порядке объявления.
        allocator_red_black_tree::block_data bd{occ, col}; // создаём временную структуру с заданными значениями.
        write_bd(block, bd); // записываем её в память блока.
    }

    // ---------- физические связи prev/next (все блоки) ----------
    void* blk_prev(void* block) noexcept { return read_ptr(static_cast<char*>(block) + 1); } // читаем указатель на предыдущий физический блок по смещению +1 байт от начала блока (после 1-байтового block_data). смещение 1 используется, т.к. block_data занимает ровно 1 байт.
    void set_blk_prev(void* block, void* v) noexcept { write_ptr(static_cast<char*>(block) + 1, v); } // записываем указатель prev по смещению +1.
    void* blk_next(void* block) noexcept { return read_ptr(static_cast<char*>(block) + 9); } // читаем указатель на следующий физический блок по смещению +9 (после block_data + prev pointer). на 64-битной системе указатель занимает 8 байт, поэтому 1 + 8 = 9.
    void set_blk_next(void* block, void* v) noexcept { write_ptr(static_cast<char*>(block) + 9, v); } // записываем указатель next по смещению +9.

    // ---------- занятый блок: указатель на родительский аллокатор ----------
    void* blk_parent_alloc(void* block) noexcept { return read_ptr(static_cast<char*>(block) + 17); } // читаем указатель на родительский аллокатор по смещению +17 (после block_data + prev + next). используется для проверки принадлежности блока при освобождении.
    void set_blk_parent_alloc(void* block, void* v) noexcept { write_ptr(static_cast<char*>(block) + 17, v); } // записываем указатель родителя.

    // ---------- свободный блок: указатели дерева кч ----------
    void* blk_left(void* block) noexcept       { return read_ptr(static_cast<char*>(block) + 17); } // левый ребёнок в красно-чёрном дереве. смещение +17 совпадает с blk_parent_alloc, т.к. для свободного блока указатель родителя не нужен — на этом месте хранится left.
    void set_blk_left(void* block, void* v) noexcept       { write_ptr(static_cast<char*>(block) + 17, v); } // записываем левого ребёнка.
    void* blk_right(void* block) noexcept      { return read_ptr(static_cast<char*>(block) + 25); } // правый ребёнок в дереве. смещение +25 = 1 (block_data) + 8 (prev) + 8 (next) + 8 (left).
    void set_blk_right(void* block, void* v) noexcept      { write_ptr(static_cast<char*>(block) + 25, v); } // записываем правого ребёнка.
    void* blk_parent_tree(void* block) noexcept{ return read_ptr(static_cast<char*>(block) + 33); } // родитель в дереве (parent_tree). смещение +33 = 1 + 8 + 8 + 8 + 8. нужен для вращений и балансировки.
    void set_blk_parent_tree(void* block, void* v) noexcept{ write_ptr(static_cast<char*>(block) + 33, v); } // записываем родителя в дереве.

    // ---------- размер блока = расстояние до следующего физического блока ----------
    size_t blk_size(void* trusted, void* block) noexcept { // вычисляет размер блока не из поля в заголовке, а как разность адресов. это ключевое отличие от предыдущих аллокаторов (boundary_tags, sorted_list, buddies), где размер хранился явно.
        void* n = blk_next(block); // читаем указатель на следующий физический блок.
        if (n == nullptr) { // если следующего нет (это последний блок)...
            void* reg = region_start(trusted); // ...начало региона.
            size_t space = get_space_size(trusted); // ...общий размер области.
            return static_cast<char*>(reg) + space - static_cast<char*>(block); // размер = (начало + общий размер) - текущий блок. так мы получаем сколько байтов от block до конца региона.
        }
        return static_cast<char*>(n) - static_cast<char*>(block); // размер = адрес следующего - адрес текущего. благодаря физическим связям prev/next размер вычисляется динамически без хранения в заголовке.
    }

    // ---------- сравнение блоков в дереве: сначала по размеру, потом по адресу ----------
    bool less_by_size(void* trusted, void* a, void* b) noexcept { // компаратор (функция сравнения) для упорядочивания узлов в красно-чёрном дереве. дерево отсортировано по размеру блока: меньшие блоки слева, большие справа. если размеры равны, сравниваем адреса (чтобы блоки с одинаковым размером не считались равными и не терялись).
        size_t sa = blk_size(trusted, a); // размер блока a (вычисляется через blk_size).
        size_t sb = blk_size(trusted, b); // размер блока b.
        if (sa != sb) return sa < sb; // если размеры разные — меньше тот, чей размер меньше.
        return a < b; // если размеры равны — меньше тот, чей адрес меньше. это гарантирует строгий порядок (strict weak ordering), необходимый для корректной работы двоичного дерева поиска.
    }

    // ---------- базовые операции красно-чёрного дерева ----------
    void rotate_left(void* trusted, void* x) noexcept { // левое вращение (left rotation) вокруг узла x. это базовая операция балансировки дерева. представьте, что x — «родитель», y — его правый ребёнок. при левом вращении y поднимается вверх, x опускается влево, а левое поддерево y становится правым поддеревом x. свойство двоичного дерева поиска сохраняется: все ключи в левом поддереве < x < все ключи в правом.
        void* y = blk_right(x); // y — правый ребёнок x.
        set_blk_right(x, blk_left(y)); // левое поддерево y становится правым поддеревом x.
        if (blk_left(y) != nullptr) set_blk_parent_tree(blk_left(y), x); // обновляем родителя у бывшего левого ребёнка y (если он есть).
        set_blk_parent_tree(y, blk_parent_tree(x)); // y теперь имеет того же родителя, что и x.
        void* xp = blk_parent_tree(x); // xp — бывший родитель x.
        if (xp == nullptr) {
            get_root(trusted) = y; // если x был корнем, теперь корнем становится y.
        } else if (x == blk_left(xp)) {
            set_blk_left(xp, y); // если x был левым ребёнком своего родителя, теперь левым ребёнком становится y.
        } else {
            set_blk_right(xp, y); // иначе y становится правым ребёнком.
        }
        set_blk_left(y, x); // x становится левым ребёнком y.
        set_blk_parent_tree(x, y); // родитель x — теперь y.
    }

    void rotate_right(void* trusted, void* x) noexcept { // правое вращение (right rotation) — зеркальная операция left rotation. x — родитель, y — его левый ребёнок. y поднимается вверх, x опускается вправо, а правое поддерево y становится левым поддеревом x.
        void* y = blk_left(x); // y — левый ребёнок x.
        set_blk_left(x, blk_right(y)); // правое поддерево y становится левым поддеревом x.
        if (blk_right(y) != nullptr) set_blk_parent_tree(blk_right(y), x); // обновляем родителя у бывшего правого ребёнка y.
        set_blk_parent_tree(y, blk_parent_tree(x)); // y получает родителя x.
        void* xp = blk_parent_tree(x); // бывший родитель x.
        if (xp == nullptr) {
            get_root(trusted) = y; // y становится корнем.
        } else if (x == blk_right(xp)) {
            set_blk_right(xp, y); // y заменяет x как правый ребёнок.
        } else {
            set_blk_left(xp, y); // y заменяет x как левый ребёнок.
        }
        set_blk_right(y, x); // x становится правым ребёнком y.
        set_blk_parent_tree(x, y); // родитель x — теперь y.
    }

    void transplant(void* trusted, void* u, void* v) noexcept { // операция transplant (пересадка) заменяет поддерево с корнем u на поддерево с корнем v. используется при удалении узла: вместо удаляемого узла u ставится другой узел v (обычно его преемник). если u был корнем, v становится новым корнем. иначе v подвешивается на место u к его родителю.
        void* up = blk_parent_tree(u); // родитель u.
        if (up == nullptr) {
            get_root(trusted) = v; // u был корнем — v становится корнем.
        } else if (u == blk_left(up)) {
            set_blk_left(up, v); // u был левым ребёнком — теперь левым становится v.
        } else {
            set_blk_right(up, v); // u был правым ребёнком — теперь правым становится v.
        }
        if (v != nullptr) set_blk_parent_tree(v, up); // обновляем родителя v (если v не nullptr).
    }

    void* tree_minimum(void* x) noexcept { // поиск узла с минимальным ключом в поддереве с корнем x. в двоичном дереве поиска минимум всегда находится в самом левом узле: идём влево, пока есть левые дети.
        while (blk_left(x) != nullptr) x = blk_left(x); // цикл: пока у текущего узла есть левый ребёнок, двигаемся влево.
        return x; // возвращаем самый левый узел.
    }

    void insert_fixup(void* trusted, void* z) noexcept { // балансировка дерева после вставки нового узла z. при вставке новый узел всегда красный. если его родитель тоже красный — нарушается свойство красно-чёрного дерева (красный узел не может иметь красного ребёнка). insert_fixup устраняет это через перекрашивание и вращения. алгоритм стандартный из учебников (cormen и др.).
        while (blk_parent_tree(z) != nullptr && blk_color(blk_parent_tree(z)) == allocator_red_black_tree::block_color::RED) { // пока родитель существует и красный...
            void* p = blk_parent_tree(z); // p — родитель z (красный).
            void* g = blk_parent_tree(p); // g — дедушка z (родитель p).
            if (p == blk_left(g)) { // случай a: родитель p — левый ребёнок дедушки g.
                void* u = blk_right(g); // u — дядя z (правый ребёнок g, брат p).
                if (u != nullptr && blk_color(u) == allocator_red_black_tree::block_color::RED) { // случай 1: дядя u существует и тоже красный.
                    set_bd(p, false, allocator_red_black_tree::block_color::BLACK); // перекрашиваем p в чёрный.
                    set_bd(u, false, allocator_red_black_tree::block_color::BLACK); // перекрашиваем u в чёрный.
                    set_bd(g, false, allocator_red_black_tree::block_color::RED); // перекрашиваем дедушку g в красный.
                    z = g; // теперь проблема может быть у g (если его родитель красный), поэтому поднимаемся вверх.
                } else { // случай 2/3: дядя u чёрный или nullptr.
                    if (z == blk_right(p)) { // случай 2: z — правый ребёнок p (образует «ломаную» линию g-p-z).
                        z = p; // поднимаем z на уровень p.
                        rotate_left(trusted, z); // левое вращение вокруг z (бывшего p), преобразуем в случай 3.
                        p = blk_parent_tree(z); // обновляем p после вращения.
                        g = blk_parent_tree(p); // обновляем g.
                    }
                    // случай 3: z — левый ребёнок p («прямая» линия g-p-z).
                    set_bd(p, false, allocator_red_black_tree::block_color::BLACK); // перекрашиваем p в чёрный.
                    set_bd(g, false, allocator_red_black_tree::block_color::RED); // перекрашиваем g в красный.
                    rotate_right(trusted, g); // правое вращение вокруг g.
                }
            } else { // случай b: родитель p — правый ребёнок дедушки g (зеркально случаю a).
                void* u = blk_left(g); // u — дядя z (левый ребёнок g).
                if (u != nullptr && blk_color(u) == allocator_red_black_tree::block_color::RED) { // случай 1: дядя красный.
                    set_bd(p, false, allocator_red_black_tree::block_color::BLACK);
                    set_bd(u, false, allocator_red_black_tree::block_color::BLACK);
                    set_bd(g, false, allocator_red_black_tree::block_color::RED);
                    z = g;
                } else { // случай 2/3: дядя чёрный.
                    if (z == blk_left(p)) { // случай 2: z — левый ребёнок p (ломаная линия).
                        z = p;
                        rotate_right(trusted, z); // правое вращение.
                        p = blk_parent_tree(z);
                        g = blk_parent_tree(p);
                    }
                    // случай 3: z — правый ребёнок p (прямая линия).
                    set_bd(p, false, allocator_red_black_tree::block_color::BLACK);
                    set_bd(g, false, allocator_red_black_tree::block_color::RED);
                    rotate_left(trusted, g); // левое вращение вокруг g.
                }
            }
        }
        void* r = get_root(trusted); // корень дерева.
        if (r != nullptr) set_bd(r, false, allocator_red_black_tree::block_color::BLACK); // свойство: корень всегда чёрный.
    }

    void rbt_insert(void* trusted, void* z) noexcept { // вставка узла z в красно-чёрное дерево. сначала выполняется обычная вставка в двоичное дерево поиска (bst insert), затем вызывается insert_fixup для восстановления свойств.
        void* y = nullptr; // y — будущий родитель z (последний не-null узел на пути поиска).
        void* x = get_root(trusted); // x — текущий узел при обходе дерева от корня.
        while (x != nullptr) { // спускаемся от корня к листу, пока не найдём место для z.
            y = x; // запоминаем текущий узел как потенциального родителя.
            if (less_by_size(trusted, z, x)) { // если z < x (по размеру, затем по адресу)...
                x = blk_left(x); // ...идём в левое поддерево.
            } else {
                x = blk_right(x); // ...иначе в правое поддерево.
            }
        }
        set_blk_parent_tree(z, y); // y становится родителем z.
        if (y == nullptr) {
            get_root(trusted) = z; // дерево было пустым — z становится корнем.
        } else if (less_by_size(trusted, z, y)) {
            set_blk_left(y, z); // z меньше y — вставляем слева.
        } else {
            set_blk_right(y, z); // z больше y — вставляем справа.
        }
        set_blk_left(z, nullptr); // у нового узла нет детей.
        set_blk_right(z, nullptr);
        set_bd(z, false, allocator_red_black_tree::block_color::RED); // новый узел всегда красный (свойство вставки в кч-дерево).
        insert_fixup(trusted, z); // восстанавливаем свойства дерева после вставки.
    }

    void remove_fixup(void* trusted, void* x, void* x_parent) noexcept { // балансировка дерева после удаления. x — узел, который занял место удалённого (может быть nullptr). x_parent — родитель x. когда удаляется чёрный узел, нарушается свойство «все пути содержат одинаковое число чёрных узлов» (black-height). remove_fixup устраняет это, рассматривая брата w узла x. алгоритм стандартный из учебников.
        while (x != get_root(trusted) && (x == nullptr || blk_color(x) == allocator_red_black_tree::block_color::BLACK)) { // пока x не корень и x чёрный (nullptr считается чёрным)...
            if (x == blk_left(x_parent)) { // случай a: x — левый ребёнок своего родителя.
                void* w = blk_right(x_parent); // w — брат x (правый ребёнок родителя).
                if (blk_color(w) == allocator_red_black_tree::block_color::RED) { // случай 1: брат w красный.
                    set_bd(w, false, allocator_red_black_tree::block_color::BLACK); // перекрашиваем w в чёрный.
                    set_bd(x_parent, false, allocator_red_black_tree::block_color::RED); // родителя в красный.
                    rotate_left(trusted, x_parent); // левое вращение вокруг родителя.
                    w = blk_right(x_parent); // обновляем w после вращения.
                }
                if ((blk_left(w) == nullptr || blk_color(blk_left(w)) == allocator_red_black_tree::block_color::BLACK) && // случай 2: оба ребёнка w чёрные (или nullptr).
                    (blk_right(w) == nullptr || blk_color(blk_right(w)) == allocator_red_black_tree::block_color::BLACK)) {
                    set_bd(w, false, allocator_red_black_tree::block_color::RED); // перекрашиваем w в красный.
                    x = x_parent; // поднимаем проблему вверх к родителю.
                    x_parent = blk_parent_tree(x); // обновляем родителя.
                } else {
                    if (blk_right(w) == nullptr || blk_color(blk_right(w)) == allocator_red_black_tree::block_color::BLACK) { // случай 3: правый ребёнок w чёрный, левый красный.
                        if (blk_left(w) != nullptr) set_bd(blk_left(w), false, allocator_red_black_tree::block_color::BLACK); // левого ребёнка w в чёрный.
                        set_bd(w, false, allocator_red_black_tree::block_color::RED); // w в красный.
                        rotate_right(trusted, w); // правое вращение вокруг w.
                        w = blk_right(x_parent); // обновляем w.
                    }
                    // случай 4: правый ребёнок w красный.
                    set_bd(w, false, blk_color(x_parent)); // w получает цвет родителя.
                    set_bd(x_parent, false, allocator_red_black_tree::block_color::BLACK); // родитель становится чёрным.
                    if (blk_right(w) != nullptr) set_bd(blk_right(w), false, allocator_red_black_tree::block_color::BLACK); // правый ребёнок w чёрный.
                    rotate_left(trusted, x_parent); // левое вращение вокруг родителя.
                    x = get_root(trusted); // проблема решена, выходим из цикла (x = корень).
                }
            } else { // случай b: x — правый ребёнок (зеркально случаю a).
                void* w = blk_left(x_parent); // брат w — левый ребёнок родителя.
                if (blk_color(w) == allocator_red_black_tree::block_color::RED) { // случай 1: брат красный.
                    set_bd(w, false, allocator_red_black_tree::block_color::BLACK);
                    set_bd(x_parent, false, allocator_red_black_tree::block_color::RED);
                    rotate_right(trusted, x_parent);
                    w = blk_left(x_parent);
                }
                if ((blk_right(w) == nullptr || blk_color(blk_right(w)) == allocator_red_black_tree::block_color::BLACK) && // случай 2: оба ребёнка w чёрные.
                    (blk_left(w) == nullptr || blk_color(blk_left(w)) == allocator_red_black_tree::block_color::BLACK)) {
                    set_bd(w, false, allocator_red_black_tree::block_color::RED);
                    x = x_parent;
                    x_parent = blk_parent_tree(x);
                } else {
                    if (blk_left(w) == nullptr || blk_color(blk_left(w)) == allocator_red_black_tree::block_color::BLACK) { // случай 3: левый ребёнок w чёрный, правый красный.
                        if (blk_right(w) != nullptr) set_bd(blk_right(w), false, allocator_red_black_tree::block_color::BLACK);
                        set_bd(w, false, allocator_red_black_tree::block_color::RED);
                        rotate_left(trusted, w);
                        w = blk_left(x_parent);
                    }
                    // случай 4: левый ребёнок w красный.
                    set_bd(w, false, blk_color(x_parent));
                    set_bd(x_parent, false, allocator_red_black_tree::block_color::BLACK);
                    if (blk_left(w) != nullptr) set_bd(blk_left(w), false, allocator_red_black_tree::block_color::BLACK);
                    rotate_right(trusted, x_parent);
                    x = get_root(trusted);
                }
            }
        }
        if (x != nullptr) set_bd(x, false, allocator_red_black_tree::block_color::BLACK); // корень (или узел, на котором остановились) делаем чёрным.
    }

    void rbt_remove(void* trusted, void* z) noexcept { // удаление узла z из красно-чёрного дерева. сначала выполняется стандартное удаление из bst (находим преемника, если два ребёнка), затем вызывается remove_fixup, если удалён чёрный узел.
        void* y = z; // y — узел, который фактически будет удалён (или перемещён).
        void* x = nullptr; // x — узел, который займёт место y.
        void* x_parent = nullptr; // родитель x (нужен для fixup, т.к. x может быть nullptr).
        auto y_orig_color = blk_color(y); // запоминаем исходный цвет y. если y был чёрным — нужна балансировка.
        if (blk_left(z) == nullptr) { // случай 1: у z нет левого ребёнка.
            x = blk_right(z); // x — правый ребёнок z (может быть nullptr).
            x_parent = blk_parent_tree(z); // родителем x становится родитель z.
            transplant(trusted, z, blk_right(z)); // заменяем z его правым ребёнком.
        } else if (blk_right(z) == nullptr) { // случай 2: у z нет правого ребёнка, но есть левый.
            x = blk_left(z); // x — левый ребёнок.
            x_parent = blk_parent_tree(z);
            transplant(trusted, z, blk_left(z)); // заменяем z левым ребёнком.
        } else { // случай 3: у z два ребёнка.
            y = tree_minimum(blk_right(z)); // y — минимальный элемент в правом поддереве z (преемник z).
            y_orig_color = blk_color(y); // запоминаем цвет преемника.
            x = blk_right(y); // x — правый ребёнок преемника y.
            if (blk_parent_tree(y) == z) { // если y — прямой ребёнок z...
                x_parent = y; // ...родителем x будет y.
                if (x != nullptr) set_blk_parent_tree(x, y); // обновляем родителя x.
            } else { // если y не прямой ребёнок z...
                x_parent = blk_parent_tree(y); // ...запоминаем родителя y.
                transplant(trusted, y, blk_right(y)); // вырезаем y из его текущего места.
                set_blk_right(y, blk_right(z)); // y получает правое поддерево z.
                if (blk_right(y) != nullptr) set_blk_parent_tree(blk_right(y), y); // обновляем родителя.
            }
            transplant(trusted, z, y); // заменяем z на y.
            set_blk_left(y, blk_left(z)); // y получает левое поддерево z.
            if (blk_left(y) != nullptr) set_blk_parent_tree(blk_left(y), y); // обновляем родителя.
            set_bd(y, false, blk_color(z)); // y получает цвет z (сохраняем количество чёрных узлов на путях).
        }
        if (y_orig_color == allocator_red_black_tree::block_color::BLACK) { // если удалён чёрный узел...
            remove_fixup(trusted, x, x_parent); // ...восстанавливаем свойства дерева.
        }
    }

    void* tree_search(void* trusted, void* root, size_t need) noexcept { // поиск в дереве блока с размером ≥ need (lower_bound). благодаря свойствам двоичного дерева поиска этот поиск выполняется за o(log n), а не за o(n) как линейный обход.
        void* best = nullptr; // лучший найденный блок (минимальный, но ≥ need).
        void* cur = root; // текущий узел при обходе.
        while (cur != nullptr) { // спускаемся по дереву.
            size_t sz = blk_size(trusted, cur); // размер текущего блока.
            if (sz >= need) { // если текущий блок подходит...
                best = cur; // ...запоминаем его как кандидата.
                cur = blk_left(cur); // и идём влево, чтобы найти ещё меньший подходящий блок (ближайший к need).
            } else {
                cur = blk_right(cur); // если текущий блок слишком мал — идём вправо (там блоки больше).
            }
        }
        return best; // возвращаем лучший найденный или nullptr.
    }

    void* tree_find_max(void* trusted, void* root) noexcept { // поиск узла с максимальным ключом в дереве. в двоичном дереве поиска максимум находится в самом правом узле.
        if (root == nullptr) return nullptr; // дерево пусто.
        void* cur = root; // начинаем с корня.
        while (blk_right(cur) != nullptr) cur = blk_right(cur); // идём вправо, пока есть правые дети.
        return cur; // возвращаем самый правый узел.
    }

} // namespace

// ==================== правило пяти ====================

allocator_red_black_tree::~allocator_red_black_tree() {
    if (_trusted_memory == nullptr) return; // объект перемещён (объяснялось ранее)
    std::pmr::memory_resource* parent = get_parent(_trusted_memory);
    size_t total = get_space_size(_trusted_memory) + allocator_metadata_size_local; // общий размер = пользовательская область + заголовок.
    get_mtx(_trusted_memory).~mutex(); // явный деструктор мьютекса (объяснялось ранее)
    if (parent != nullptr) {
        parent->deallocate(_trusted_memory, total, alignof(std::max_align_t)); // (объяснялось ранее)
    } else {
        ::operator delete(_trusted_memory, total, std::align_val_t{alignof(std::max_align_t)}); // (объяснялось ранее)
    }
}

allocator_red_black_tree::allocator_red_black_tree(
    allocator_red_black_tree &&other) noexcept
    : _trusted_memory(other._trusted_memory) // (объяснялось ранее)
{
    other._trusted_memory = nullptr;
}

allocator_red_black_tree &allocator_red_black_tree::operator=(
    allocator_red_black_tree &&other) noexcept
{
    if (this != &other) { // (объяснялось ранее)
        if (_trusted_memory != nullptr) {
            std::pmr::memory_resource* parent = get_parent(_trusted_memory);
            size_t total = get_space_size(_trusted_memory) + allocator_metadata_size_local;
            get_mtx(_trusted_memory).~mutex();
            if (parent != nullptr) {
                parent->deallocate(_trusted_memory, total, alignof(std::max_align_t));
            } else {
                ::operator delete(_trusted_memory, total, std::align_val_t{alignof(std::max_align_t)});
            }
        }
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    return *this;
}

allocator_red_black_tree::allocator_red_black_tree(
    size_t space_size,
    std::pmr::memory_resource *parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
    : _trusted_memory(nullptr) // (объяснялось ранее)
{
    if (parent_allocator == nullptr) {
        parent_allocator = std::pmr::get_default_resource(); // (объяснялось ранее)
    }
    _trusted_memory = parent_allocator->allocate(space_size + allocator_metadata_size_local, alignof(std::max_align_t)); // (объяснялось ранее)

    new (static_cast<char*>(_trusted_memory) + mtx_offset) std::mutex(); // placement new (объяснялось ранее)
    get_parent(_trusted_memory) = parent_allocator; // (объяснялось ранее)
    get_space_size(_trusted_memory) = space_size; // (объяснялось ранее)
    get_root(_trusted_memory) = nullptr; // дерево свободных блоков пока пусто.
    get_mode(_trusted_memory) = allocate_fit_mode; // (объяснялось ранее)

    void* first = region_start(_trusted_memory); // первый блок.
    if (space_size >= free_block_metadata_size) { // если места достаточно хотя бы для одного свободного блока (с учётом его метаданных)...
        set_bd(first, false, block_color::BLACK); // помечаем первый блок как свободный и чёрный (цвет корня дерева).
        set_blk_prev(first, nullptr); // физические связи: нет предыдущего.
        set_blk_next(first, nullptr); // нет следующего.
        set_blk_left(first, nullptr); // в дереве: нет левого ребёнка.
        set_blk_right(first, nullptr); // нет правого ребёнка.
        set_blk_parent_tree(first, nullptr); // нет родителя в дереве (это корень).
        rbt_insert(_trusted_memory, first); // вставляем первый (и пока единственный) свободный блок в красно-чёрное дерево.
    }
}

allocator_red_black_tree::allocator_red_black_tree(const allocator_red_black_tree &other) {
    throw not_implemented("allocator_red_black_tree::allocator_red_black_tree(const allocator_red_black_tree &)", "copying is not supported"); // (объяснялось ранее)
}

allocator_red_black_tree &allocator_red_black_tree::operator=(const allocator_red_black_tree &other) {
    throw not_implemented("allocator_red_black_tree::operator=(const allocator_red_black_tree &)", "copying is not supported"); // (объяснялось ранее)
}

bool allocator_red_black_tree::do_is_equal(const std::pmr::memory_resource &other) const noexcept {
    return this == &other; // (объяснялось ранее)
}

// ==================== allocate / deallocate ====================

[[nodiscard]] void *allocator_red_black_tree::do_allocate_sm(size_t size) {
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory)); // (объяснялось ранее)

    if (size == 0) size = 1; // нулевой запрос трактуем как 1 байт (объяснялось в 3-й задаче)
    size_t needed = size; // в этой реализации размер блока = запрошенный размер + метаданные уже учтены через occupied_block_metadata_size при возврате. но здесь needed = size напрямую, т.к. сравнение идёт с blk_size, который включает метаданные.

    void* first = region_start(_trusted_memory); // начало региона.
    void* end = static_cast<char*>(first) + get_space_size(_trusted_memory); // конец региона.
    if (first >= end) throw std::bad_alloc(); // защита: если регион нулевой или отрицательный.

    void* best = nullptr; // выбранный блок.
    fit_mode mode = get_mode(_trusted_memory); // текущий режим.

    if (mode == fit_mode::first_fit) { // линейный поиск по физическому списку (как в sorted_list/boundary_tags).
        void* cur = first; // начинаем с первого блока.
        while (cur != nullptr) { // обход через физические связи next.
            if (!blk_occupied(cur) && blk_size(_trusted_memory, cur) >= needed) { // если блок свободен и подходит по размеру...
                best = cur; // ...выбираем его.
                break; // first_fit — останавливаемся на первом подходящем.
            }
            cur = blk_next(cur); // переходим к следующему физическому блоку.
        }
    } else if (mode == fit_mode::the_best_fit) { // best_fit через красно-чёрное дерево — o(log n)!
        best = tree_search(_trusted_memory, get_root(_trusted_memory), needed); // tree_search ищет lower_bound: минимальный блок, размер которого ≥ needed. это гораздо быстрее линейного поиска (o(log n) против o(n)).
    } else { // the_worst_fit — тоже через дерево.
        void* mx = tree_find_max(_trusted_memory, get_root(_trusted_memory)); // находим максимальный блок в дереве.
        if (mx != nullptr && blk_size(_trusted_memory, mx) >= needed) { // если он существует и подходит...
            best = mx; // ...выбираем его.
        }
    }

    if (best == nullptr) throw std::bad_alloc(); // не нашли подходящий блок.

    size_t best_sz = blk_size(_trusted_memory, best); // размер выбранного блока.
    size_t remaining = best_sz - needed; // остаток после выделения needed байт.

    rbt_remove(_trusted_memory, best); // удаляем best из дерева свободных блоков (он становится занятым).

    if (remaining >= free_block_metadata_size) { // если остаток достаточно велик для нового свободного блока...
        void* new_free = static_cast<char*>(best) + needed; // адрес оставшегося фрагмента.
        set_bd(new_free, false, block_color::BLACK); // помечаем остаток как свободный (цвет пока чёрный, rbt_insert установит правильный).
        set_blk_prev(new_free, best); // физическая связь: prev указывает на best.
        set_blk_next(new_free, blk_next(best)); // next наследует от best.
        if (blk_next(best) != nullptr) {
            set_blk_prev(blk_next(best), new_free); // обновляем prev у бывшего следующего за best.
        }
        set_blk_next(best, new_free); // best теперь указывает на new_free как next.

        set_bd(best, true, block_color::BLACK); // best становится занятым.
        set_blk_parent_alloc(best, get_parent(_trusted_memory)); // записываем указатель родителя для проверки принадлежности.

        rbt_insert(_trusted_memory, new_free); // остаток вставляем обратно в дерево свободных блоков.
    } else { // остаток слишком мал — отдаём весь блок.
        set_bd(best, true, block_color::BLACK); // best занят.
        set_blk_parent_alloc(best, get_parent(_trusted_memory)); // записываем родителя.
    }

    return static_cast<char*>(best) + occupied_block_metadata_size; // возвращаем payload (пропускаем метаданные занятого блока).
}

void allocator_red_black_tree::do_deallocate_sm(void *at) {
    if (at == nullptr) return; // освобождение nullptr — no-op (объяснялось в 3-й задаче)

    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory)); // (объяснялось ранее)

    void* reg = region_start(_trusted_memory); // начало региона.
    void* end = static_cast<char*>(reg) + get_space_size(_trusted_memory); // конец региона.
    void* block = static_cast<char*>(at) - occupied_block_metadata_size; // вычисляем адрес заголовка блока.

    // валидация: линейный проход по всем блокам (объяснялся во 2-й задаче)
    void* cur = reg;
    while (cur < end) {
        if (cur == block) break;
        size_t sz = blk_size(_trusted_memory, cur);
        if (sz == 0 || cur > block) {
            throw std::invalid_argument("invalid pointer to deallocate"); // (объяснялось ранее)
        }
        cur = static_cast<char*>(cur) + sz;
    }
    if (cur != block || cur >= end) {
        throw std::invalid_argument("invalid pointer to deallocate"); // (объяснялось ранее)
    }
    if (!blk_occupied(block)) {
        throw std::invalid_argument("double free or corruption"); // (объяснялось ранее)
    }

    void* prev = blk_prev(block); // физический предыдущий блок.
    void* next = blk_next(block); // физический следующий блок.

    if (prev != nullptr && !blk_occupied(prev)) { // если слева свободный блок...
        rbt_remove(_trusted_memory, prev); // ...удаляем его из дерева (скоро объединим).
        block = prev; // новый объединённый блок начинается с prev.
    }
    if (next != nullptr && !blk_occupied(next)) { // если справа свободный блок...
        rbt_remove(_trusted_memory, next); // ...удаляем его из дерева.
        next = blk_next(next); // обновляем next (перепрыгиваем через объединённый).
    }

    set_blk_next(block, next); // обновляем физическую связь next.
    if (next != nullptr) {
        set_blk_prev(next, block); // обновляем prev у следующего.
    }

    set_bd(block, false, block_color::BLACK); // помечаем объединённый блок свободным (цвет чёрный — rbt_insert исправит при необходимости).
    set_blk_left(block, nullptr); // очищаем указатели дерева перед вставкой.
    set_blk_right(block, nullptr);
    set_blk_parent_tree(block, nullptr);

    rbt_insert(_trusted_memory, block); // вставляем объединённый свободный блок в красно-чёрное дерево.
}

void allocator_red_black_tree::set_fit_mode(allocator_with_fit_mode::fit_mode mode) {
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory)); // (объяснялось ранее)
    get_mode(_trusted_memory) = mode;
}

// ==================== отладочная информация ====================

std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info() const {
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory)); // (объяснялось ранее)
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info_inner() const {
    std::vector<block_info> result; // (объяснялось ранее)
    void* cur = region_start(_trusted_memory); // начало региона.
    void* end = static_cast<char*>(cur) + get_space_size(_trusted_memory); // конец региона.
    while (cur < end) {
        size_t sz = blk_size(_trusted_memory, cur); // размер вычисляется через разницу адресов (объяснялось выше).
        bool occ = blk_occupied(cur); // читаем флаг занятости из bit fields.
        result.push_back({sz, occ}); // (объяснялось ранее)
        cur = static_cast<char*>(cur) + sz; // переходим к следующему блоку.
    }
    return result;
}

// ==================== итераторы ====================

allocator_red_black_tree::rb_iterator allocator_red_black_tree::begin() const noexcept {
    return rb_iterator(_trusted_memory); // начинаем с первого блока.
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::end() const noexcept {
    return rb_iterator(); // пустой итератор = конец.
}

bool allocator_red_black_tree::rb_iterator::operator==(const rb_iterator &other) const noexcept {
    return _block_ptr == other._block_ptr; // (объяснялось ранее)
}

bool allocator_red_black_tree::rb_iterator::operator!=(const rb_iterator &other) const noexcept {
    return !(*this == other); // (объяснялось ранее)
}

allocator_red_black_tree::rb_iterator &allocator_red_black_tree::rb_iterator::operator++() & noexcept {
    if (_block_ptr == nullptr || _trusted == nullptr) return *this; // защита.
    _block_ptr = blk_next(_block_ptr); // переход к следующему физическому блоку через связь next (как в sorted_list).
    return *this;
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::rb_iterator::operator++(int) {
    rb_iterator tmp = *this; // (объяснялось ранее)
    ++(*this);
    return tmp;
}

size_t allocator_red_black_tree::rb_iterator::size() const noexcept {
    if (_block_ptr == nullptr || _trusted == nullptr) return 0;
    return blk_size(_trusted, _block_ptr); // размер вычисляется динамически.
}

void *allocator_red_black_tree::rb_iterator::operator*() const noexcept {
    if (_block_ptr == nullptr) return nullptr;
    return blk_occupied(_block_ptr) // тернарный оператор: в зависимости от занятости...
        ? static_cast<char*>(_block_ptr) + allocator_red_black_tree::occupied_block_metadata_size // ...для занятого пропускаем occupied_block_metadata_size байт.
        : static_cast<char*>(_block_ptr) + allocator_red_black_tree::free_block_metadata_size;   // ...для свободного пропускаем free_block_metadata_size байт.
}

allocator_red_black_tree::rb_iterator::rb_iterator() : _block_ptr(nullptr), _trusted(nullptr) {} // пустой итератор.

allocator_red_black_tree::rb_iterator::rb_iterator(void* trusted) : _block_ptr(nullptr), _trusted(trusted) { // инициализация trusted.
    if (trusted == nullptr) return;
    void* first = region_start(trusted); // первый блок.
    void* end = static_cast<char*>(first) + get_space_size(trusted); // конец региона.
    if (first < end) {
        _block_ptr = first; // устанавливаем на первый блок.
    } else {
        _trusted = nullptr; // регион пуст — делаем end-итератор.
    }
}

bool allocator_red_black_tree::rb_iterator::occupied() const noexcept {
    if (_block_ptr == nullptr) return false;
    return blk_occupied(_block_ptr); // читаем флаг из bit fields.
}
