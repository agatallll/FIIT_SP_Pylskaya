#include <not_implemented.h> // заголовок с классом not_implemented — исключением, которое бросается для функций, которые сознательно не реализованы (например, копирование). наследуется от std::logic_error.
#include "../include/allocator_boundary_tags.h" // собственный заголовок (объяснялся ранее)
#include <mutex>          // примитив синхронизации (объяснялся ранее)
#include <memory_resource> // стандартный заголовок для std::pmr::memory_resource (объяснялся ранее)
#include <stdexcept>      // стандартные исключения (объяснялся ранее)
#include <cstddef>        // заголовок c-style с определением size_t, ptrdiff_t и nullptr_t. <cstddef> — c++-ная версия c-заголовка <stddef.h>, все имена находятся в пространстве имён std.
#include <cstdint>        // заголовок c-style с точными целочисленными типами (uint32_t, int64_t и т.д.). <cstdint> — c++-ная обёртка над <stdint.h>.
#include <vector>         // заголовок с шаблонным классом std::vector — динамический массив, который сам управляет памятью: автоматически расширяется при добавлении элементов и освобождает память при уничтожении.
#include <cstring>        // заголовок c-style для работы с c-строками и блоками памяти. содержит std::memcpy — функцию побайтового копирования памяти без перекрытия.

namespace { // объявление безымянного (анонимного) пространства имён. всё, что объявлено внутри, видно только в этом файле (единице трансляции). это позволяет скрыть вспомогательные функции и переменные от других .cpp файлов, избегая конфликтов имён (аналог static для глобальных переменных в c).
    // вычисляем размер метаданных аллокатора вручную (совпадает с allocator_metadata_size)
    constexpr size_t allocator_metadata_size_local = sizeof(std::mutex)
                                                   + sizeof(std::pmr::memory_resource*)
                                                   + sizeof(size_t)
                                                   + sizeof(void*)
                                                   + sizeof(allocator_with_fit_mode::fit_mode); // constexpr — константное выражение, вычисляемое на этапе компиляции. здесь складываем sizeof всех полей, которые хранятся в начале доверенной памяти: мьютекс, указатель на родителя, размер области, указатель на первый свободный блок, режим размещения. переменная с суффиксом _local — локальная для этого файла.

    // смещения полей метаданных аллокатора в доверенной памяти
    constexpr size_t mtx_offset = 0; // смещение мьютекса от начала доверенной памяти — 0 байт, т.е. мьютекс лежит прямо в начале.
    constexpr size_t parent_offset = sizeof(std::mutex); // смещение указателя на родительский аллокатор. равно размеру мьютекса, т.к. идёт сразу после него.
    constexpr size_t space_size_offset = parent_offset + sizeof(std::pmr::memory_resource*); // смещение поля space_size (размер доверенной области). складываем предыдущее смещение и размер указателя.
    constexpr size_t first_free_offset = space_size_offset + sizeof(size_t); // смещение указателя на первый свободный блок. идёт после space_size.
    constexpr size_t mode_offset = first_free_offset + sizeof(void*); // смещение поля fit_mode. идёт после указателя на первый свободный блок.

    // доступ к полям метаданных аллокатора
    std::mutex& get_mtx(void* trusted) noexcept { // функция возвращает ссылку (std::mutex&) на мьютекс, лежащий в доверенной памяти. trusted — указатель на начало доверенной области.
        return *reinterpret_cast<std::mutex*>(static_cast<char*>(trusted) + mtx_offset); // static_cast<char*>(trusted) — приводим void* к char*, чтобы иметь возможность арифметики указателей в байтах (в c++ арифметика с char* идёт побайтово). + mtx_offset — сдвигаемся на нужное число байт. reinterpret_cast<std::mutex*> — самое «сильное» приведение типов в c++. оно говорит компилятору: «возьми сырой указатель и интерпретируй память по этому адресу как объект типа std::mutex». reinterpret_cast не проверяет корректность адреса и не вызывает конструкторов; он просто меняет тип указателя. далее разыменовываем * и получаем ссылку на мьютекс.
    }
    std::pmr::memory_resource*& get_parent(void* trusted) noexcept { // возвращает ссылку на указатель (std::pmr::memory_resource*&) на родительский аллокатор. & после * означает ссылку на указатель: мы можем не только читать указатель, но и изменять его.
        return *reinterpret_cast<std::pmr::memory_resource**>(static_cast<char*>(trusted) + parent_offset); // приводим к std::pmr::memory_resource** — указатель на указатель. reinterpret_cast интерпретирует байты в памяти как указатель. разыменование даёт ссылку на сам указатель.
    }
    size_t& get_space_size(void* trusted) noexcept { // возвращает ссылку на size_t внутри доверенной памяти — размер доступной области.
        return *reinterpret_cast<size_t*>(static_cast<char*>(trusted) + space_size_offset); // reinterpret_cast<size_t*> — интерпретируем байты по адресу trusted + space_size_offset как size_t. ссылка позволяет читать и писать это поле.
    }
    void*& get_first_free(void* trusted) noexcept { // возвращает ссылку на void* — указатель на первый свободный блок в явном списке свободных.
        return *reinterpret_cast<void**>(static_cast<char*>(trusted) + first_free_offset); // void** — указатель на указатель void*. reinterpret_cast позволяет обращаться к сырым байтам как к указателю.
    }
    allocator_with_fit_mode::fit_mode& get_mode(void* trusted) noexcept { // возвращает ссылку на режим размещения (fit_mode), хранимый в доверенной памяти.
        return *reinterpret_cast<allocator_with_fit_mode::fit_mode*>(static_cast<char*>(trusted) + mode_offset); // reinterpret_cast приводит указатель к fit_mode*. так мы читаем/пишем enum прямо по заданному адресу в памяти.
    }

