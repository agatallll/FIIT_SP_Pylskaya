#include <not_implemented.h> // исключение для неподдерживаемых операций (объяснялось ранее)
#include "../include/allocator_buddies_system.h" // собственный заголовок (объяснялся ранее)
#include <mutex>          // (объяснялся ранее)
#include <memory_resource> // (объяснялся ранее)
#include <stdexcept>      // (объяснялся ранее)
#include <cstddef>        // (объяснялся ранее)
#include <cstdint>        // (объяснялся ранее)
#include <vector>         // (объяснялся ранее)
#include <cstring>        // (объяснялся ранее)

namespace { // безымянное пространство имён (объяснялось ранее)
    // размер метаданных аллокатора (без padding, ручные смещения)
    constexpr size_t allocator_metadata_size_local = sizeof(allocator_dbg_helper*)
                                                   + sizeof(allocator_with_fit_mode::fit_mode)
                                                   + sizeof(unsigned char)
                                                   + sizeof(std::mutex); // локальная constexpr копия размера метаданных. unsigned char добавлен для хранения max_k.

    // смещения полей метаданных аллокатора
    constexpr size_t mtx_offset = 0;                                           // std::mutex
    constexpr size_t parent_offset = sizeof(std::mutex);                       // std::pmr::memory_resource*
    constexpr size_t mode_offset = parent_offset + sizeof(allocator_dbg_helper*); // fit_mode
    constexpr size_t max_k_offset = mode_offset + sizeof(allocator_with_fit_mode::fit_mode); // unsigned char — максимальная степень двойки (max_k). размер доверенной области = 2^max_k.

    std::mutex& get_mtx(void* trusted) noexcept { // (объяснялось ранее)
        return *reinterpret_cast<std::mutex*>(static_cast<char*>(trusted) + mtx_offset);
    }
    std::pmr::memory_resource*& get_parent(void* trusted) noexcept { // (объяснялось ранее)
        return *reinterpret_cast<std::pmr::memory_resource**>(static_cast<char*>(trusted) + parent_offset);
    }
    allocator_with_fit_mode::fit_mode& get_mode(void* trusted) noexcept { // (объяснялось ранее)
        return *reinterpret_cast<allocator_with_fit_mode::fit_mode*>(static_cast<char*>(trusted) + mode_offset);
    }
    unsigned char& get_max_k(void* trusted) noexcept { // возвращает ссылку на unsigned char — поле max_k в доверенной памяти. unsigned char — беззнаковый 1-байтовый тип (0..255), достаточный для хранения показателя степени двойки.
        return *reinterpret_cast<unsigned char*>(static_cast<char*>(trusted) + max_k_offset);
    }

    // начало области блоков (сразу за метаданными аллокатора)
    void* region_start(void* trusted) noexcept { // (объяснялось ранее)
        return static_cast<char*>(trusted) + allocator_metadata_size_local;
    }

    // безопасное чтение/запись void* через memcpy
    // (адреса блоков могут быть невыровненными из-за allocator_metadata_size = 93)
    void* read_ptr(void* addr) noexcept { // (объяснялось ранее)
        void* val;
        std::memcpy(&val, addr, sizeof(val));
        return val;
    }
    void write_ptr(void* addr, void* val) noexcept { // (объяснялось ранее)
        std::memcpy(addr, &val, sizeof(val));
    }
}

// ==================== правило пяти ====================

allocator_buddies_system::~allocator_buddies_system()
{
    if (_trusted_memory == nullptr) { // (объяснялось ранее)
        return;
    }
    std::pmr::memory_resource* parent = get_parent(_trusted_memory);
    unsigned char max_k_val = get_max_k(_trusted_memory); // читаем max_k из доверенной памяти. max_k — показатель степени двойки, определяющий общий размер региона блоков.
    size_t total_size = (static_cast<size_t>(1) << max_k_val) + allocator_metadata_size_local; // вычисляем общий размер выделенной памяти. static_cast<size_t>(1) << max_k_val — битовый сдвиг единицы влево на max_k_val позиций. это эквивалентно математическому выражению 2^max_k_val. например, если max_k_val = 12, получаем 1 << 12 = 4096 байт.
    get_mtx(_trusted_memory).~mutex(); // (объяснялось ранее)
    if (parent != nullptr) {
        parent->deallocate(_trusted_memory, total_size, alignof(std::max_align_t)); // (объяснялось ранее)
    } else {
        ::operator delete(_trusted_memory, total_size, std::align_val_t{alignof(std::max_align_t)}); // (объяснялось ранее)
    }
}

