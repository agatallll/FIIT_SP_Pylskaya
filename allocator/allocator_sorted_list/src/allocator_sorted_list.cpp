#include "../include/allocator_sorted_list.h" // заголовочный файл с объявлением класса и итераторов
#include <cstddef>                            // стандартные типы размеров и выравнивания
#include <cstring>                            // побайтовое копирование памяти при перемещении/копировании
#include <stdexcept>                          // исключения для обработки ошибок (bad_alloc, logic_error)
#include <limits>                             // numeric_limits для проверки переполнения при сложении размеров

// ---------- вспомогательные функции для работы с заголовком аллокатора ----------

namespace {

    // вычисляем реальный размер заголовка аллокатора с учётом выравнивания max_align_t
    size_t header_size() noexcept {
        size_t raw = sizeof(std::mutex)                            // примитив синхронизации
                   + sizeof(std::pmr::memory_resource*)            // указатель на родительский аллокатор
                   + sizeof(size_t)                                // общий размер доверенной памяти
                   + sizeof(void*)                                 // указатель на первый свободный блок
                   + sizeof(allocator_with_fit_mode::fit_mode);    // режим подбора блока
        size_t align = alignof(std::max_align_t);
        return (raw + align - 1) & ~(align - 1);                   // округляем вверх до границы выравнивания
    }

    // размер метаданных одного блока (заголовок + указатель на следующий свободный)
    constexpr size_t block_meta_size() noexcept {
        return sizeof(void*) + sizeof(size_t);
    }

    // получение ссылки на мьютекс, расположенный по смещению 0 в доверенной памяти
    std::mutex* mutex_of(void* trusted) noexcept {
        return reinterpret_cast<std::mutex*>(trusted);
    }

    // получение ссылки на указатель родительского аллокатора
    std::pmr::memory_resource*& parent_of(void* trusted) noexcept {
        return *reinterpret_cast<std::pmr::memory_resource**>(
            static_cast<char*>(trusted) + sizeof(std::mutex));
    }

    // получение ссылки на поле общего размера доверенной памяти
    size_t& total_size_of(void* trusted) noexcept {
        return *reinterpret_cast<size_t*>(
            static_cast<char*>(trusted) + sizeof(std::mutex) + sizeof(std::pmr::memory_resource*));
    }

    // получение ссылки на указатель первого свободного блока
    void*& first_free_of(void* trusted) noexcept {
        return *reinterpret_cast<void**>(
            static_cast<char*>(trusted) + sizeof(std::mutex) + sizeof(std::pmr::memory_resource*) + sizeof(size_t));
    }

    // получение ссылки на режим подбора (first/best/worst fit)
    allocator_with_fit_mode::fit_mode& fit_mode_of(void* trusted) noexcept {
        return *reinterpret_cast<allocator_with_fit_mode::fit_mode*>(
            static_cast<char*>(trusted) + sizeof(std::mutex) + sizeof(std::pmr::memory_resource*) + sizeof(size_t) + sizeof(void*));
    }

    // ---------- вспомогательные функции для работы с блоком ----------

    // размер блока (хранится в первых sizeof(size_t) байтах заголовка блока)
    size_t& blk_size(void* block) noexcept {
        return *reinterpret_cast<size_t*>(block);
    }

    // указатель на следующий свободный блок (идёт сразу за размером)
    void*& blk_next(void* block) noexcept {
        return *reinterpret_cast<void**>(static_cast<char*>(block) + sizeof(size_t));
    }

    // получение адреса полезной нагрузки (payload) из адреса заголовка блока
    void* blk_payload(void* block) noexcept {
        return static_cast<char*>(block) + block_meta_size();
    }

    // обратное преобразование: из адреса, выданного пользователю, получаем адрес заголовка блока
    void* blk_from_payload(void* payload) noexcept {
        return static_cast<char*>(payload) - block_meta_size();
    }

} // anonymous namespace

// ==================== конструкторы, деструктор, правило пяти ====================