    // начало области блоков (ровно за метаданными аллокатора)
    void* region_start(void* trusted) noexcept { // функция вычисляет адрес первого байта, который может быть отдан пользователю (сразу после метаданных аллокатора).
        return static_cast<char*>(trusted) + allocator_metadata_size_local; // сдвигаем trusted на размер метаданных. static_cast<char*> нужен для побайтовой арифметики: trusted + n байт.
    }

    // безопасное чтение/запись size_t и void* через memcpy
    // (адреса блоков могут быть невыровненными, т.к. allocator_metadata_size = 108)
    size_t read_size_t(void* addr) noexcept { // функция читает значение типа size_t по адресу addr через std::memcpy. почему не просто *reinterpret_cast<size_t*>(addr)? потому что addr может быть невыровнен (not aligned) относительно требований процессора для size_t. на некоторых архитектурах чтение невыровненного указателя вызывает segfault или undefined behavior. memcpy всегда безопасен, т.к. работает побайтово и не требует выравнивания.
        size_t val; // локальная переменная на стеке, выровненная автоматически.
        std::memcpy(&val, addr, sizeof(val)); // std::memcpy — функция из <cstring>, копирующая sizeof(val) байтов из памяти по адресу addr в память по адресу &val. копирование идёт побайтово, без учёта типов, поэтому безопасно для любых адресов.
        return val; // возвращаем прочитанное значение.
    }
    void write_size_t(void* addr, size_t val) noexcept { // запись size_t по невыровненному адресу через memcpy.
        std::memcpy(addr, &val, sizeof(val)); // копируем sizeof(val) байтов из &val в addr. addr может быть невыровнен — memcpy справится.
    }
    void* read_ptr(void* addr) noexcept { // чтение указателя void* по невыровненному адресу.
        void* val;
        std::memcpy(&val, addr, sizeof(val));
        return val;
    }
    void write_ptr(void* addr, void* val) noexcept { // запись указателя void* по невыровненному адресу.
        std::memcpy(addr, &val, sizeof(val));
    }

    // размер блока из заголовка (без младшего бита)
    size_t blk_size(void* block) noexcept { // функция извлекает размер блока из его заголовка. в boundary tags заголовок — это size_t, где младший бит используется как флаг занятости.
        return read_size_t(block) & ~static_cast<size_t>(1); // read_size_t(block) читает size_t из начала блока. static_cast<size_t>(1) — приводим 1 к типу size_t. ~ — побитовое НЕ (инверсия всех битов). ~1 даёт маску, у которой все биты 1, кроме младшего. оператор & — побитовое И. таким образом мы обнуляем младший бит, получая чистый размер блока без флага занятости.
    }
    // флаг занятости из заголовка
    bool blk_occupied(void* block) noexcept { // функция проверяет, занят ли блок, читая младший бит заголовка.
        return (read_size_t(block) & 1) != 0; // read_size_t(block) & 1 — побитовое И с 1. если младший бит установлен (1), результат 1 — блок занят. если сброшен (0) — свободен. != 0 преобразует целочисленный результат в bool.
    }
    // установить заголовок блока
    void set_blk(void* block, size_t sz, bool occ) noexcept { // функция записывает заголовок блока: размер sz и флаг занятости occ.
        write_size_t(block, occ ? (sz | 1) : sz); // тернарный оператор ? : . если occ == true, вычисляем sz | 1 — побитовое ИЛИ с 1, что устанавливает младший бит в 1 (блок занят). если occ == false, записываем просто sz (младший бит 0 — блок свободен).
    }