allocator_buddies_system::allocator_buddies_system(
    allocator_buddies_system const &other)
{
    throw not_implemented("allocator_buddies_system::allocator_buddies_system(const allocator_buddies_system &)", "copying is not supported"); // (объяснялось ранее)
}

allocator_buddies_system &allocator_buddies_system::operator=(
    allocator_buddies_system const &other)
{
    throw not_implemented("allocator_buddies_system::operator=(const allocator_buddies_system &)", "copying is not supported"); // (объяснялось ранее)
}

allocator_buddies_system::allocator_buddies_system(
    allocator_buddies_system &&other) noexcept
    : _trusted_memory(other._trusted_memory) // (объяснялось ранее)
{
    other._trusted_memory = nullptr;
}

allocator_buddies_system &allocator_buddies_system::operator=(
    allocator_buddies_system &&other) noexcept
{
    if (this != &other) { // (объяснялось ранее)
        if (_trusted_memory != nullptr) {
            std::pmr::memory_resource* parent = get_parent(_trusted_memory);
            unsigned char max_k_val = get_max_k(_trusted_memory); // читаем max_k текущего объекта.
            size_t total_size = (static_cast<size_t>(1) << max_k_val) + allocator_metadata_size_local; // 2^max_k + заголовок.
            get_mtx(_trusted_memory).~mutex();
            if (parent != nullptr) {
                parent->deallocate(_trusted_memory, total_size, alignof(std::max_align_t));
            } else {
                ::operator delete(_trusted_memory, total_size, std::align_val_t{alignof(std::max_align_t)});
            }
        }
        _trusted_memory = other._trusted_memory;
        other._trusted_memory = nullptr;
    }
    return *this;
}

// ==================== конструктор ====================

allocator_buddies_system::allocator_buddies_system(
        size_t space_size,
        std::pmr::memory_resource *parent_allocator,
        allocator_with_fit_mode::fit_mode allocate_fit_mode)
    : _trusted_memory(nullptr) // (объяснялось ранее)
{
    if (parent_allocator == nullptr) {
        parent_allocator = std::pmr::get_default_resource(); // (объяснялось ранее)
    }
    if (space_size < (static_cast<size_t>(1) << min_k)) { // проверка: запрошенный размер должен быть не меньше минимального блока (2^min_k). 1 << min_k — вычисление 2 в степени min_k через битовый сдвиг. если меньше — бросаем исключение, т.к. система двойников не может работать с блоками меньше минимума.
        throw std::logic_error("space size is too small for buddy system"); // (объяснялось ранее)
    }

    // округляем размер до ближайшей степени двойки
    size_t max_k_val = min_k; // начинаем с минимальной степени.
    size_t pow2 = static_cast<size_t>(1) << min_k; // pow2 = 2^min_k — начальный размер.
    while (pow2 < space_size) { // пока текущая степень двойки меньше запрошенного размера...
        ++max_k_val;            // ...увеличиваем показатель степени на 1...
        pow2 <<= 1;             // ...и удваиваем pow2 (сдвиг влево на 1 равен умножению на 2).
    } // по окончании цикла pow2 — ближайшая степень двойки, большая или равная space_size, а max_k_val — её показатель.

    size_t total_size = pow2 + allocator_metadata_size_local; // общий размер = размер региона блоков + заголовок аллокатора.
    _trusted_memory = parent_allocator->allocate(total_size, alignof(std::max_align_t)); // (объяснялось ранее)

    new (static_cast<char*>(_trusted_memory) + mtx_offset) std::mutex(); // placement new для мьютекса (объяснялось ранее)
    get_parent(_trusted_memory) = parent_allocator; // (объяснялось ранее)
    get_mode(_trusted_memory) = allocate_fit_mode; // (объяснялось ранее)
    get_max_k(_trusted_memory) = static_cast<unsigned char>(max_k_val); // сохраняем max_k в доверенную память. static_cast<unsigned char> — явное приведение size_t к unsigned char. это безопасно, т.к. max_k_val обычно невелик (например, 12 для 4 кб).

    void* first = region_start(_trusted_memory); // адрес первого блока.
    block_metadata meta; // создаём локальную переменную типа block_metadata (структура с битовыми полями).
    meta.occupied = false; // блок свободен.
    meta.size = static_cast<unsigned char>(max_k_val); // размер блока = max_k_val (т.е. блок занимает весь регион).
    std::memcpy(first, &meta, sizeof(meta)); // копируем 1 байт структуры meta по адресу first. используем memcpy, т.к. адрес first может быть невыровнен для структуры (хотя здесь размер 1 байт, выравнивание не критично, но используется единообразно).
}