allocator_sorted_list::allocator_sorted_list(
    size_t space_size,
    std::pmr::memory_resource *parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    void* mem; // указатель на выделенную доверенную память
    if (parent_allocator == nullptr) {
        mem = ::operator new(space_size); // запрашиваем память из глобальной кучи
    } else {
        mem = parent_allocator->allocate(space_size, alignof(std::max_align_t)); // делегируем родительскому ресурсу
    }
    _trusted_memory = mem; // единственное поле класса — сохраняем указатель

    parent_of(_trusted_memory) = parent_allocator;       // запоминаем, откуда память
    total_size_of(_trusted_memory) = space_size;         // фиксируем общий размер
    first_free_of(_trusted_memory) = nullptr;            // пока нет свободных блоков
    fit_mode_of(_trusted_memory) = allocate_fit_mode;    // устанавливаем начальный режим выделения

    new (mutex_of(_trusted_memory)) std::mutex();        // placement new для мьютекса в доверенной памяти

    size_t hdr = header_size();                          // реальный размер заголовка
    if (space_size > hdr) {                              // если осталось место под хотя бы один блок
        void* first = static_cast<char*>(_trusted_memory) + hdr; // адрес первого (единственного) блока
        first_free_of(_trusted_memory) = first;          // он пока единственный свободный
        blk_size(first) = space_size - hdr;              // размер блока = всё оставшееся пространство
        blk_next(first) = nullptr;                       // следующего свободного нет
    }
}

allocator_sorted_list::~allocator_sorted_list()
{
    if (_trusted_memory == nullptr) { // объект мог быть перемещён
        return;
    }
    std::pmr::memory_resource* parent = parent_of(_trusted_memory); // узнаём владельца памяти
    mutex_of(_trusted_memory)->~mutex(); // явно вызываем деструктор мьютекса (placement new требует ручного уничтожения)
    if (parent == nullptr) {
        ::operator delete(_trusted_memory); // возвращаем память в глобальную кучу
    } else {
        parent->deallocate(_trusted_memory, total_size_of(_trusted_memory), alignof(std::max_align_t)); // возвращаем родителю
    }
}

allocator_sorted_list::allocator_sorted_list(const allocator_sorted_list &other)
{
    if (other._trusted_memory == nullptr) { // копируем пустой (перемещённый) объект
        _trusted_memory = nullptr;
        return;
    }

    size_t total = total_size_of(other._trusted_memory); // размер доверенной памяти источника
    std::pmr::memory_resource* parent = parent_of(other._trusted_memory); // откуда выделять новую память
    void* new_mem;
    if (parent == nullptr) {
        new_mem = ::operator new(total); // глобальная куча
    } else {
        new_mem = parent->allocate(total, alignof(std::max_align_t)); // родительский аллокатор
    }

    std::memcpy(new_mem, other._trusted_memory, total); // копируем побайтно все блоки и метаданные

    // пересчитываем указатели свободных блоков, так как базовый адрес изменился
    void* old_base = other._trusted_memory;
    void* new_base = new_mem;
    auto remap = [old_base, new_base](void* ptr) -> void* {
        if (ptr == nullptr) return nullptr;
        return static_cast<char*>(new_base) + (static_cast<char*>(ptr) - static_cast<char*>(old_base));
    };

    first_free_of(new_mem) = remap(first_free_of(old_base)); // новый адрес головы списка
    void* curr = first_free_of(new_mem);
    while (curr != nullptr) {
        blk_next(curr) = remap(blk_next(curr)); // обновляем каждый next в списке
        curr = blk_next(curr);
    }

    new (mutex_of(new_mem)) std::mutex(); // создаём новый мьютекс (старый нельзя копировать)
    _trusted_memory = new_mem;
}

allocator_sorted_list &allocator_sorted_list::operator=(const allocator_sorted_list &other)
{
    if (this == &other) { // защита от самоприсваивания
        return *this;
    }

    void* old_trusted = _trusted_memory; // сохраняем старую память для освобождения

    if (other._trusted_memory == nullptr) {
        _trusted_memory = nullptr; // источник пуст
    } else {
        size_t total = total_size_of(other._trusted_memory);
        std::pmr::memory_resource* parent = parent_of(other._trusted_memory);
        void* new_mem;
        if (parent == nullptr) {
            new_mem = ::operator new(total);
        } else {
            new_mem = parent->allocate(total, alignof(std::max_align_t));
        }

        std::memcpy(new_mem, other._trusted_memory, total);

        void* old_base = other._trusted_memory;
        void* new_base = new_mem;
        auto remap = [old_base, new_base](void* ptr) -> void* {
            if (ptr == nullptr) return nullptr;
            return static_cast<char*>(new_base) + (static_cast<char*>(ptr) - static_cast<char*>(old_base));
        };

        first_free_of(new_mem) = remap(first_free_of(old_base));
        void* curr = first_free_of(new_mem);
        while (curr != nullptr) {
            blk_next(curr) = remap(blk_next(curr));
            curr = blk_next(curr);
        }

        new (mutex_of(new_mem)) std::mutex();
        _trusted_memory = new_mem;
    }

    if (old_trusted != nullptr) { // освобождаем предыдущие ресурсы
        std::pmr::memory_resource* old_parent = parent_of(old_trusted);
        mutex_of(old_trusted)->~mutex();
        if (old_parent == nullptr) {
            ::operator delete(old_trusted);
        } else {
            old_parent->deallocate(old_trusted, total_size_of(old_trusted), alignof(std::max_align_t));
        }
    }

    return *this;
}

