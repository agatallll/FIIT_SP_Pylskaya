#include <not_implemented.h>
#include "../include/allocator_red_black_tree.h"
#include <mutex>                           // примитив синхронизации
#include <memory_resource>                 // родительский аллокатор
#include <stdexcept>                       // исключения при ошибках
#include <cstddef>                         // size_t и ptrdiff_t
#include <cstring>                         // memcpy для невыровненной памяти
#include <vector>                          // возврат информации о блоках

namespace { // анонимное пространство имён — вспомогательные функции

    constexpr size_t allocator_metadata_size_local =
        sizeof(std::mutex)                         // 80 байт
        + sizeof(std::pmr::memory_resource*)       // 8 байт
        + sizeof(size_t)                           // 8 байт
        + sizeof(void*)                            // 8 байт
        + sizeof(allocator_with_fit_mode::fit_mode); // 4 байт
    static_assert(allocator_metadata_size_local == 108); // проверка размера

    constexpr size_t mtx_offset        = 0;                                    // мьютекс с начала
    constexpr size_t parent_offset     = sizeof(std::mutex);                  // 80
    constexpr size_t space_size_offset = parent_offset + sizeof(void*);       // 88
    constexpr size_t root_offset       = space_size_offset + sizeof(size_t);  // 96
    constexpr size_t mode_offset       = root_offset + sizeof(void*);         // 104

    std::mutex& get_mtx(void* trusted) noexcept {
        return *reinterpret_cast<std::mutex*>(static_cast<char*>(trusted) + mtx_offset);
    }
    std::pmr::memory_resource*& get_parent(void* trusted) noexcept {
        return *reinterpret_cast<std::pmr::memory_resource**>(static_cast<char*>(trusted) + parent_offset);
    }
    size_t& get_space_size(void* trusted) noexcept {
        return *reinterpret_cast<size_t*>(static_cast<char*>(trusted) + space_size_offset);
    }
    void*& get_root(void* trusted) noexcept {
        return *reinterpret_cast<void**>(static_cast<char*>(trusted) + root_offset);
    }
    allocator_with_fit_mode::fit_mode& get_mode(void* trusted) noexcept {
        return *reinterpret_cast<allocator_with_fit_mode::fit_mode*>(static_cast<char*>(trusted) + mode_offset);
    }
    void* region_start(void* trusted) noexcept {
        return static_cast<char*>(trusted) + allocator_metadata_size_local; // начало области блоков
    }

    // ---------- безопасное чтение/запись через memcpy (msvc debug) ----------
    void* read_ptr(void* addr) noexcept {
        void* v;
        std::memcpy(&v, addr, sizeof(v));
        return v;
    }
    void write_ptr(void* addr, void* v) noexcept {
        std::memcpy(addr, &v, sizeof(v));
    }

    allocator_red_black_tree::block_data read_bd(void* block) noexcept {
        allocator_red_black_tree::block_data bd;
        std::memcpy(&bd, block, sizeof(bd));
        return bd;
    }
    void write_bd(void* block, allocator_red_black_tree::block_data bd) noexcept {
        std::memcpy(block, &bd, sizeof(bd));
    }

    bool blk_occupied(void* block) noexcept {
        return read_bd(block).occupied;
    }
    allocator_red_black_tree::block_color blk_color(void* block) noexcept {
        if (block == nullptr) return allocator_red_black_tree::block_color::BLACK; // nil — чёрный
        return read_bd(block).color;
    }
    void set_bd(void* block, bool occ, allocator_red_black_tree::block_color col) noexcept {
        allocator_red_black_tree::block_data bd{occ, col};
        write_bd(block, bd);
    }

    // ---------- физические связи prev/next (все блоки) ----------
    void* blk_prev(void* block) noexcept { return read_ptr(static_cast<char*>(block) + 1); }
    void set_blk_prev(void* block, void* v) noexcept { write_ptr(static_cast<char*>(block) + 1, v); }
    void* blk_next(void* block) noexcept { return read_ptr(static_cast<char*>(block) + 9); }
    void set_blk_next(void* block, void* v) noexcept { write_ptr(static_cast<char*>(block) + 9, v); }

    // ---------- занятый блок: указатель на родительский аллокатор ----------
    void* blk_parent_alloc(void* block) noexcept { return read_ptr(static_cast<char*>(block) + 17); }
    void set_blk_parent_alloc(void* block, void* v) noexcept { write_ptr(static_cast<char*>(block) + 17, v); }