// ==================== allocate / deallocate ====================

[[nodiscard]] void *allocator_buddies_system::do_allocate_sm(
    size_t size)
{
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory)); // (объяснялось ранее)

    size_t total_needed = size + occupied_block_metadata_size; // общий размер = запрошенный + метаданные занятого блока.
    unsigned char k_needed = min_k; // начинаем с минимальной степени.
    while ((static_cast<size_t>(1) << k_needed) < total_needed) { // подбираем минимальную степень двойки k_needed, такую что 2^k_needed >= total_needed. сдвиг 1 << k_needed вычисляет 2^k_needed.
        ++k_needed;
    }

    unsigned char max_k_val = get_max_k(_trusted_memory); // максимальная степень региона.
    void* reg_start = region_start(_trusted_memory); // начало региона блоков.
    void* reg_end = static_cast<char*>(reg_start) + (static_cast<size_t>(1) << max_k_val); // конец региона = начало + 2^max_k_val.

    void* best = nullptr; // выбранный блок.
    unsigned char best_k = 0; // степень выбранного блока.

    for (void* curr = reg_start; curr < reg_end; ) { // линейный обход всех блоков в регионе. итерация не через i++, а через curr = curr + block_size.
        block_metadata meta; std::memcpy(&meta, curr, sizeof(meta)); // читаем метаданные текущего блока через memcpy в локальную переменную. так безопасно при любом выравнивании.
        unsigned char k = meta.size; // читаем поле size из bit fields (показатель степени блока).
        if (!meta.occupied && k >= k_needed) { // если блок свободен и достаточно велик...
            switch (get_mode(_trusted_memory)) { // (логика first/best/worst fit объяснялась во 2-й задаче)
                case fit_mode::first_fit:
                    best = curr;
                    best_k = k;
                    curr = reg_end; // пометка для выхода из цикла
                    break;
                case fit_mode::the_best_fit:
                    if (best == nullptr || k < best_k) {
                        best = curr;
                        best_k = k;
                    }
                    break;
                case fit_mode::the_worst_fit:
                    if (best == nullptr || k > best_k) {
                        best = curr;
                        best_k = k;
                    }
                    break;
            }
        }
        if (curr < reg_end) {
            curr = static_cast<char*>(curr) + (static_cast<size_t>(1) << k); // переходим к следующему блоку: прибавляем 2^k (текущий размер блока).
        }
    }

    if (best == nullptr) { // (объяснялось ранее)
        throw std::bad_alloc();
    }

    // разбиваем блок до нужного размера (split / buddy splitting)
    while (best_k > k_needed) { // пока блок больше, чем нужно...
        --best_k;               // ...уменьшаем степень на 1 (размер в 2 раза меньше).
        size_t half = static_cast<size_t>(1) << best_k; // half = 2^best_k — размер половинки после разбиения.
        void* buddy = static_cast<char*>(best) + half; // адрес buddy (второй половинки) = best + half. buddy — «двойник» блока: два блока-buddy имеют одинаковый размер и адреса, отличающиеся ровно на half, причём их смещения от начала региона отличаются только одним битом (тот, который соответствует half).

        block_metadata buddy_meta; // создаём метаданные для buddy.
        buddy_meta.occupied = false; // buddy свободен.
        buddy_meta.size = best_k;    // размер buddy = новая уменьшенная степень.
        std::memcpy(buddy, &buddy_meta, sizeof(buddy_meta)); // записываем метаданные buddy в память.

        block_metadata best_meta; // обновляем метаданные исходного блока (теперь он тоже уменьшился вдвое).
        best_meta.occupied = false;
        best_meta.size = best_k;
        std::memcpy(best, &best_meta, sizeof(best_meta));
    } // цикл повторяется, пока блок не станет нужного размера. на каждой итерации создаётся один свободный buddy.

    block_metadata occupied_meta; // финальные метаданные для выделяемого блока.
    occupied_meta.occupied = true; // блок занят.
    occupied_meta.size = best_k;   // фиксируем размер (теперь равен k_needed).
    std::memcpy(best, &occupied_meta, sizeof(occupied_meta)); // записываем в память.

    write_ptr(static_cast<char*>(best) + sizeof(block_metadata), get_parent(_trusted_memory)); // записываем указатель на родительский аллокатор в метаданные занятого блока (сразу после bit fields). это нужно для проверки принадлежности при освобождении.

    return static_cast<char*>(best) + occupied_block_metadata_size; // возвращаем payload — пропускаем bit fields и указатель родителя.
}