    // чтение/запись указателей внутри блока
    void* blk_prev(void* block) noexcept { // читает указатель на предыдущий блок из метаданных занятого блока. хранится по смещению sizeof(size_t) от начала блока.
        return read_ptr(static_cast<char*>(block) + sizeof(size_t)); // сдвигаемся на sizeof(size_t) байт вперёд (пропускаем заголовок) и читаем указатель.
    }
    void set_blk_prev(void* block, void* val) noexcept { // записывает указатель на предыдущий блок в метаданные занятого блока.
        write_ptr(static_cast<char*>(block) + sizeof(size_t), val); // пишем указатель val по смещению sizeof(size_t) от начала блока.
    }
    void* blk_next(void* block) noexcept { // читает указатель на следующий блок.
        return read_ptr(static_cast<char*>(block) + sizeof(size_t) + sizeof(void*)); // сдвигаемся на sizeof(size_t) + sizeof(void*), пропуская заголовок и указатель prev.
    }
    void set_blk_next(void* block, void* val) noexcept { // записывает указатель на следующий блок.
        write_ptr(static_cast<char*>(block) + sizeof(size_t) + sizeof(void*), val);
    }
    void* blk_parent(void* block) noexcept { // читает указатель на родительский аллокатор (дескриптор границы — boundary tag). нужен для проверки принадлежности блока аллокатору при освобождении.
        return read_ptr(static_cast<char*>(block) + sizeof(size_t) + 2 * sizeof(void*)); // сдвигаемся на sizeof(size_t) + 2*sizeof(void*), пропуская prev и next.
    }
    void set_blk_parent(void* block, void* val) noexcept { // записывает указатель на родительский аллокатор.
        write_ptr(static_cast<char*>(block) + sizeof(size_t) + 2 * sizeof(void*), val);
    }

    // указатели prev/next для явного списка свободных блоков (хранятся в payload)
    void* free_prev(void* block) noexcept { // читает указатель на предыдущий свободный блок из явного списка свободных. для свободного блока указатели списка хранятся в той же памяти, где у занятого блока лежат prev/next/parent, но семантика другая: это не соседи в памяти, а соседи в списке.
        return read_ptr(static_cast<char*>(block) + sizeof(size_t)); // читаем по смещению sizeof(size_t) — сразу после заголовка.
    }
    void set_free_prev(void* block, void* val) noexcept { // записывает указатель на предыдущий свободный блок в списке.
        write_ptr(static_cast<char*>(block) + sizeof(size_t), val);
    }
    void* free_next(void* block) noexcept { // читает указатель на следующий свободный блок в списке.
        return read_ptr(static_cast<char*>(block) + sizeof(size_t) + sizeof(void*)); // сдвигаемся на sizeof(size_t) + sizeof(void*).
    }
    void set_free_next(void* block, void* val) noexcept { // записывает указатель на следующий свободный блок в списке.
        write_ptr(static_cast<char*>(block) + sizeof(size_t) + sizeof(void*), val);
    }

    // вставить блок в начало списка свободных
    void insert_free(void* trusted, void* block) noexcept { // функция добавляет свободный block в голову односвязного/двусвязного списка свободных блоков. trusted — указатель на доверенную память, нужен, чтобы получить ссылку на first_free.
        void*& first = get_first_free(trusted); // получаем ссылку на указатель первого свободного блока. void*& — ссылка на указатель, благодаря этому изменения first отразятся прямо в доверенной памяти.
        set_free_prev(block, nullptr); // у нового блока prev = nullptr (он станет первым).
        set_free_next(block, first);   // next нового блока указывает на бывший первый.
        if (first != nullptr) {        // если список был не пуст...
            set_free_prev(first, block); // ...у бывшего первого prev теперь указывает на новый блок.
        }
        first = block; // обновляем голову списка: first теперь указывает на новый блок.
    }

    // удалить блок из списка свободных
    void remove_free(void* trusted, void* block) noexcept { // функция исключает block из списка свободных. нужна при выделении блока (он становится занятым) или при слиянии с соседом.
        void* prev = free_prev(block); // читаем указатель на предыдущий свободный блок в списке.
        void* next = free_next(block); // читаем указатель на следующий свободный блок в списке.
        if (prev != nullptr) { // если есть предыдущий...
            set_free_next(prev, next); // ...его next теперь ссылается на next (перепрыгиваем через block).
        } else { // если prev == nullptr, значит block — голова списка.
            get_first_free(trusted) = next; // обновляем first_free: теперь голова — next.
        }
        if (next != nullptr) { // если есть следующий...
            set_free_prev(next, prev); // ...его prev теперь ссылается на prev (перепрыгиваем назад).
        }
    }
}

// ==================== правило пяти ====================