    // ---------- свободный блок: указатели дерева кч ----------
    void* blk_left(void* block) noexcept       { return read_ptr(static_cast<char*>(block) + 17); }
    void set_blk_left(void* block, void* v) noexcept       { write_ptr(static_cast<char*>(block) + 17, v); }
    void* blk_right(void* block) noexcept      { return read_ptr(static_cast<char*>(block) + 25); }
    void set_blk_right(void* block, void* v) noexcept      { write_ptr(static_cast<char*>(block) + 25, v); }
    void* blk_parent_tree(void* block) noexcept{ return read_ptr(static_cast<char*>(block) + 33); }
    void set_blk_parent_tree(void* block, void* v) noexcept{ write_ptr(static_cast<char*>(block) + 33, v); }

    // ---------- размер блока = расстояние до следующего физического блока ----------
    size_t blk_size(void* trusted, void* block) noexcept {
        void* n = blk_next(block);
        if (n == nullptr) { // последний блок
            void* reg = region_start(trusted);
            size_t space = get_space_size(trusted);
            return static_cast<char*>(reg) + space - static_cast<char*>(block);
        }
        return static_cast<char*>(n) - static_cast<char*>(block);
    }

    // ---------- сравнение блоков в дереве: сначала по размеру, потом по адресу ----------
    bool less_by_size(void* trusted, void* a, void* b) noexcept {
        size_t sa = blk_size(trusted, a);
        size_t sb = blk_size(trusted, b);
        if (sa != sb) return sa < sb;
        return a < b;
    }

    // ---------- базовые операции красно-чёрного дерева ----------
    void rotate_left(void* trusted, void* x) noexcept {
        void* y = blk_right(x);
        set_blk_right(x, blk_left(y));
        if (blk_left(y) != nullptr) set_blk_parent_tree(blk_left(y), x);
        set_blk_parent_tree(y, blk_parent_tree(x));
        void* xp = blk_parent_tree(x);
        if (xp == nullptr) {
            get_root(trusted) = y;
        } else if (x == blk_left(xp)) {
            set_blk_left(xp, y);
        } else {
            set_blk_right(xp, y);
        }
        set_blk_left(y, x);
        set_blk_parent_tree(x, y);
    }

    void rotate_right(void* trusted, void* x) noexcept {
        void* y = blk_left(x);
        set_blk_left(x, blk_right(y));
        if (blk_right(y) != nullptr) set_blk_parent_tree(blk_right(y), x);
        set_blk_parent_tree(y, blk_parent_tree(x));
        void* xp = blk_parent_tree(x);
        if (xp == nullptr) {
            get_root(trusted) = y;
        } else if (x == blk_right(xp)) {
            set_blk_right(xp, y);
        } else {
            set_blk_left(xp, y);
        }
        set_blk_right(y, x);
        set_blk_parent_tree(x, y);
    }

    void transplant(void* trusted, void* u, void* v) noexcept {
        void* up = blk_parent_tree(u);
        if (up == nullptr) {
            get_root(trusted) = v;
        } else if (u == blk_left(up)) {
            set_blk_left(up, v);
        } else {
            set_blk_right(up, v);
        }
        if (v != nullptr) set_blk_parent_tree(v, up);
    }

    void* tree_minimum(void* x) noexcept {
        while (blk_left(x) != nullptr) x = blk_left(x);
        return x;
    }