void allocator_buddies_system::do_deallocate_sm(
    void *at)
{
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory)); // (объяснялось ранее)

    void* block_start = static_cast<char*>(at) - occupied_block_metadata_size; // вычисляем адрес заголовка блока из пользовательского указателя.

    unsigned char max_k_val = get_max_k(_trusted_memory); // максимальная степень региона.
    void* reg_start = region_start(_trusted_memory); // начало региона.
    void* reg_end = static_cast<char*>(reg_start) + (static_cast<size_t>(1) << max_k_val); // конец региона.

    // валидация: block_start должен быть началом одного из блоков
    void* curr = reg_start;
    while (curr < reg_end) { // линейный обход блоков (объяснялся во 2-й задаче)
        if (curr == block_start) {
            break;
        }
        block_metadata meta; std::memcpy(&meta, curr, sizeof(meta)); // читаем метаданные через memcpy.
        size_t sz = static_cast<size_t>(1) << meta.size; // размер блока = 2^meta.size.
        if (sz == 0 || curr > block_start) {
            throw std::invalid_argument("invalid pointer to deallocate"); // (объяснялось ранее)
        }
        curr = static_cast<char*>(curr) + sz;
    }
    if (curr != block_start || curr >= reg_end) {
        throw std::invalid_argument("invalid pointer to deallocate"); // (объяснялось ранее)
    }

    block_metadata meta; std::memcpy(&meta, block_start, sizeof(meta)); // читаем метаданные освобождаемого блока.
    if (!meta.occupied) {
        throw std::invalid_argument("double free or corruption"); // (объяснялось ранее)
    }

    unsigned char k = meta.size; // текущая степень освобождаемого блока.

    // слияние с buddy (coalescing)
    while (true) { // бесконечный цикл, прерывается break, когда buddy нельзя объединить.
        size_t block_size = static_cast<size_t>(1) << k; // block_size = 2^k — текущий размер блока.
        uintptr_t offset = reinterpret_cast<uintptr_t>(block_start) - reinterpret_cast<uintptr_t>(reg_start); // reinterpret_cast<uintptr_t>(pointer) — преобразование указателя в целое число типа uintptr_t (беззнаковое, достаточно большое, чтобы вместить любой указатель). это нужно для битовых операций над адресами. offset — смещение текущего блока от начала региона в байтах.
        uintptr_t buddy_offset = offset ^ block_size; // КЛЮЧЕВАЯ ОПЕРАЦИЯ СИСТЕМЫ ДВОЙНИКОВ. XOR (^) с размером block_size переключает бит, соответствующий block_size. поскольку регион — это степень двойки, а блоки делятся пополам рекурсивно, два buddy-блока одного уровня k имеют смещения, отличающиеся ровно на block_size. XOR с block_size меняет этот бит, вычисляя смещение buddy. например: offset=0, block_size=512 → buddy_offset = 0 ^ 512 = 512. offset=512, block_size=512 → buddy_offset = 512 ^ 512 = 0. offset=1024, block_size=512 → buddy_offset = 1024 ^ 512 = 1536.
        void* buddy = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(reg_start) + buddy_offset); // обратное преобразование: складываем целочисленное смещение buddy с базовым адресом reg_start (предварительно приведённым к uintptr_t) и получаем указатель на buddy.

        if (buddy < reg_start || buddy >= reg_end) { // проверка границ: buddy должен лежать внутри региона.
            break;
        }
        block_metadata buddy_meta; std::memcpy(&buddy_meta, buddy, sizeof(buddy_meta)); // читаем метаданные buddy.
        if (buddy_meta.occupied || buddy_meta.size != k) { // если buddy занят ИЛИ buddy имеет другой размер (не тот же уровень k)...
            break; // ...слияние невозможно.
        }
        if (buddy < block_start) { // если buddy левее текущего блока...
            block_start = buddy;   // ...новый объединённый блок начинается с buddy (чтобы адрес всегда был минимальным).
        }
        ++k; // увеличиваем степень на 1 (объединённый блок вдвое больше).
    } // цикл повторяется: теперь пытаемся слиться с buddy нового, большего размера.

    block_metadata free_meta; // финальные метаданные после всех слияний.
    free_meta.occupied = false; // блок свободен.
    free_meta.size = k;         // новая степень (возможно, увеличилась после coalescing).
    std::memcpy(block_start, &free_meta, sizeof(free_meta)); // записываем в память.
}