allocator_sorted_list::allocator_sorted_list(allocator_sorted_list &&other) noexcept
    : _trusted_memory(other._trusted_memory) // забираем владение доверенной памятью
{
    other._trusted_memory = nullptr; // источник больше не владеет памятью
}

allocator_sorted_list &allocator_sorted_list::operator=(allocator_sorted_list &&other) noexcept
{
    if (this == &other) { // защита от самоперемещения
        return *this;
    }

    if (_trusted_memory != nullptr) { // освобождаем текущую память перед захватом новой
        std::pmr::memory_resource* old_parent = parent_of(_trusted_memory);
        mutex_of(_trusted_memory)->~mutex();
        if (old_parent == nullptr) {
            ::operator delete(_trusted_memory);
        } else {
            old_parent->deallocate(_trusted_memory, total_size_of(_trusted_memory), alignof(std::max_align_t));
        }
    }

    _trusted_memory = other._trusted_memory; // переносим владение
    other._trusted_memory = nullptr;
    return *this;
}

// ==================== основные операции выделения/освобождения ====================

[[nodiscard]] void *allocator_sorted_list::do_allocate_sm(size_t size)
{
    std::lock_guard<std::mutex> lock(*mutex_of(_trusted_memory)); // синхронизируем доступ к аллокатору

    if (size == 0) { // нулевой запрос трактуем как запрос минимального полезного размера
        size = 1;
    }

    if (size > std::numeric_limits<size_t>::max() - block_meta_size()) { // проверка на переполнение size_t
        throw std::bad_alloc(); // при переполнении невозможно корректно выделить блок
    }
    size_t needed = size + block_meta_size(); // полный размер блока с учётом служебных данных

    void* prev_free = nullptr;          // предыдущий свободный блок при обходе списка
    void* curr_free = first_free_of(_trusted_memory); // текущий свободный блок
    void* best = nullptr;               // выбранный подходящий блок
    void* best_prev = nullptr;          // предыдущий перед выбранным
    allocator_with_fit_mode::fit_mode mode = fit_mode_of(_trusted_memory); // текущий режим подбора

    while (curr_free != nullptr) { // обходим отсортированный список свободных блоков
        size_t curr_sz = blk_size(curr_free);
        if (curr_sz >= needed) { // блок подходит по размеру
            if (mode == allocator_with_fit_mode::fit_mode::first_fit) {
                best = curr_free;
                best_prev = prev_free;
                break; // первый подходящий найден — останавливаем поиск
            } else if (mode == allocator_with_fit_mode::fit_mode::the_best_fit) {
                if (best == nullptr || curr_sz < blk_size(best)) { // ищем наименьший достаточный блок
                    best = curr_free;
                    best_prev = prev_free;
                }
            } else { // the_worst_fit
                if (best == nullptr || curr_sz > blk_size(best)) { // ищем наибольший достаточный блок
                    best = curr_free;
                    best_prev = prev_free;
                }
            }
        }
        prev_free = curr_free;
        curr_free = blk_next(curr_free); // переходим к следующему свободному блоку
    }

    if (best == nullptr) { // ни один блок не подошёл
        throw std::bad_alloc(); // сообщаем вызывающему коду о нехватке памяти
    }

    size_t best_sz = blk_size(best);
    size_t remaining = best_sz - needed; // остаток после выделения запрошенного фрагмента

    if (remaining >= block_meta_size() + 1) { // остаток достаточен для нового свободного блока
        void* new_free = static_cast<char*>(best) + needed; // адрес оставшегося фрагмента
        blk_size(new_free) = remaining;                     // размер остатка
        blk_next(new_free) = blk_next(best);                // наследуем ссылку на следующий свободный

        blk_size(best) = needed;                            // фиксируем размер выделяемого блока

        if (best_prev == nullptr) {
            first_free_of(_trusted_memory) = new_free;      // обновляем голову списка
        } else {
            blk_next(best_prev) = new_free;                 // включаем остаток в список
        }
    } else {
        // остаток слишком мал — отдаём весь блок целиком, чтобы не образовывать непригодные микро-осколки
        if (best_prev == nullptr) {
            first_free_of(_trusted_memory) = blk_next(best);
        } else {
            blk_next(best_prev) = blk_next(best);
        }
    }

    return blk_payload(best); // возвращаем указатель на полезную нагрузку
}

