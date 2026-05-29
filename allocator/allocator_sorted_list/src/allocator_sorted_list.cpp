#include "../include/allocator_sorted_list.h" // собственный заголовок (объяснялся ранее)
#include <cstddef>                            // size_t, ptrdiff_t (объяснялся ранее)
#include <cstring>                            // std::memcpy (объяснялся ранее)
#include <stdexcept>                          // исключения (объяснялся ранее)
#include <limits>                             // std::numeric_limits (объяснялся ранее)

// ---------- вспомогательные функции для работы с заголовком аллокатора ----------

namespace { // безымянное пространство имён (объяснялось ранее)

    // вычисляем реальный размер заголовка аллокатора с учётом выравнивания max_align_t
    size_t header_size() noexcept { // обычная функция (не constexpr), вычисляющая размер заголовка аллокатора с округлением вверх до выравнивания. noexcept — не бросает исключений.
        size_t raw = sizeof(std::mutex)                            // примитив синхронизации (объяснялся ранее)
                   + sizeof(std::pmr::memory_resource*)            // указатель на родительский аллокатор (объяснялся ранее)
                   + sizeof(size_t)                                // общий размер доверенной памяти (объяснялся ранее)
                   + sizeof(void*)                                 // указатель на первый свободный блок (объяснялся ранее)
                   + sizeof(allocator_with_fit_mode::fit_mode);    // режим подбора блока (объяснялся ранее)
        size_t align = alignof(std::max_align_t);                  // получаем требование выравнивания (объяснялось ранее)
        return (raw + align - 1) & ~(align - 1);                   // округление вверх (round up) до границы выравнивания align. принцип: (raw + align - 1) сдвигает число почти на следующую границу, а & ~(align - 1) обнуляет младшие биты, соответствующие align. например, при align=16 (align-1=15, ~15=...11110000): (108+15)=123, 123 & ~15 = 112. таким образом 108 округляется до 112.
    }

    // размер метаданных одного блока (заголовок + указатель на следующий свободный)
    constexpr size_t block_meta_size() noexcept { // constexpr функция (вычисляется на этапе компиляции). возвращает размер заголовка блока: sizeof(void*) + sizeof(size_t). это константа, используемая для вычисления смещения от заголовка к полезной нагрузке и обратно.
        return sizeof(void*) + sizeof(size_t);
    }

    // получение указателя на мьютекс, расположенный по смещению 0 в доверенной памяти
    std::mutex* mutex_of(void* trusted) noexcept { // возвращает std::mutex* (указатель), а не ссылку std::mutex&, как во второй задаче. указатель нужен для вызова деструктора через ->~mutex() и для передачи в std::lock_guard.
        return reinterpret_cast<std::mutex*>(trusted); // reinterpret_cast — интерпретация памяти как мьютекс (объяснялся ранее)
    }

    // получение ссылки на указатель родительского аллокатора
    std::pmr::memory_resource*& parent_of(void* trusted) noexcept { // возвращает ссылку на указатель (std::pmr::memory_resource*&). структура полей отличается от boundary_tags: здесь нет именованных констант смещений, а адрес вычисляется прямо в теле функции через сумму sizeof.
        return *reinterpret_cast<std::pmr::memory_resource**>(
            static_cast<char*>(trusted) + sizeof(std::mutex)); // сдвигаем trusted на sizeof(std::mutex) и интерпретируем как указатель на указатель.
    }

    // получение ссылки на поле общего размера доверенной памяти
    size_t& total_size_of(void* trusted) noexcept { // total_size — общий размер доверенной области (включая заголовок аллокатора и все блоки). ссылка позволяет читать и писать.
        return *reinterpret_cast<size_t*>(
            static_cast<char*>(trusted) + sizeof(std::mutex) + sizeof(std::pmr::memory_resource*)); // сдвигаем на мьютекс + указатель родителя.
    }

    // получение ссылки на указатель первого свободного блока
    void*& first_free_of(void* trusted) noexcept { // ссылка на void* — указатель на голову explicit списка свободных блоков.
        return *reinterpret_cast<void**>(
            static_cast<char*>(trusted) + sizeof(std::mutex) + sizeof(std::pmr::memory_resource*) + sizeof(size_t)); // сдвигаем на мьютекс + родитель + размер.
    }