// ==================== вспомогательные методы ====================

void allocator_buddies_system::set_fit_mode(
    allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory)); // (объяснялось ранее)
    get_mode(_trusted_memory) = mode;
}

bool allocator_buddies_system::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return this == &other; // (объяснялось ранее)
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info() const noexcept
{
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory)); // (объяснялось ранее)
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info_inner() const
{
    std::vector<block_info> result; // (объяснялось ранее)
    unsigned char max_k_val = get_max_k(_trusted_memory); // максимальная степень.
    void* curr = region_start(_trusted_memory); // начало региона.
    void* end = static_cast<char*>(curr) + (static_cast<size_t>(1) << max_k_val); // конец региона = curr + 2^max_k.
    while (curr < end) {
        block_metadata meta; std::memcpy(&meta, curr, sizeof(meta)); // читаем bit fields.
        size_t sz = static_cast<size_t>(1) << meta.size; // размер = 2^meta.size.
        result.push_back({sz, meta.occupied}); // (объяснялось ранее)
        curr = static_cast<char*>(curr) + sz; // переходим к следующему блоку.
    }
    return result;
}

// ==================== итераторы ====================

allocator_buddies_system::buddy_iterator allocator_buddies_system::begin() const noexcept
{
    return buddy_iterator(region_start(_trusted_memory)); // начинаем с первого блока.
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::end() const noexcept
{
    unsigned char max_k_val = get_max_k(_trusted_memory); // (объяснялось ранее)
    void* reg_start = region_start(_trusted_memory);
    void* reg_end = static_cast<char*>(reg_start) + (static_cast<size_t>(1) << max_k_val); // конец региона.
    return buddy_iterator(reg_end); // end-итератор указывает на reg_end (адрес за последним байтом региона).
}

// --- buddy_iterator ---

allocator_buddies_system::buddy_iterator::buddy_iterator()
    : _block(nullptr) // пустой итератор.
{
}

allocator_buddies_system::buddy_iterator::buddy_iterator(void* start)
    : _block(start) // инициализация указателем на блок.
{
}

bool allocator_buddies_system::buddy_iterator::operator==(
        const buddy_iterator& other) const noexcept
{
    return _block == other._block; // сравнение указателей.
}

bool allocator_buddies_system::buddy_iterator::operator!=(
        const buddy_iterator& other) const noexcept
{
    return _block != other._block; // инверсия сравнения.
}

allocator_buddies_system::buddy_iterator& allocator_buddies_system::buddy_iterator::operator++() & noexcept
{
    if (_block == nullptr) { // защита от nullptr.
        return *this;
    }
    block_metadata meta; std::memcpy(&meta, _block, sizeof(meta)); // читаем bit fields.
    _block = static_cast<char*>(_block) + (static_cast<size_t>(1) << meta.size); // сдвигаем на размер блока (2^meta.size).
    return *this;
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::buddy_iterator::operator++(int)
{
    buddy_iterator tmp = *this; // (объяснялось ранее)
    ++(*this);
    return tmp;
}

size_t allocator_buddies_system::buddy_iterator::size() const noexcept
{
    if (_block == nullptr) {
        return 0;
    }
    block_metadata meta; std::memcpy(&meta, _block, sizeof(meta)); // читаем bit fields.
    return static_cast<size_t>(1) << meta.size; // возвращаем 2^meta.size.
}

bool allocator_buddies_system::buddy_iterator::occupied() const noexcept
{
    if (_block == nullptr) {
        return false;
    }
    block_metadata meta; std::memcpy(&meta, _block, sizeof(meta)); // читаем bit fields.
    return meta.occupied; // возвращаем 1-битовое поле занятости.
}

void* allocator_buddies_system::buddy_iterator::operator*() const noexcept
{
    if (_block == nullptr) {
        return nullptr;
    }
    return static_cast<char*>(_block) + allocator_buddies_system::occupied_block_metadata_size; // payload после bit fields и указателя родителя.
}