    void insert_fixup(void* trusted, void* z) noexcept {
        while (blk_parent_tree(z) != nullptr && blk_color(blk_parent_tree(z)) == allocator_red_black_tree::block_color::RED) {
            void* p = blk_parent_tree(z);
            void* g = blk_parent_tree(p);
            if (p == blk_left(g)) {
                void* u = blk_right(g);
                if (u != nullptr && blk_color(u) == allocator_red_black_tree::block_color::RED) {
                    set_bd(p, false, allocator_red_black_tree::block_color::BLACK);
                    set_bd(u, false, allocator_red_black_tree::block_color::BLACK);
                    set_bd(g, false, allocator_red_black_tree::block_color::RED);
                    z = g;
                } else {
                    if (z == blk_right(p)) {
                        z = p;
                        rotate_left(trusted, z);
                        p = blk_parent_tree(z);
                        g = blk_parent_tree(p);
                    }
                    set_bd(p, false, allocator_red_black_tree::block_color::BLACK);
                    set_bd(g, false, allocator_red_black_tree::block_color::RED);
                    rotate_right(trusted, g);
                }
            } else {
                void* u = blk_left(g);
                if (u != nullptr && blk_color(u) == allocator_red_black_tree::block_color::RED) {
                    set_bd(p, false, allocator_red_black_tree::block_color::BLACK);
                    set_bd(u, false, allocator_red_black_tree::block_color::BLACK);
                    set_bd(g, false, allocator_red_black_tree::block_color::RED);
                    z = g;
                } else {
                    if (z == blk_left(p)) {
                        z = p;
                        rotate_right(trusted, z);
                        p = blk_parent_tree(z);
                        g = blk_parent_tree(p);
                    }
                    set_bd(p, false, allocator_red_black_tree::block_color::BLACK);
                    set_bd(g, false, allocator_red_black_tree::block_color::RED);
                    rotate_left(trusted, g);
                }
            }
        }
        void* r = get_root(trusted);
        if (r != nullptr) set_bd(r, false, allocator_red_black_tree::block_color::BLACK);
    }

    void rbt_insert(void* trusted, void* z) noexcept {
        void* y = nullptr;
        void* x = get_root(trusted);
        while (x != nullptr) {
            y = x;
            if (less_by_size(trusted, z, x)) {
                x = blk_left(x);
            } else {
                x = blk_right(x);
            }
        }
        set_blk_parent_tree(z, y);
        if (y == nullptr) {
            get_root(trusted) = z;
        } else if (less_by_size(trusted, z, y)) {
            set_blk_left(y, z);
        } else {
            set_blk_right(y, z);
        }
        set_blk_left(z, nullptr);
        set_blk_right(z, nullptr);
        set_bd(z, false, allocator_red_black_tree::block_color::RED);
        insert_fixup(trusted, z);
    }

    void remove_fixup(void* trusted, void* x, void* x_parent) noexcept {
        while (x != get_root(trusted) && (x == nullptr || blk_color(x) == allocator_red_black_tree::block_color::BLACK)) {
            if (x == blk_left(x_parent)) {
                void* w = blk_right(x_parent);
                if (blk_color(w) == allocator_red_black_tree::block_color::RED) {
                    set_bd(w, false, allocator_red_black_tree::block_color::BLACK);
                    set_bd(x_parent, false, allocator_red_black_tree::block_color::RED);
                    rotate_left(trusted, x_parent);
                    w = blk_right(x_parent);
                }
                if ((blk_left(w) == nullptr || blk_color(blk_left(w)) == allocator_red_black_tree::block_color::BLACK) &&
                    (blk_right(w) == nullptr || blk_color(blk_right(w)) == allocator_red_black_tree::block_color::BLACK)) {
                    set_bd(w, false, allocator_red_black_tree::block_color::RED);
                    x = x_parent;
                    x_parent = blk_parent_tree(x);
                } else {
                    if (blk_right(w) == nullptr || blk_color(blk_right(w)) == allocator_red_black_tree::block_color::BLACK) {
                        if (blk_left(w) != nullptr) set_bd(blk_left(w), false, allocator_red_black_tree::block_color::BLACK);
                        set_bd(w, false, allocator_red_black_tree::block_color::RED);
                        rotate_right(trusted, w);
                        w = blk_right(x_parent);
                    }
                    set_bd(w, false, blk_color(x_parent));
                    set_bd(x_parent, false, allocator_red_black_tree::block_color::BLACK);
                    if (blk_right(w) != nullptr) set_bd(blk_right(w), false, allocator_red_black_tree::block_color::BLACK);
                    rotate_left(trusted, x_parent);
                    x = get_root(trusted);
                }
            } else {
                void* w = blk_left(x_parent);
                if (blk_color(w) == allocator_red_black_tree::block_color::RED) {
                    set_bd(w, false, allocator_red_black_tree::block_color::BLACK);
                    set_bd(x_parent, false, allocator_red_black_tree::block_color::RED);
                    rotate_right(trusted, x_parent);
                    w = blk_left(x_parent);
                }
                if ((blk_right(w) == nullptr || blk_color(blk_right(w)) == allocator_red_black_tree::block_color::BLACK) &&
                    (blk_left(w) == nullptr || blk_color(blk_left(w)) == allocator_red_black_tree::block_color::BLACK)) {
                    set_bd(w, false, allocator_red_black_tree::block_color::RED);
                    x = x_parent;
                    x_parent = blk_parent_tree(x);
                } else {
                    if (blk_left(w) == nullptr || blk_color(blk_left(w)) == allocator_red_black_tree::block_color::BLACK) {
                        if (blk_right(w) != nullptr) set_bd(blk_right(w), false, allocator_red_black_tree::block_color::BLACK);
                        set_bd(w, false, allocator_red_black_tree::block_color::RED);
                        rotate_left(trusted, w);
                        w = blk_left(x_parent);
                    }
                    set_bd(w, false, blk_color(x_parent));
                    set_bd(x_parent, false, allocator_red_black_tree::block_color::BLACK);
                    if (blk_left(w) != nullptr) set_bd(blk_left(w), false, allocator_red_black_tree::block_color::BLACK);
                    rotate_right(trusted, x_parent);
                    x = get_root(trusted);
                }
            }
        }
        if (x != nullptr) set_bd(x, false, allocator_red_black_tree::block_color::BLACK);
    }