    // получение ссылки на режим подбора (first/best/worst fit)
    allocator_with_fit_mode::fit_mode& fit_mode_of(void* trusted) noexcept { // ссылка на enum fit_mode, хранимый в доверенной памяти.
        return *reinterpret_cast<allocator_with_fit_mode::fit_mode*>(
            static_cast<char*>(trusted) + sizeof(std::mutex) + sizeof(std::pmr::memory_resource*) + sizeof(size_t) + sizeof(void*)); // сдвигаем на все предыдущие поля.
    }

    // ---------- вспомогательные функции для работы с блоком ----------

    // размер блока (хранится в первых sizeof(size_t) байтах заголовка блока)
    size_t& blk_size(void* block) noexcept { // возвращает ссылку на size_t (size_t&), а не значение. благодаря ссылке можно писать blk_size(block) = 123; — это изменит размер прямо в памяти по адресу block.
        return *reinterpret_cast<size_t*>(block); // reinterpret_cast — интерпретируем начало блока как size_t.
    }

    // указатель на следующий свободный блок (идёт сразу за размером)
    void*& blk_next(void* block) noexcept { // возвращает ссылку на указатель (void*&). позволяет писать blk_next(block) = other; — связывать блоки в explicit списке свободных.
        return *reinterpret_cast<void**>(static_cast<char*>(block) + sizeof(size_t)); // сдвигаем на sizeof(size_t) и интерпретируем как указатель.
    }

    // получение адреса полезной нагрузки (payload) из адреса заголовка блока
    void* blk_payload(void* block) noexcept { // преобразует указатель на заголовок блока в указатель на данные пользователя (payload). сдвигаем на block_meta_size() — пропускаем служебные поля.
        return static_cast<char*>(block) + block_meta_size();
    }

    // обратное преобразование: из адреса, выданного пользователю, получаем адрес заголовка блока
    void* blk_from_payload(void* payload) noexcept { // обратная функция: из указателя, который получил пользователь от allocate, вычисляем адрес заголовка блока. вычитаем block_meta_size.
        return static_cast<char*>(payload) - block_meta_size();
    }

} // anonymous namespace

// ==================== конструкторы, деструктор, правило пяти ====================

allocator_sorted_list::allocator_sorted_list(
    size_t space_size,
    std::pmr::memory_resource *parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode)
{
    void* mem; // локальная переменная-указатель для выделенной доверенной памяти.
    if (parent_allocator == nullptr) {
        mem = ::operator new(space_size); // если родитель не указан, запрашиваем память напрямую из глобальной кучи через глобальный operator new (без выравнивания через alignof, т.к. ::operator new(size) гарантирует выравнивание под max_align_t по стандарту).
    } else {
        mem = parent_allocator->allocate(space_size, alignof(std::max_align_t)); // делегируем выделение родительскому ресурсу с явным указанием выравнивания (объяснялось ранее).
    }
    _trusted_memory = mem; // сохраняем единственное поле класса.

    parent_of(_trusted_memory) = parent_allocator;       // записываем, откуда память (nullptr или родитель).
    total_size_of(_trusted_memory) = space_size;         // фиксируем общий размер выделенной области.
    first_free_of(_trusted_memory) = nullptr;            // пока нет свободных блоков (создадим ниже).
    fit_mode_of(_trusted_memory) = allocate_fit_mode;    // записываем начальный режим размещения.

    new (mutex_of(_trusted_memory)) std::mutex();        // placement new (объяснялся ранее): создаём мьютекс по адресу, на который указывает mutex_of(_trusted_memory). в данном случае это смещение 0, т.е. прямо в начале доверенной памяти.

    size_t hdr = header_size();                          // вычисляем реальный размер заголовка аллокатора с учётом округления вверх.
    if (space_size > hdr) {                              // если после заголовка остаётся место хотя бы для одного блока...
        void* first = static_cast<char*>(_trusted_memory) + hdr; // ...вычисляем адрес первого блока: сразу за заголовком.
        first_free_of(_trusted_memory) = first;          // он пока единственный свободный — делаем его головой списка.
        blk_size(first) = space_size - hdr;              // размер блока = всё оставшееся пространство минус заголовок.
        blk_next(first) = nullptr;                       // следующего свободного нет.
    }
}