allocator_boundary_tags::~allocator_boundary_tags()
{
    if (_trusted_memory == nullptr) { // если _trusted_memory == nullptr, значит объект был перемещён (move) или ещё не инициализирован. уничтожать нечего.
        return;
    }
    std::pmr::memory_resource* parent = get_parent(_trusted_memory); // получаем указатель на родительский аллокатор, из которого запрашивалась доверенная память.
    size_t total = get_space_size(_trusted_memory) + allocator_metadata_size_local; // общий размер выделенного блока = пользовательская область + метаданные аллокатора.
    get_mtx(_trusted_memory).~mutex(); // явный вызов деструктора мьютекса. мьютекс размещён в доверенной памяти через placement new (вручную), поэтому его нужно вручуню и уничтожать — вызывать деструктор напрямую. синтаксис: указатель->~тип().
    if (parent != nullptr) { // если родительский аллокатор был указан...
        parent->deallocate(_trusted_memory, total, alignof(std::max_align_t)); // ...возвращаем память через родителя. alignof(std::max_align_t) — оператор alignof возвращает требование к выравниванию для типа. std::max_align_t — специальный тип, который имеет самое строгое выравнивание из всех скалярных типов (обычно 16 байт). так мы гарантируем, что память выделялась и освобождается с одинаковым выравниванием.
    } else {
        ::operator delete(_trusted_memory, total, std::align_val_t{alignof(std::max_align_t)}); // если родителя не было — используем глобальный operator delete с размером и выравниванием. std::align_val_t — специальный тип из <new>, оборачивающий значение выравнивания. синтаксис std::align_val_t{...} — инициализация через фигурные скобки (uniform initialization). эта перегрузка delete появилась в c++17 и позволяет освобождать память с учётом выравнивания, соответствуя placement new.
    }
}

allocator_boundary_tags::allocator_boundary_tags(
    allocator_boundary_tags const &other)
{
    throw not_implemented("allocator_boundary_tags::allocator_boundary_tags(const allocator_boundary_tags &)", "copying is not supported"); // бросаем исключение not_implemented. это класс-наследник std::logic_error (логическая ошибка: программист попытался использовать неподдерживаемую операцию). первый аргумент — имя метода, второй — пояснение.
}

allocator_boundary_tags &allocator_boundary_tags::operator=(
    allocator_boundary_tags const &other)
{
    throw not_implemented("allocator_boundary_tags::operator=(const allocator_boundary_tags &)", "copying is not supported"); // аналогично: копирующее присваивание не поддерживается.
}

allocator_boundary_tags::allocator_boundary_tags(
    allocator_boundary_tags &&other) noexcept
    : _trusted_memory(other._trusted_memory) // конструктор перемещения. забираем указатель на доверенную память у other.
{
    other._trusted_memory = nullptr; // обнуляем other, чтобы его деструктор не освободил память повторно.
}

allocator_boundary_tags &allocator_boundary_tags::operator=(
    allocator_boundary_tags &&other) noexcept
{
    if (this != &other) { // защита от самоперемещения (объяснялась ранее)
        if (_trusted_memory != nullptr) { // если текущий объект владеет памятью...
            std::pmr::memory_resource* parent = get_parent(_trusted_memory);
            size_t total = get_space_size(_trusted_memory) + allocator_metadata_size_local;
            get_mtx(_trusted_memory).~mutex(); // явно уничтожаем мьютекс текущего объекта.
            if (parent != nullptr) {
                parent->deallocate(_trusted_memory, total, alignof(std::max_align_t)); // возвращаем старую память родителю.
            } else {
                ::operator delete(_trusted_memory, total, std::align_val_t{alignof(std::max_align_t)}); // или в глобальную кучу.
            }
        }
        _trusted_memory = other._trusted_memory; // забираем ресурс у other.
        other._trusted_memory = nullptr; // обнуляем other.
    }
    return *this; // возвращаем ссылку на текущий объект (объяснялось ранее)
}

// ==================== конструктор ====================