    void rbt_remove(void* trusted, void* z) noexcept {
        void* y = z;
        void* x = nullptr;
        void* x_parent = nullptr;
        auto y_orig_color = blk_color(y);
        if (blk_left(z) == nullptr) {
            x = blk_right(z);
            x_parent = blk_parent_tree(z);
            transplant(trusted, z, blk_right(z));
        } else if (blk_right(z) == nullptr) {
            x = blk_left(z);
            x_parent = blk_parent_tree(z);
            transplant(trusted, z, blk_left(z));
        } else {
            y = tree_minimum(blk_right(z));
            y_orig_color = blk_color(y);
            x = blk_right(y);
            if (blk_parent_tree(y) == z) {
                x_parent = y;
                if (x != nullptr) set_blk_parent_tree(x, y);
            } else {
                x_parent = blk_parent_tree(y);
                transplant(trusted, y, blk_right(y));
                set_blk_right(y, blk_right(z));
                if (blk_right(y) != nullptr) set_blk_parent_tree(blk_right(y), y);
            }
            transplant(trusted, z, y);
            set_blk_left(y, blk_left(z));
            if (blk_left(y) != nullptr) set_blk_parent_tree(blk_left(y), y);
            set_bd(y, false, blk_color(z));
        }
        if (y_orig_color == allocator_red_black_tree::block_color::BLACK) {
            remove_fixup(trusted, x, x_parent);
        }
    }

    void* tree_search(void* trusted, void* root, size_t need) noexcept { // lower_bound по размеру
        void* best = nullptr;
        void* cur = root;
        while (cur != nullptr) {
            size_t sz = blk_size(trusted, cur);
            if (sz >= need) {
                best = cur;
                cur = blk_left(cur);
            } else {
                cur = blk_right(cur);
            }
        }
        return best;
    }

    void* tree_find_max(void* trusted, void* root) noexcept {
        if (root == nullptr) return nullptr;
        void* cur = root;
        while (blk_right(cur) != nullptr) cur = blk_right(cur);
        return cur;
    }

} // namespace

// ==================== правило пяти ====================

allocator_red_black_tree::~allocator_red_black_tree() {
    if (_trusted_memory == nullptr) return; // объект уже перемещён
    std::pmr::memory_resource* parent = get_parent(_trusted_memory);
    size_t total = get_space_size(_trusted_memory) + allocator_metadata_size_local;
    get_mtx(_trusted_memory).~mutex(); // явный вызов деструктора placement new
    if (parent != nullptr) {
        parent->deallocate(_trusted_memory, total, alignof(std::max_align_t));
    } else {
        ::operator delete(_trusted_memory, total, std::align_val_t{alignof(std::max_align_t)});
    }
}

allocator_red_black_tree::allocator_red_black_tree(
    allocator_red_black_tree &&other) noexcept
    : _trusted_memory(other._trusted_memory) // забираем владение
{
    other._trusted_memory = nullptr;
}