allocator_sorted_list::~allocator_sorted_list()
{
    if (_trusted_memory == nullptr) { // если объект был перемещён (move), _trusted_memory обнулён — уничтожать нечего.
        return;
    }
    std::pmr::memory_resource* parent = parent_of(_trusted_memory); // узнаём владельца памяти.
    mutex_of(_trusted_memory)->~mutex(); // явный вызов деструктора мьютекса через указатель. так как мьютекс создан placement new, его нужно уничтожать вручную. синтаксис: указатель->~тип().
    if (parent == nullptr) {
        ::operator delete(_trusted_memory); // возвращаем память в глобальную кучу.
    } else {
        parent->deallocate(_trusted_memory, total_size_of(_trusted_memory), alignof(std::max_align_t)); // возвращаем родителю с тем же размером и выравниванием (объяснялось ранее).
    }
}

allocator_sorted_list::allocator_sorted_list(const allocator_sorted_list &other)
{
    if (other._trusted_memory == nullptr) { // если источник пуст (был перемещён), создаём тоже пустой объект.
        _trusted_memory = nullptr;
        return;
    }

    size_t total = total_size_of(other._trusted_memory); // размер доверенной памяти источника.
    std::pmr::memory_resource* parent = parent_of(other._trusted_memory); // откуда выделять новую память.
    void* new_mem;
    if (parent == nullptr) {
        new_mem = ::operator new(total); // выделяем из глобальной кучи.
    } else {
        new_mem = parent->allocate(total, alignof(std::max_align_t)); // выделяем из родителя.
    }

    std::memcpy(new_mem, other._trusted_memory, total); // побайтовое копирование (std::memcpy из <cstring>). копирует total байтов из other._trusted_memory в new_mem. это копирует все блоки и их метаданные, но указатели внутри блоков (ссылки next) всё ещё указывают на старую память (other._trusted_memory) — их нужно пересчитать.

    // пересчитываем указатели свободных блоков, так как базовый адрес изменился
    void* old_base = other._trusted_memory; // старый базовый адрес.
    void* new_base = new_mem;               // новый базовый адрес.
    auto remap = [old_base, new_base](void* ptr) -> void* { // лямбда-функция (lambda). синтаксис: [захват](параметры) -> возвращаемый_тип { тело }. [old_base, new_base] — список захвата: лямбда получает доступ к локальным переменным old_base и new_base из окружающей функции по значению (копирует их внутрь себя). (void* ptr) — параметр: указатель в старой памяти. -> void* — явное указание возвращаемого типа. лямбда вычисляет новый адрес: new_base + (ptr - old_base), т.е. сохраняет смещение относительно базы.
        if (ptr == nullptr) return nullptr; // nullptr остаётся nullptr.
        return static_cast<char*>(new_base) + (static_cast<char*>(ptr) - static_cast<char*>(old_base)); // переводим указатели в char* для побайтовой арифметики: разность даёт смещение в байтах, прибавляем к new_base.
    };

    first_free_of(new_mem) = remap(first_free_of(old_base)); // пересчитываем голову списка свободных блоков.
    void* curr = first_free_of(new_mem);
    while (curr != nullptr) {
        blk_next(curr) = remap(blk_next(curr)); // пересчитываем каждый next в цепочке свободных блоков.
        curr = blk_next(curr);
    }

    new (mutex_of(new_mem)) std::mutex(); // создаём новый мьютекс в новой памяти (старый нельзя копировать).
    _trusted_memory = new_mem; // сохраняем новую доверенную память.
}