void allocator_sorted_list::do_deallocate_sm(void *at)
{
    if (at == nullptr) { // освобождение nullptr — корректная no-op операция
        return;
    }

    std::lock_guard<std::mutex> lock(*mutex_of(_trusted_memory)); // синхронизируем освобождение

    void* block = blk_from_payload(at); // получаем адрес заголовка блока по адресу полезной нагрузки
    char* trusted_start = static_cast<char*>(_trusted_memory);
    char* trusted_end = trusted_start + total_size_of(_trusted_memory);
    char* block_char = static_cast<char*>(block);

    // валидация: блок должен находиться внутри доверенной области, за пределами заголовка
    if (block_char < trusted_start + static_cast<ptrdiff_t>(header_size()) ||
        block_char + static_cast<ptrdiff_t>(block_meta_size()) > trusted_end) {
        throw std::logic_error("deallocate: block does not belong to this allocator");
    }

    // проверка: указатель должен быть началом реально существующего блока (а не серединой)
    bool found = false;
    void* cur = trusted_start + header_size();
    while (cur < trusted_end) {
        if (cur == block) {
            found = true;
            break;
        }
        size_t sz = blk_size(cur);
        if (sz == 0 || static_cast<char*>(cur) + sz > trusted_end) { // защита от повреждённых данных
            break;
        }
        cur = static_cast<char*>(cur) + sz;
    }
    if (!found) {
        throw std::logic_error("deallocate: invalid block pointer");
    }

    // проверка на двойное освобождение: ищем блок в списке свободных
    void* check = first_free_of(_trusted_memory);
    while (check != nullptr) {
        if (check == block) {
            throw std::logic_error("deallocate: double free detected");
        }
        check = blk_next(check);
    }

    // вставляем освобождённый блок в отсортированный список свободных (по возрастанию адреса)
    void* prev_free = nullptr;
    void* curr_free = first_free_of(_trusted_memory);
    while (curr_free != nullptr && curr_free < block) {
        prev_free = curr_free;
        curr_free = blk_next(curr_free);
    }

    blk_next(block) = curr_free;
    if (prev_free == nullptr) {
        first_free_of(_trusted_memory) = block;
    } else {
        blk_next(prev_free) = block;
    }

    // слияние (coalescing) с правым соседом, если он свободный и примыкает вплотную
    if (curr_free != nullptr && block_char + static_cast<ptrdiff_t>(blk_size(block)) == static_cast<char*>(curr_free)) {
        blk_size(block) += blk_size(curr_free);
        blk_next(block) = blk_next(curr_free);
    }

    // слияние с левым соседом, если он свободный и примыкает вплотную
    if (prev_free != nullptr && static_cast<char*>(prev_free) + static_cast<ptrdiff_t>(blk_size(prev_free)) == block_char) {
        blk_size(prev_free) += blk_size(block);
        blk_next(prev_free) = blk_next(block);
    }
}

bool allocator_sorted_list::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    auto p = dynamic_cast<const allocator_sorted_list*>(&other); // проверяем тип
    if (p == nullptr) {
        return false; // другой тип ресурса — не эквивалентен
    }
    return _trusted_memory == p->_trusted_memory; // эквивалентны только при полном совпадении памяти
}

inline void allocator_sorted_list::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard<std::mutex> lock(*mutex_of(_trusted_memory)); // защищаем изменение режима
    fit_mode_of(_trusted_memory) = mode;
}

// ==================== отладочная/тестовая информация ====================

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info() const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(*mutex_of(_trusted_memory)); // синхронизируем сбор информации
        return get_blocks_info_inner();
    } catch (...) { // get_blocks_info объявлена noexcept — перехватываем любые исключения
        return {}; // при ошибке возвращаем пустой вектор
    }
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info_inner() const
{
    std::vector<block_info> result;
    void* current = static_cast<char*>(_trusted_memory) + header_size(); // первый блок за заголовком
    void* end_ptr = static_cast<char*>(_trusted_memory) + total_size_of(_trusted_memory);
    void* free_current = first_free_of(_trusted_memory); // указатель на текущий свободный блок при обходе

    while (current < end_ptr) {
        size_t sz = blk_size(current);
        bool is_occupied = (current != free_current); // если адрес совпадает со свободным — блок занят
        result.push_back({sz, is_occupied});
        if (!is_occupied) {
            free_current = blk_next(free_current); // продвигаем указатель по списку свободных
        }
        current = static_cast<char*>(current) + sz; // переходим к следующему физическому блоку
    }
    return result;
}