allocator_boundary_tags::allocator_boundary_tags(
        size_t space_size,
        std::pmr::memory_resource *parent_allocator,
        allocator_with_fit_mode::fit_mode allocate_fit_mode)
    : _trusted_memory(nullptr) // инициализируем _trusted_memory нулём до начала работы.
{
    if (parent_allocator == nullptr) { // если родитель не передан...
        parent_allocator = std::pmr::get_default_resource(); // ...получаем глобальный ресурс памяти по умолчанию. std::pmr::get_default_resource() — функция из <memory_resource>, возвращающая указатель на глобальный memory_resource (обычно обёртку над new/delete).
    }
    _trusted_memory = parent_allocator->allocate(space_size + allocator_metadata_size_local, alignof(std::max_align_t)); // запрашиваем у родителя память под пользовательскую область + метаданные аллокатора. alignof(std::max_align_t) — выравнивание по самому строгому типу, чтобы внутри можно было размещать любые объекты (включая мьютекс и указатели).

    new (static_cast<char*>(_trusted_memory) + mtx_offset) std::mutex(); // placement new (размещающий new). синтаксис: new (адрес) тип(аргументы). в отличие от обычного new, он не выделяет память, а создаёт объект по уже готовому адресу. здесь мы вручную вызываем конструктор std::mutex по адресу _trusted_memory + mtx_offset. это нужно, потому that мьютекс лежит внутри «сырой» памяти, выделенной как байты.
    get_parent(_trusted_memory) = parent_allocator; // записываем указатель на родителя в доверенную память.
    get_space_size(_trusted_memory) = space_size;   // записываем размер пользовательской области.
    get_first_free(_trusted_memory) = nullptr;      // пока нет свободных блоков (скоро создадим).
    get_mode(_trusted_memory) = allocate_fit_mode;  // записываем режим размещения (first_fit, best_fit или worst_fit).

    void* first_block = region_start(_trusted_memory); // вычисляем адрес первого блока — сразу за метаданными аллокатора.
    set_blk(first_block, space_size, false); // записываем заголовок: размер = space_size, занятость = false (свободен).
    set_free_prev(first_block, nullptr); // prev в списке свободных = nullptr.
    set_free_next(first_block, nullptr); // next в списке свободных = nullptr.
    insert_free(_trusted_memory, first_block); // добавляем единственный большой свободный блок в список свободных.
}

// ==================== allocate / deallocate ====================

[[nodiscard]] void *allocator_boundary_tags::do_allocate_sm(
    size_t size)
{
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory)); // захват мьютекса (объяснялся ранее)

    size_t needed = size + occupied_block_metadata_size; // общий размер, который нужен под пользовательские данные + служебные поля занятого блока (заголовок size_t, prev, next, parent).
    void* best = nullptr; // указатель на выбранный свободный блок. изначально nullptr — «не найден».
    size_t best_sz = 0;   // размер выбранного блока. нужен для сравнения в режимах best_fit и worst_fit.

    for (void* curr = get_first_free(_trusted_memory); curr != nullptr; curr = free_next(curr)) { // цикл обхода явного списка свободных блоков. curr = get_first_free(...) — начинаем с головы. условие curr != nullptr — пока не конец списка. curr = free_next(curr) — переходим к следующему свободному блоку через указатель next.
        size_t curr_sz = blk_size(curr); // читаем размер текущего свободного блока (из заголовка).
        if (curr_sz < needed) { // если блок слишком мал для запрошенного размера...
            continue; // ...пропускаем его и идём дальше. continue — немедленный переход к следующей итерации цикла.
        }
        switch (get_mode(_trusted_memory)) { // switch — оператор многонаправленного ветвления. проверяем текущий режим размещения.
            case fit_mode::first_fit: // case — метка ветви. first_fit: первый подходящий блок.
                best = curr; // выбираем текущий блок.
                best_sz = curr_sz; // запоминаем его размер.
                curr = nullptr; // пометка для выхода из цикла: присваиваем curr nullptr, чтобы условие curr != nullptr в for стало ложным на следующей проверке (но сначала выполнится if ниже).
                break; // выход из switch. break здесь прерывает только switch, не цикл (цикл прервётся через проверку curr == nullptr ниже).
            case fit_mode::the_best_fit: // the_best_fit: наименьший подходящий блок (минимум остатка).
                if (best == nullptr || curr_sz < best_sz) { // если ещё ничего не нашли ИЛИ текущий блок меньше лучшего найденного...
                    best = curr; // ...обновляем лучший.
                    best_sz = curr_sz;
                }
                break;
            case fit_mode::the_worst_fit: // the_worst_fit: наибольший подходящий блок (максимум остатка).
                if (best == nullptr || curr_sz > best_sz) { // если ещё ничего не нашли ИЛИ текущий блок больше лучшего найденного...
                    best = curr; // ...обновляем лучший.
                    best_sz = curr_sz;
                }
                break;
        }
        if (curr == nullptr) { // если в first_fit мы поставили curr = nullptr для выхода...
            break; // ...прерываем цикл for полностью.
        }
    }

    if (best == nullptr) { // если после обхода списка best всё ещё nullptr — значит, ни один свободный блок не подошёл.
        throw std::bad_alloc(); // бросаем стандартное исключение нехватки памяти.
    }

    remove_free(_trusted_memory, best); // исключаем выбранный блок из списка свободных: он становится занятым.
    size_t remaining = best_sz - needed; // вычисляем, сколько байтов останется свободными после выделения needed байт из best.
    if (remaining >= occupied_block_metadata_size) { // если остаток достаточно большой, чтобы вместить метаданные нового свободного блока...
        // разбиваем блок
        void* new_free = static_cast<char*>(best) + needed; // адрес нового свободного блока = best + needed (сразу после занятой части).
        set_blk(new_free, remaining, false); // записываем заголовок нового блока: размер = remaining, занятость = false.
        set_free_prev(new_free, nullptr); // инициализируем указатели списка свободных.
        set_free_next(new_free, nullptr);
        insert_free(_trusted_memory, new_free); // добавляем остаток в список свободных блоков.

        set_blk(best, needed, true); // обновляем заголовок выделенного блока: размер = needed, занятость = true.
    } else {
        // занимаем весь блок
        set_blk(best, best_sz, true); // остаток слишком мал для нового блока (внутренняя фрагментация), поэтому отдаём пользователю весь best_sz, включая «хвост».
    }

    set_blk_prev(best, nullptr); // инициализируем дескрипторы границ (boundary tags) для занятого блока.
    set_blk_next(best, nullptr); // prev и next пока не используются (можно было бы хранить соседей для быстрого слияния, но здесь они обнуляются).
    set_blk_parent(best, get_parent(_trusted_memory)); // parent указывает на родительский аллокатор — это дескриптор границы, который поможет проверить принадлежность блока при освобождении.

    return static_cast<char*>(best) + occupied_block_metadata_size; // возвращаем пользователю указатель на полезную нагрузку (payload), т.е. пропускаем служебные метаданные занятого блока.
}