allocator_sorted_list &allocator_sorted_list::operator=(const allocator_sorted_list &other)
{
    if (this == &other) { // защита от самоприсваивания (объяснялась ранее)
        return *this;
    }

    void* old_trusted = _trusted_memory; // сохраняем старый указатель, чтобы освободить память ПОСЛЕ создания копии.

    if (other._trusted_memory == nullptr) {
        _trusted_memory = nullptr; // источник пуст.
    } else {
        size_t total = total_size_of(other._trusted_memory);
        std::pmr::memory_resource* parent = parent_of(other._trusted_memory);
        void* new_mem;
        if (parent == nullptr) {
            new_mem = ::operator new(total);
        } else {
            new_mem = parent->allocate(total, alignof(std::max_align_t));
        }

        std::memcpy(new_mem, other._trusted_memory, total); // копируем побайтно (объяснялось выше).

        void* old_base = other._trusted_memory;
        void* new_base = new_mem;
        auto remap = [old_base, new_base](void* ptr) -> void* { // лямбда-функция для пересчёта указателей (объяснялась выше).
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

    if (old_trusted != nullptr) { // освобождаем предыдущие ресурсы текущего объекта
        std::pmr::memory_resource* old_parent = parent_of(old_trusted);
        mutex_of(old_trusted)->~mutex();
        if (old_parent == nullptr) {
            ::operator delete(old_trusted);
        } else {
            old_parent->deallocate(old_trusted, total_size_of(old_trusted), alignof(std::max_align_t));
        }
    }

    return *this; // (объяснялось ранее)
}

allocator_sorted_list::allocator_sorted_list(allocator_sorted_list &&other) noexcept
    : _trusted_memory(other._trusted_memory) // забираем владение (объяснялось ранее)
{
    other._trusted_memory = nullptr;
}

allocator_sorted_list &allocator_sorted_list::operator=(allocator_sorted_list &&other) noexcept
{
    if (this == &other) { // (объяснялось ранее)
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
    std::lock_guard<std::mutex> lock(*mutex_of(_trusted_memory)); // синхронизация (объяснялась ранее). *mutex_of(...) — разыменование указателя на мьютекс.

    if (size == 0) { // нулевой запрос по стандарту должен выделить хотя бы 1 байт (иначе невозможно отличить успех от неудачи).
        size = 1;
    }

    if (size > std::numeric_limits<size_t>::max() - block_meta_size()) { // проверка на переполнение size_t при сложении size + block_meta_size(). std::numeric_limits<size_t>::max() — максимальное значение size_t. если size больше max - block_meta_size(), то size + block_meta_size() переполнится (undefined behavior). в таком случае бросаем std::bad_alloc.
        throw std::bad_alloc();
    }
    size_t needed = size + block_meta_size(); // полный размер блока = запрошенный размер + заголовок.

    void* prev_free = nullptr;          // указатель на предыдущий свободный блок при обходе списка. нужен, чтобы переподвесить указатели при удалении/разбиении блока из списка.
    void* curr_free = first_free_of(_trusted_memory); // текущий свободный блок (голова списка).
    void* best = nullptr;               // выбранный подходящий блок (результат поиска).
    void* best_prev = nullptr;          // предыдущий перед выбранным (нужен для переподвешивания).
    allocator_with_fit_mode::fit_mode mode = fit_mode_of(_trusted_memory); // текущий режим.

    while (curr_free != nullptr) { // обходим отсортированный explicit список свободных блоков. список отсортирован по возрастанию адреса: каждый следующий свободный блок лежит в памяти правее предыдущего.
        size_t curr_sz = blk_size(curr_free); // читаем размер текущего свободного блока.
        if (curr_sz >= needed) { // блок подходит по размеру?
            if (mode == allocator_with_fit_mode::fit_mode::first_fit) {
                best = curr_free;
                best_prev = prev_free;
                break; // первый подходящий найден — останавливаем поиск.
            } else if (mode == allocator_with_fit_mode::fit_mode::the_best_fit) {
                if (best == nullptr || curr_sz < blk_size(best)) { // ищем наименьший достаточный блок. если best ещё не найден ИЛИ текущий меньше лучшего — обновляем.
                    best = curr_free;
                    best_prev = prev_free;
                }
            } else { // the_worst_fit
                if (best == nullptr || curr_sz > blk_size(best)) { // ищем наибольший достаточный блок. если best ещё не найден ИЛИ текущий больше лучшего — обновляем.
                    best = curr_free;
                    best_prev = prev_free;
                }
            }
        }
        prev_free = curr_free;       // запоминаем текущий как предыдущий.
        curr_free = blk_next(curr_free); // переходим к следующему свободному блоку через указатель next.
    }

    if (best == nullptr) { // ни один блок не подошёл — память кончилась.
        throw std::bad_alloc();
    }

    size_t best_sz = blk_size(best);
    size_t remaining = best_sz - needed; // остаток после выделения needed байт из best.

    if (remaining >= block_meta_size() + 1) { // если остаток достаточен для нового свободного блока + хотя бы 1 байт полезной нагрузки (чтобы блок имел смысл)...
        void* new_free = static_cast<char*>(best) + needed; // адрес оставшегося фрагмента = best + needed.
        blk_size(new_free) = remaining;                     // размер остатка.
        blk_next(new_free) = blk_next(best);                // наследуем ссылку на следующий свободный из старого блока.

        blk_size(best) = needed;                            // фиксируем размер выделяемого блока.

        if (best_prev == nullptr) {
            first_free_of(_trusted_memory) = new_free;      // обновляем голову списка: теперь первый свободный — остаток.
        } else {
            blk_next(best_prev) = new_free;                 // переподвешиваем предыдущий: его next теперь указывает на остаток.
        }
    } else {
        // остаток слишком мал — отдаём весь блок целиком, чтобы не образовывать непригодные микро-осколки (которые нельзя будет выделить из-за размера меньше block_meta_size + 1).
        if (best_prev == nullptr) {
            first_free_of(_trusted_memory) = blk_next(best); // голова списка теперь — то, на что указывал best.
        } else {
            blk_next(best_prev) = blk_next(best);            // перепрыгиваем через best в списке.
        }
    }

    return blk_payload(best); // возвращаем указатель на полезную нагрузку (пропускаем заголовок block_meta_size).
}

void allocator_sorted_list::do_deallocate_sm(void *at)
{
    if (at == nullptr) { // освобождение nullptr — корректная no-op операция по стандарту c++. ничего не делаем.
        return;
    }

    std::lock_guard<std::mutex> lock(*mutex_of(_trusted_memory)); // синхронизация (объяснялась ранее)

    void* block = blk_from_payload(at); // преобразуем пользовательский указатель в указатель на заголовок блока (объяснялось выше).
    char* trusted_start = static_cast<char*>(_trusted_memory); // начало доверенной памяти в char* для арифметики.
    char* trusted_end = trusted_start + total_size_of(_trusted_memory); // конец доверенной памяти.
    char* block_char = static_cast<char*>(block); // заголовок блока в char*.

    // валидация: блок должен находиться внутри доверенной области, за пределами заголовка аллокатора
    if (block_char < trusted_start + static_cast<ptrdiff_t>(header_size()) || // static_cast<ptrdiff_t>(header_size()) — приводим беззнаковый size_t к знаковому ptrdiff_t. ptrdiff_t — тип, который может хранить разность двух указателей (со знаком). это нужно для корректной арифметики с указателями: trusted_start + ptrdiff_t даёт смещение в байтах. если block_char меньше адреса за заголовком — указатель не принадлежит полезной области.
        block_char + static_cast<ptrdiff_t>(block_meta_size()) > trusted_end) { // проверяем, что блок не вылезает за пределы доверенной памяти. block_char + block_meta_size() — адрес конца заголовка блока; он должен быть ≤ trusted_end.
        throw std::logic_error("deallocate: block does not belong to this allocator"); // std::logic_error — базовый класс для ошибок в логике программы (наследник std::exception). отличается от std::invalid_argument тем, что это более общая категория логических нарушений.
    }

    // проверка: указатель должен быть началом реально существующего блока (а не серединой)
    bool found = false; // флаг: нашли ли блок при линейном обходе.
    void* cur = trusted_start + header_size(); // начинаем обход с первого блока (сразу за заголовком).
    while (cur < trusted_end) {
        if (cur == block) { // если текущий блок совпадает с block — валидация пройдена.
            found = true;
            break;
        }
        size_t sz = blk_size(cur); // размер текущего блока.
        if (sz == 0 || static_cast<char*>(cur) + sz > trusted_end) { // защита от повреждённых данных: размер 0 или выход за границу.
            break;
        }
        cur = static_cast<char*>(cur) + sz; // переходим к следующему физическому блоку.
    }
    if (!found) {
        throw std::logic_error("deallocate: invalid block pointer");
    }

    // проверка на двойное освобождение: ищем блок в списке свободных
    void* check = first_free_of(_trusted_memory); // начинаем обход explicit списка свободных блоков.
    while (check != nullptr) {
        if (check == block) { // если блок уже присутствует в списке свободных...
            throw std::logic_error("deallocate: double free detected"); // ...значит пользователь пытается освободить его второй раз.
        }
        check = blk_next(check); // следующий свободный.
    }

    // вставляем освобождённый блок в отсортированный список свободных (по возрастанию адреса)
    void* prev_free = nullptr; // предыдущий свободный перед точкой вставки.
    void* curr_free = first_free_of(_trusted_memory); // текущий при обходе списка.
    while (curr_free != nullptr && curr_free < block) { // идём по списку, пока не найдём место, где curr_free ≥ block. таким образом список всегда отсортирован по возрастанию адреса — это ключевое отличие от boundary_tags, где порядок мог быть произвольным.
        prev_free = curr_free;
        curr_free = blk_next(curr_free);
    }

    blk_next(block) = curr_free; // новый блок указывает на следующий за ним (curr_free).
    if (prev_free == nullptr) {
        first_free_of(_trusted_memory) = block; // вставка в голову: новый блок становится первым свободным.
    } else {
        blk_next(prev_free) = block; // переподвешиваем prev_free: его next теперь указывает на block.
    }

    // слияние (coalescing) с правым соседом, если он свободный и примыкает вплотную
    if (curr_free != nullptr && block_char + static_cast<ptrdiff_t>(blk_size(block)) == static_cast<char*>(curr_free)) { // проверяем строгую смежность: адрес конца текущего блока (block_char + blk_size(block)) равен адресу начала следующего свободного (curr_free). static_cast<ptrdiff_t> нужен для корректного сложения signed + unsigned.
        blk_size(block) += blk_size(curr_free); // увеличиваем размер текущего блока на размер правого соседа.
        blk_next(block) = blk_next(curr_free);  // перепрыгиваем через правого соседа: next теперь указывает на того, на кого указывал правый сосед.
    }

    // слияние с левым соседом, если он свободный и примыкает вплотную
    if (prev_free != nullptr && static_cast<char*>(prev_free) + static_cast<ptrdiff_t>(blk_size(prev_free)) == block_char) { // аналогично: конец левого блока равен началу текущего?
        blk_size(prev_free) += blk_size(block); // увеличиваем размер левого блока на размер текущего.
        blk_next(prev_free) = blk_next(block);  // перепрыгиваем через текущий блок.
    }
}

bool allocator_sorted_list::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    auto p = dynamic_cast<const allocator_sorted_list*>(&other); // auto — ключевое слово автоматического вывода типа. компилятор сам подставляет тип слева от = (здесь const allocator_sorted_list*). auto упрощает код, когда тип очевиден из инициализатора и слишком длинный. dynamic_cast — безопасное приведение (объяснялось ранее).
    if (p == nullptr) {
        return false; // другой тип ресурса — не эквивалентен.
    }
    return _trusted_memory == p->_trusted_memory; // эквивалентны только при совпадении доверенной памяти (т.е. это один и тот же объект или копия, указывающая на ту же память).
}

inline void allocator_sorted_list::set_fit_mode(allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard<std::mutex> lock(*mutex_of(_trusted_memory)); // синхронизация (объяснялась ранее)
    fit_mode_of(_trusted_memory) = mode;
}

// ==================== отладочная/тестовая информация ====================

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info() const noexcept
{
    try {
        std::lock_guard<std::mutex> lock(*mutex_of(_trusted_memory)); // синхронизированный сбор информации.
        return get_blocks_info_inner(); // делегируем внутреннему методу.
    } catch (...) { // catch (...) — перехват ЛЮБОГО исключения (объяснялось ранее). поскольку метод объявлен noexcept, любое вылетевшее исключение привело бы к std::terminate (аварийному завершению). поэтому мы ловим всё внутри и возвращаем безопасное значение.
        return {}; // return {} — сокращённая запись (brace initialization). означает «вернуть пустой std::vector<block_info>». фигурные скобки {} создают временный объект по умолчанию (пустой вектор), который затем перемещается/копируется как возвращаемое значение.
    }
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info_inner() const
{
    std::vector<block_info> result; // (объяснялось ранее)
    void* current = static_cast<char*>(_trusted_memory) + header_size(); // первый блок за заголовком.
    void* end_ptr = static_cast<char*>(_trusted_memory) + total_size_of(_trusted_memory); // конец доверенной памяти.
    void* free_current = first_free_of(_trusted_memory); // указатель на текущий свободный блок при обходе.

    while (current < end_ptr) {
        size_t sz = blk_size(current); // размер блока.
        bool is_occupied = (current != free_current); // определяем занятость: если адрес текущего физического блока совпадает с адресом текущего свободного из explicit списка — блок свободен. иначе — занят. это возможно благодаря тому, что список свободных отсортирован по возрастанию адреса и совпадает с физическим порядком (после слияний).
        result.push_back({sz, is_occupied}); // агрегатная инициализация block_info (объяснялась ранее)
        if (!is_occupied) {
            free_current = blk_next(free_current); // если блок был свободен — продвигаем указатель по списку свободных.
        }
        current = static_cast<char*>(current) + sz; // переходим к следующему физическому блоку.
    }
    return result;
}

// ==================== итераторы по всем блокам ====================

allocator_sorted_list::sorted_iterator::sorted_iterator()
    : _free_ptr(nullptr), _current_ptr(nullptr), _trusted_memory(nullptr) // пустой итератор (end)
{
}

allocator_sorted_list::sorted_iterator::sorted_iterator(void* trusted)
    : _trusted_memory(trusted) // инициализируем trusted_memory
{
    _current_ptr = static_cast<char*>(trusted) + header_size(); // первый блок.
    _free_ptr = first_free_of(trusted); // первый свободный.
}

bool allocator_sorted_list::sorted_iterator::operator==(const sorted_iterator& other) const noexcept
{
    return _current_ptr == other._current_ptr; // два итератора равны, если указывают на один и тот же физический блок.
}

bool allocator_sorted_list::sorted_iterator::operator!=(const sorted_iterator& other) const noexcept
{
    return !(*this == other); // (объяснялось ранее)
}

allocator_sorted_list::sorted_iterator& allocator_sorted_list::sorted_iterator::operator++() & noexcept
{
    _current_ptr = static_cast<char*>(_current_ptr) + blk_size(_current_ptr); // сдвигаем _current_ptr на размер текущего блока — переходим к следующему физическому блоку в памяти.
    if (_current_ptr == _free_ptr) { // если новый _current_ptr совпал с _free_ptr (т.е. следующий блок свободен)...
        _free_ptr = blk_next(_free_ptr); // ...продвигаем _free_ptr дальше по explicit списку свободных.
    }
    return *this;
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::sorted_iterator::operator++(int)
{
    sorted_iterator tmp = *this; // (объяснялось ранее)
    ++(*this);
    return tmp;
}

size_t allocator_sorted_list::sorted_iterator::size() const noexcept
{
    return blk_size(_current_ptr); // размер текущего блока.
}

void* allocator_sorted_list::sorted_iterator::operator*() const noexcept
{
    return blk_payload(_current_ptr); // payload текущего блока.
}

bool allocator_sorted_list::sorted_iterator::occupied() const noexcept
{
    return _current_ptr != _free_ptr; // текущий блок занят, если его адрес не совпадает со следующим свободным из списка.
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::begin() const noexcept
{
    return sorted_iterator(_trusted_memory); // начинаем с первого блока.
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::end() const noexcept
{
    sorted_iterator it;
    it._trusted_memory = _trusted_memory;
    it._current_ptr = static_cast<char*>(_trusted_memory) + total_size_of(_trusted_memory); // end указывает за пределы памяти (на trusted_start + total_size).
    it._free_ptr = nullptr;
    return it;
}

// ==================== итераторы только по свободным блокам ====================

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator()
    : _free_ptr(nullptr) // пустой итератор (end)
{
}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator(void* trusted)
    : _free_ptr(first_free_of(trusted)) // начинаем с головы списка свободных.
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
    _free_ptr = blk_next(_free_ptr); // переходим к следующему свободному блоку через указатель next в заголовке.
    return *this;
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::sorted_free_iterator::operator++(int)
{
    sorted_free_iterator tmp = *this; // (объяснялось ранее)
    ++(*this);
    return tmp;
}

size_t allocator_sorted_list::sorted_free_iterator::size() const noexcept
{
    return blk_size(_free_ptr); // размер свободного блока.
}

void* allocator_sorted_list::sorted_free_iterator::operator*() const noexcept
{
    return blk_payload(_free_ptr); // payload свободного блока.
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_begin() const noexcept
{
    return sorted_free_iterator(_trusted_memory); // голова списка свободных.
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_end() const noexcept
{
    sorted_free_iterator it;
    it._free_ptr = nullptr; // конец списка — нулевой указатель.
    return it;
}