// ==================== итераторы по всем блокам ====================

allocator_sorted_list::sorted_iterator::sorted_iterator()
    : _free_ptr(nullptr), _current_ptr(nullptr), _trusted_memory(nullptr) // конструктор по умолчанию: нулевые указатели
{
}

allocator_sorted_list::sorted_iterator::sorted_iterator(void* trusted)
    : _trusted_memory(trusted) // инициализируем итератор началом блоков
{
    _current_ptr = static_cast<char*>(trusted) + header_size();
    _free_ptr = first_free_of(trusted);
}

bool allocator_sorted_list::sorted_iterator::operator==(const sorted_iterator& other) const noexcept
{
    return _current_ptr == other._current_ptr; // два итератора равны, если смотрят на один блок
}

bool allocator_sorted_list::sorted_iterator::operator!=(const sorted_iterator& other) const noexcept
{
    return !(*this == other);
}

allocator_sorted_list::sorted_iterator& allocator_sorted_list::sorted_iterator::operator++() & noexcept
{
    _current_ptr = static_cast<char*>(_current_ptr) + blk_size(_current_ptr); // переход к следующему физическому блоку
    if (_current_ptr == _free_ptr) { // если попали на свободный блок
        _free_ptr = blk_next(_free_ptr); // продвигаем указатель свободных
    }
    return *this;
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::sorted_iterator::operator++(int)
{
    sorted_iterator tmp = *this; // сохраняем текущее состояние
    ++(*this);                   // двигаемся вперёд
    return tmp;                  // возвращаем предыдущее состояние
}

size_t allocator_sorted_list::sorted_iterator::size() const noexcept
{
    return blk_size(_current_ptr); // размер текущего блока (включая метаданные)
}

void* allocator_sorted_list::sorted_iterator::operator*() const noexcept
{
    return blk_payload(_current_ptr); // адрес полезной нагрузки текущего блока
}

bool allocator_sorted_list::sorted_iterator::occupied() const noexcept
{
    return _current_ptr != _free_ptr; // текущий блок занят, если он не совпадает со следующим свободным
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::begin() const noexcept
{
    return sorted_iterator(_trusted_memory); // начинаем с первого блока
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::end() const noexcept
{
    sorted_iterator it;
    it._trusted_memory = _trusted_memory;
    it._current_ptr = static_cast<char*>(_trusted_memory) + total_size_of(_trusted_memory); // конец памяти
    it._free_ptr = nullptr;
    return it;
}

// ==================== итераторы только по свободным блокам ====================

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator()
    : _free_ptr(nullptr) // конструктор по умолчанию — нулевой указатель
{
}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator(void* trusted)
    : _free_ptr(first_free_of(trusted)) // начинаем с головы списка свободных блоков
{
}

bool allocator_sorted_list::sorted_free_iterator::operator==(const sorted_free_iterator& other) const noexcept
{
    return _free_ptr == other._free_ptr;
}

bool allocator_sorted_list::sorted_free_iterator::operator!=(const sorted_free_iterator& other) const noexcept
{
    return !(*this == other);
}

allocator_sorted_list::sorted_free_iterator& allocator_sorted_list::sorted_free_iterator::operator++() & noexcept
{
    _free_ptr = blk_next(_free_ptr); // переходим к следующему свободному блоку через указатель
    return *this;
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::sorted_free_iterator::operator++(int)
{
    sorted_free_iterator tmp = *this;
    ++(*this);
    return tmp;
}

size_t allocator_sorted_list::sorted_free_iterator::size() const noexcept
{
    return blk_size(_free_ptr);
}

void* allocator_sorted_list::sorted_free_iterator::operator*() const noexcept
{
    return blk_payload(_free_ptr); // адрес полезной нагрузки свободного блока
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_begin() const noexcept
{
    return sorted_free_iterator(_trusted_memory);
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_end() const noexcept
{
    sorted_free_iterator it;
    it._free_ptr = nullptr; // конец списка свободных — нулевой указатель
    return it;
}