void allocator_boundary_tags::do_deallocate_sm(
    void *at)
{
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory)); // захват мьютекса (объяснялся ранее)

    void* reg_start = region_start(_trusted_memory); // адрес начала пользовательской области (сразу за метаданными аллокатора).
    void* reg_end = static_cast<char*>(reg_start) + get_space_size(_trusted_memory); // адрес конца пользовательской области = начало + размер.

    void* block_start = static_cast<char*>(at) - occupied_block_metadata_size; // вычисляем адрес начала блока по адресу, полученному от пользователя. at указывает на payload, а метаданные занимают occupied_block_metadata_size байт перед ним.

    // валидация: block_start должен совпадать с началом одного из блоков
    void* curr = reg_start; // начинаем линейный обход всех блоков от начала региона.
    while (curr < reg_end) { // пока не дошли до конца...
        if (curr == block_start) { // если нашли блок, начинающийся ровно с block_start...
            break; // ...валидация пройдена, прерываем поиск.
        }
        size_t sz = blk_size(curr); // читаем размер текущего блока.
        if (sz == 0 || curr > block_start) { // если размер 0 (повреждённая структура) или мы перепрыгнули block_start (значит, block_start указывает в середину блока)...
            throw std::invalid_argument("invalid pointer to deallocate"); // ...бросаем исключение: указатель не является началом выделенного блока.
        }
        curr = static_cast<char*>(curr) + sz; // переходим к следующему блоку: curr += размер текущего блока.
    }
    if (curr != block_start || curr >= reg_end) { // если после цикла curr не равен block_start (не нашли) или вышли за границу...
        throw std::invalid_argument("invalid pointer to deallocate"); // ...указатель не принадлежит этому аллокатору.
    }
    if (!blk_occupied(block_start)) { // если блок уже свободен (младший бит заголовка = 0)...
        throw std::invalid_argument("double free or corruption"); // ...это повторное освобождение (double free) или повреждение памяти.
    }

    size_t sz = blk_size(block_start); // запоминаем размер освобождаемого блока.

    // слияние со следующим блоком
    void* next = static_cast<char*>(block_start) + sz; // адрес блока, следующего за освобождённым в памяти.
    if (next < reg_end && !blk_occupied(next)) { // если следующий блок внутри региона И он свободен...
        size_t next_sz = blk_size(next); // ...читаем его размер.
        remove_free(_trusted_memory, next); // удаляем следующий блок из списка свободных (он скоро объединится с текущим).
        sz += next_sz; // увеличиваем размер текущего блока на размер следующего — физическое слияние (coalesce) в памяти.
    }

    // слияние с предыдущим блоком (линейный поиск с начала)
    void* prev = nullptr; // указатель на предыдущий блок. изначально неизвестен.
    curr = reg_start; // начинаем обход с начала региона.
    while (curr < block_start) { // идём блок за блоком, пока не дойдём до block_start.
        prev = curr; // запоминаем текущий как потенциальный предыдущий.
        size_t prev_sz = blk_size(curr); // читаем его размер.
        if (prev_sz == 0) { // защита от повреждённой структуры.
            throw std::invalid_argument("corrupted block structure");
        }
        curr = static_cast<char*>(curr) + prev_sz; // переходим к следующему блоку.
    }
    if (prev != nullptr && !blk_occupied(prev)) { // если нашли предыдущий блок И он свободен...
        size_t prev_sz = blk_size(prev); // ...читаем его размер.
        remove_free(_trusted_memory, prev); // удаляем предыдущий из списка свободных.
        sz += prev_sz; // увеличиваем размер на размер предыдущего — слияние назад.
        block_start = prev; // начало объединённого блока теперь совпадает с началом предыдущего.
    }

    set_blk(block_start, sz, false); // записываем новый заголовок: обновлённый размер после слияний, занятость = false (свободен).
    set_free_prev(block_start, nullptr); // очищаем указатели списка свободных перед вставкой.
    set_free_next(block_start, nullptr);
    insert_free(_trusted_memory, block_start); // добавляем объединённый свободный блок в список свободных.
}