allocator_red_black_tree &allocator_red_black_tree::operator=(
    allocator_red_black_tree &&other) noexcept
{
    if (this != &other) {
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
    : _trusted_memory(nullptr)
{
    if (parent_allocator == nullptr) {
        parent_allocator = std::pmr::get_default_resource(); // дефолтный ресурс
    }
    _trusted_memory = parent_allocator->allocate(space_size + allocator_metadata_size_local, alignof(std::max_align_t));

    new (static_cast<char*>(_trusted_memory) + mtx_offset) std::mutex(); // placement new
    get_parent(_trusted_memory) = parent_allocator;
    get_space_size(_trusted_memory) = space_size;
    get_root(_trusted_memory) = nullptr;
    get_mode(_trusted_memory) = allocate_fit_mode;

    void* first = region_start(_trusted_memory);
    if (space_size >= free_block_metadata_size) { // место хотя бы под один свободный блок
        set_bd(first, false, block_color::BLACK);
        set_blk_prev(first, nullptr);
        set_blk_next(first, nullptr);
        set_blk_left(first, nullptr);
        set_blk_right(first, nullptr);
        set_blk_parent_tree(first, nullptr);
        rbt_insert(_trusted_memory, first); // вставляем в дерево
    }
}

allocator_red_black_tree::allocator_red_black_tree(const allocator_red_black_tree &other) {
    throw not_implemented("allocator_red_black_tree::allocator_red_black_tree(const allocator_red_black_tree &)", "copying is not supported");
}

allocator_red_black_tree &allocator_red_black_tree::operator=(const allocator_red_black_tree &other) {
    throw not_implemented("allocator_red_black_tree::operator=(const allocator_red_black_tree &)", "copying is not supported");
}

bool allocator_red_black_tree::do_is_equal(const std::pmr::memory_resource &other) const noexcept {
    return this == &other; // эквивалентность по указателю объекта
}

// ==================== allocate / deallocate ====================

[[nodiscard]] void *allocator_red_black_tree::do_allocate_sm(size_t size) {
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory));

    if (size == 0) size = 1; // нулевой запрос трактуем как минимальный
    size_t needed = size; // размер блока = запрошенный размер (метаданные внутри)

    void* first = region_start(_trusted_memory);
    void* end = static_cast<char*>(first) + get_space_size(_trusted_memory);
    // отладочный вывод удалён
    if (first >= end) throw std::bad_alloc(); // память слишком мала

    void* best = nullptr;
    fit_mode mode = get_mode(_trusted_memory);

    if (mode == fit_mode::first_fit) { // линейный поиск по физическому списку
        void* cur = first;
        while (cur != nullptr) {
            if (!blk_occupied(cur) && blk_size(_trusted_memory, cur) >= needed) {
                best = cur;
                break;
            }
            cur = blk_next(cur);
        }
    } else if (mode == fit_mode::the_best_fit) { // lower_bound в кч-дереве
        best = tree_search(_trusted_memory, get_root(_trusted_memory), needed);
    } else { // the_worst_fit — максимальный подходящий блок
        void* mx = tree_find_max(_trusted_memory, get_root(_trusted_memory));
        if (mx != nullptr && blk_size(_trusted_memory, mx) >= needed) {
            best = mx;
        }
    }

    if (best == nullptr) throw std::bad_alloc();

    size_t best_sz = blk_size(_trusted_memory, best);
    size_t remaining = best_sz - needed;

    rbt_remove(_trusted_memory, best); // удаляем из дерева свободных

    if (remaining >= free_block_metadata_size) { // есть остаток — разбиваем
        void* new_free = static_cast<char*>(best) + needed;
        set_bd(new_free, false, block_color::BLACK);
        set_blk_prev(new_free, best);
        set_blk_next(new_free, blk_next(best));
        if (blk_next(best) != nullptr) {
            set_blk_prev(blk_next(best), new_free);
        }
        set_blk_next(best, new_free);

        set_bd(best, true, block_color::BLACK);
        set_blk_parent_alloc(best, get_parent(_trusted_memory));

        rbt_insert(_trusted_memory, new_free); // остаток возвращаем в дерево
    } else { // отдаём весь блок
        set_bd(best, true, block_color::BLACK);
        set_blk_parent_alloc(best, get_parent(_trusted_memory));
    }

    return static_cast<char*>(best) + occupied_block_metadata_size; // адрес полезной нагрузки
}