// ==================== вспомогательные методы ====================

void allocator_boundary_tags::set_fit_mode(
    allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory)); // захват мьютекса (объяснялся ранее)
    get_mode(_trusted_memory) = mode; // записываем новый режим размещения прямо в доверенную память.
}

bool allocator_boundary_tags::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return this == &other; // сравниваем адреса объектов. два allocator_boundary_tags эквивалентны только если это один и тот же объект (у каждого своя доверенная память).
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info() const
{
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory)); // синхронизированная обёртка: захватываем мьютекс, чтобы другие потоки не меняли структуру блоков во время обхода.
    return get_blocks_info_inner(); // делегируем реальную работу внутреннему методу без синхронизации, чтобы избежать deadlock (повторного захвата мьютекса).
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info_inner() const
{
    std::vector<block_info> result; // создаём локальный вектор (динамический массив) для хранения результата. std::vector автоматически управляет памятью: при необходимости перевыделяет внутренний буфер.
    void* curr = region_start(_trusted_memory); // начинаем с первого блока в пользовательской области.
    void* end = static_cast<char*>(curr) + get_space_size(_trusted_memory); // вычисляем адрес конца региона.
    while (curr < end) { // пока не дошли до конца...
        size_t sz = blk_size(curr); // читаем размер блока.
        bool occ = blk_occupied(curr); // читаем флаг занятости.
        result.push_back({sz, occ}); // push_back — метод std::vector, добавляющий элемент в конец массива. если внутренний буфер заполнен, vector автоматически выделит новый, больший буфер и скопирует/переместит старые данные. здесь используется агрегатная инициализация (aggregate initialization): создаётся временный block_info{sz, occ} и добавляется в вектор.
        curr = static_cast<char*>(curr) + sz; // переходим к следующему блоку.
    }
    return result; // возвращаем вектор по значению. современные компиляторы (c++11 и новее) применят оптимизацию return value optimization (RVO) или перемещение, чтобы избежать лишнего копирования.
}