void allocator_red_black_tree::do_deallocate_sm(void *at) {
    if (at == nullptr) return; // освобождение nullptr — no-op

    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory));

    void* reg = region_start(_trusted_memory);
    void* end = static_cast<char*>(reg) + get_space_size(_trusted_memory);
    void* block = static_cast<char*>(at) - occupied_block_metadata_size;

    // валидация: линейный проход по всем блокам
    void* cur = reg;
    while (cur < end) {
        if (cur == block) break;
        size_t sz = blk_size(_trusted_memory, cur);
        if (sz == 0 || cur > block) {
            throw std::invalid_argument("invalid pointer to deallocate");
        }
        cur = static_cast<char*>(cur) + sz;
    }
    if (cur != block || cur >= end) {
        throw std::invalid_argument("invalid pointer to deallocate");
    }
    if (!blk_occupied(block)) {
        throw std::invalid_argument("double free or corruption");
    }

    void* prev = blk_prev(block);
    void* next = blk_next(block);

    if (prev != nullptr && !blk_occupied(prev)) { // слияние слева
        rbt_remove(_trusted_memory, prev);
        block = prev;
    }
    if (next != nullptr && !blk_occupied(next)) { // слияние справа
        rbt_remove(_trusted_memory, next);
        next = blk_next(next);
    }

    set_blk_next(block, next);
    if (next != nullptr) {
        set_blk_prev(next, block);
    }

    set_bd(block, false, block_color::BLACK);
    set_blk_left(block, nullptr);
    set_blk_right(block, nullptr);
    set_blk_parent_tree(block, nullptr);

    rbt_insert(_trusted_memory, block); // вставляем объединённый блок
}

void allocator_red_black_tree::set_fit_mode(allocator_with_fit_mode::fit_mode mode) {
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory));
    get_mode(_trusted_memory) = mode;
}

// ==================== отладочная информация ====================

std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info() const {
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory));
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_red_black_tree::get_blocks_info_inner() const {
    std::vector<block_info> result;
    void* cur = region_start(_trusted_memory);
    void* end = static_cast<char*>(cur) + get_space_size(_trusted_memory);
    while (cur < end) {
        size_t sz = blk_size(_trusted_memory, cur);
        bool occ = blk_occupied(cur);
        result.push_back({sz, occ});
        cur = static_cast<char*>(cur) + sz;
    }
    return result;
}

// ==================== итераторы ====================

allocator_red_black_tree::rb_iterator allocator_red_black_tree::begin() const noexcept {
    return rb_iterator(_trusted_memory);
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::end() const noexcept {
    return rb_iterator(); // нулевой итератор = конец
}

bool allocator_red_black_tree::rb_iterator::operator==(const rb_iterator &other) const noexcept {
    return _block_ptr == other._block_ptr; // сравнение по указателю блока
}

bool allocator_red_black_tree::rb_iterator::operator!=(const rb_iterator &other) const noexcept {
    return !(*this == other);
}

allocator_red_black_tree::rb_iterator &allocator_red_black_tree::rb_iterator::operator++() & noexcept {
    if (_block_ptr == nullptr || _trusted == nullptr) return *this;
    _block_ptr = blk_next(_block_ptr); // переход по физическим связям
    return *this;
}

allocator_red_black_tree::rb_iterator allocator_red_black_tree::rb_iterator::operator++(int) {
    rb_iterator tmp = *this;
    ++(*this);
    return tmp;
}

size_t allocator_red_black_tree::rb_iterator::size() const noexcept {
    if (_block_ptr == nullptr || _trusted == nullptr) return 0;
    return blk_size(_trusted, _block_ptr);
}

void *allocator_red_black_tree::rb_iterator::operator*() const noexcept {
    if (_block_ptr == nullptr) return nullptr;
    return blk_occupied(_block_ptr)
        ? static_cast<char*>(_block_ptr) + allocator_red_black_tree::occupied_block_metadata_size
        : static_cast<char*>(_block_ptr) + allocator_red_black_tree::free_block_metadata_size;
}

allocator_red_black_tree::rb_iterator::rb_iterator() : _block_ptr(nullptr), _trusted(nullptr) {}

allocator_red_black_tree::rb_iterator::rb_iterator(void* trusted) : _block_ptr(nullptr), _trusted(trusted) {
    if (trusted == nullptr) return;
    void* first = region_start(trusted);
    void* end = static_cast<char*>(first) + get_space_size(trusted);
    if (first < end) {
        _block_ptr = first;
    } else {
        _trusted = nullptr;
    }
}

bool allocator_red_black_tree::rb_iterator::occupied() const noexcept {
    if (_block_ptr == nullptr) return false;
    return blk_occupied(_block_ptr);
}