// ==================== итераторы ====================

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::begin() const noexcept
{
    return boundary_iterator(_trusted_memory); // возвращаем итератор, указывающий на первый блок (конструктор от _trusted_memory сам найдёт первый блок).
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::end() const noexcept
{
    return boundary_iterator(); // возвращаем пустой итератор (конструктор по умолчанию), обозначающий конец последовательности.
}

// --- boundary_iterator ---

allocator_boundary_tags::boundary_iterator::boundary_iterator()
    : _occupied_ptr(nullptr), _occupied(false), _trusted_memory(nullptr) // инициализация полей списком. nullptr — нулевой указатель. этот объект представляет «конец» (end) или невалидный итератор.
{
}

allocator_boundary_tags::boundary_iterator::boundary_iterator(void* trusted)
    : _occupied_ptr(nullptr), _occupied(false), _trusted_memory(trusted)
{
    if (trusted == nullptr) { // если trusted пустой — итератор считается конечным.
        return;
    }
    void* first = region_start(trusted); // адрес первого блока в памяти.
    void* end = static_cast<char*>(first) + get_space_size(trusted); // адрес конца региона.
    if (first < end) { // если в регионе есть хоть один байт...
        _occupied = blk_occupied(first); // ...определяем, занят ли первый блок.
        _occupied_ptr = _occupied // тернарный оператор: если блок занят...
            ? static_cast<char*>(first) + allocator_boundary_tags::occupied_block_metadata_size // ..._occupied_ptr указывает на payload после метаданных занятого блока.
            : static_cast<char*>(first) + sizeof(size_t); // ...иначе указывает после заголовка size_t (у свободного блока нет prev/next/parent).
    } else { // если регион пуст...
        _trusted_memory = nullptr; // ...делаем итератор конечным.
    }
}

bool allocator_boundary_tags::boundary_iterator::operator==(
        const boundary_iterator& other) const noexcept
{
    return _occupied_ptr == other._occupied_ptr && _occupied == other._occupied && _trusted_memory == other._trusted_memory; // два итератора равны, если все их поля совпадают. логическое && (И): все три условия должны быть истинны.
}

bool allocator_boundary_tags::boundary_iterator::operator!=(
        const boundary_iterator& other) const noexcept
{
    return !(*this == other); // используем уже определённый operator== и инвертируем результат оператором ! (логическое НЕ).
}

allocator_boundary_tags::boundary_iterator& allocator_boundary_tags::boundary_iterator::operator++() & noexcept
{
    if (_occupied_ptr == nullptr || _trusted_memory == nullptr) { // если итератор уже конечный — ничего не делаем.
        return *this;
    }
    void* block_start = _occupied // тернарный оператор: вычисляем адрес начала блока по _occupied_ptr.
        ? static_cast<char*>(_occupied_ptr) - allocator_boundary_tags::occupied_block_metadata_size // если занят — отступаем назад на размер метаданных.
        : static_cast<char*>(_occupied_ptr) - sizeof(size_t); // если свободен — отступаем на заголовок.
    size_t sz = blk_size(block_start); // читаем размер блока.
    void* next_block = static_cast<char*>(block_start) + sz; // адрес следующего блока в памяти.
    void* end = static_cast<char*>(region_start(_trusted_memory)) + get_space_size(_trusted_memory); // конец региона.
    if (next_block >= end) { // если следующий блок за пределами региона...
        *this = boundary_iterator(); // ...итератор становится конечным (пустым).
    } else {
        _occupied = blk_occupied(next_block); // определяем занятость следующего блока.
        _occupied_ptr = _occupied // вычисляем payload для следующего блока.
            ? static_cast<char*>(next_block) + allocator_boundary_tags::occupied_block_metadata_size
            : static_cast<char*>(next_block) + sizeof(size_t);
    }
    return *this; // возвращаем ссылку на себя (префиксный инкремент).
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator++(int)
{
    boundary_iterator tmp = *this; // создаём копию текущего состояния (до изменения).
    ++(*this); // вызываем префиксный инкремент ++it, который реально сдвигает итератор.
    return tmp; // возвращаем старую копию. именно поэтому постфиксный инкремент менее эффективен: создаётся временная копия.
}

allocator_boundary_tags::boundary_iterator& allocator_boundary_tags::boundary_iterator::operator--() & noexcept
{
    if (_occupied_ptr == nullptr || _trusted_memory == nullptr) { // если конечный — не двигаемся.
        return *this;
    }
    void* block_start = _occupied // вычисляем начало текущего блока.
        ? static_cast<char*>(_occupied_ptr) - allocator_boundary_tags::occupied_block_metadata_size
        : static_cast<char*>(_occupied_ptr) - sizeof(size_t);
    void* reg_start = region_start(_trusted_memory); // начало региона.
    if (block_start <= reg_start) { // если текущий блок — первый, назад идти некуда.
        *this = boundary_iterator(); // делаем конечным.
        return *this;
    }
    void* prev_block = reg_start; // начинаем линейный поиск предыдущего блока с начала региона.
    while (true) { // бесконечный цикл (выход через break).
        size_t prev_sz = blk_size(prev_block); // размер текущего кандидата.
        void* next = static_cast<char*>(prev_block) + prev_sz; // адрес блока за кандидатом.
        if (next >= block_start) { // если next достиг или перепрыгнул block_start...
            break; // ...значит prev_block — искомый предыдущий блок.
        }
        prev_block = next; // иначе двигаемся дальше.
    }
    _occupied = blk_occupied(prev_block); // определяем занятость найденного предыдущего блока.
    _occupied_ptr = _occupied // вычисляем payload.
        ? static_cast<char*>(prev_block) + allocator_boundary_tags::occupied_block_metadata_size
        : static_cast<char*>(prev_block) + sizeof(size_t);
    return *this;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator--(int)
{
    boundary_iterator tmp = *this; // сохраняем текущее состояние.
    --(*this); // сдвигаемся назад префиксным декрементом.
    return tmp; // возвращаем состояние до сдвига.
}

size_t allocator_boundary_tags::boundary_iterator::size() const noexcept
{
    if (_occupied_ptr == nullptr) { // для конечного итератора размер 0.
        return 0;
    }
    void* block_start = _occupied // вычисляем начало блока.
        ? static_cast<char*>(_occupied_ptr) - allocator_boundary_tags::occupied_block_metadata_size
        : static_cast<char*>(_occupied_ptr) - sizeof(size_t);
    return blk_size(block_start); // возвращаем размер блока.
}

bool allocator_boundary_tags::boundary_iterator::occupied() const noexcept
{
    return _occupied; // возвращаем флаг занятости.
}

void* allocator_boundary_tags::boundary_iterator::operator*() const noexcept
{
    return _occupied_ptr; // оператор разыменования: возвращает указатель на полезную нагрузку текущего блока.
}

void* allocator_boundary_tags::boundary_iterator::get_ptr() const noexcept
{
    return _occupied_ptr; // явный метод получения указателя (аналогичен operator*).
}
