#ifndef MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_BOUNDARY_TAGS_H // include guard (объяснялся ранее)
#define MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_BOUNDARY_TAGS_H

#include <allocator_test_utils.h>   // заголовок с базовым классом allocator_test_utils. он содержит структуру block_info (размер и занятость блока) и виртуальные методы для получения информации о блоках — нужно для тестирования и отладки.
#include <allocator_with_fit_mode.h> // заголовок с абстрактным классом allocator_with_fit_mode. от него наследуемся, чтобы поддерживать переключаемый режим выделения памяти (first_fit, the_best_fit, the_worst_fit).
#include <pp_allocator.h>            // интерфейс polymorphic memory resource (объяснялся ранее)
#include <iterator>                  // стандартный заголовок <iterator>. содержит теги категорий итераторов (например, std::bidirectional_iterator_tag), нужные для создания собственного итератора, совместимого со стандартной библиотекой c++.
#include <mutex>                     // примитив синхронизации (объяснялся ранее)

class allocator_boundary_tags final :
    public smart_mem_resource,       // интерфейс памяти (объяснялся ранее)
    public allocator_test_utils,     // наследуем тестовые утилиты. allocator_test_utils добавляет метод get_blocks_info(), который возвращает std::vector<block_info> — список всех блоков с их размером и флагом занятости. это позволяет тестам проверять, правильно ли аллокатор разбивает и освобождает память.
    public allocator_with_fit_mode   // наследуем контракт режима размещения. allocator_with_fit_mode — абстрактный класс, содержащий enum class fit_mode { first_fit, the_best_fit, the_worst_fit } и чисто виртуальный метод set_fit_mode().
{

private:

    static constexpr const size_t allocator_metadata_size = sizeof(memory_resource*) + sizeof(allocator_with_fit_mode::fit_mode) +
                                                            sizeof(size_t) + sizeof(std::mutex) + sizeof(void*); // размер метаданных самого аллокатора, которые хранятся в начале доверенной памяти. memory_resource* — указатель на родительский аллокатор. fit_mode — режим размещения. size_t — размер доверенной области. std::mutex — мьютекс для синхронизации. void* — указатель на первый свободный блок. constexpr означает, что выражение вычисляется на этапе компиляции.

    static constexpr const size_t occupied_block_metadata_size = sizeof(size_t) + sizeof(void*) + sizeof(void*) + sizeof(void*); // размер служебной информации (метаданных), хранимой внутри каждого занятого блока. size_t — размер блока + флаг занятости в младшем бите. три void* — указатели prev, next, parent (дескрипторы границ).

    static constexpr const size_t free_block_metadata_size = 0; // для свободного блока дополнительных метаданных не требуется (размер и флаг хранятся в том же заголовке size_t, что и у занятого).

    void *_trusted_memory; // единственное поле-указатель, разрешённое заданием. _trusted_memory указывает на начало доверенной области памяти, выделенной из родительского аллокатора (или глобальной кучи). все остальные данные аллокатора (мьютекс, режим, размер, список свободных блоков) хранятся прямо в начале этой области — по смещениям от _trusted_memory.

public:
    
    ~allocator_boundary_tags() override; // деструктор (объяснялся ранее)
    
    allocator_boundary_tags(allocator_boundary_tags const &other); // конструктор копирования (объяснялся ранее). в реализации бросает not_implemented, т.к. копирование не поддерживается.
    
    allocator_boundary_tags &operator=(allocator_boundary_tags const &other); // оператор копирования (объяснялся ранее). тоже бросает not_implemented.
    
    allocator_boundary_tags(
        allocator_boundary_tags &&other) noexcept; // конструктор перемещения (объяснялся ранее)
    
    allocator_boundary_tags &operator=(
        allocator_boundary_tags &&other) noexcept; // оператор перемещения (объяснялся ранее)

public:
    
    explicit allocator_boundary_tags(
            size_t space_size,
            std::pmr::memory_resource *parent_allocator = nullptr,
            allocator_with_fit_mode::fit_mode allocate_fit_mode = allocator_with_fit_mode::fit_mode::first_fit); // конструктор с тремя параметрами. space_size — размер доверенной области памяти (в байтах), которую аллокатор будет раздавать. parent_allocator — указатель на родительский ресурс памяти (std::pmr::memory_resource*), из которого запрашивается доверенная память. nullptr по умолчанию означает, что если родитель не указан, используется глобальная куча (через std::pmr::get_default_resource()). allocate_fit_mode — режим размещения: first_fit (первый подходящий), the_best_fit (наименьший подходящий), the_worst_fit (наибольший подходящий).

private:
    
    [[nodiscard]] void *do_allocate_sm(
        size_t bytes) override; // выделение памяти с учётом fit_mode (объяснялся ранее как override)
    
    void do_deallocate_sm(
        void *at) override; // освобождение памяти с дескрипторами границ (boundary tags). перед освобождением проверяет, принадлежит ли указатель текущему аллокатору, и выполняет слияние (coalesce) с соседними свободными блоками.

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override; // сравнение ресурсов (объяснялось ранее)

public:
    
    inline void set_fit_mode(
        allocator_with_fit_mode::fit_mode mode) override; // реализация чисто виртуального метода из базового класса allocator_with_fit_mode. inline — подсказка компилятору вставить тело функции прямо в место вызова (часто используется для простых сеттеров). позволяет пользователю переключать стратегию поиска свободного блока после создания аллокатора. override — переопределение.

public:
    
    std::vector<allocator_test_utils::block_info> get_blocks_info() const override; // реализация метода из allocator_test_utils. возвращает вектор (std::vector — динамический массив из стандартной библиотеки), содержащий информацию о каждом блоке: размер и занятость. const — метод не меняет объект. override — переопределение. используется тестами для проверки внутреннего состояния.

private:

    std::vector<allocator_test_utils::block_info> get_blocks_info_inner() const override; // защищённая версия get_blocks_info без синхронизации. вызывается из get_blocks_info, который уже захватил мьютекс, чтобы избежать deadlock (взаимной блокировки) при повторном захвате.

/** TODO: Highly recommended for helper functions to return references */

    class boundary_iterator // вложенный класс (nested class) — класс, объявленный внутри другого класса. boundary_iterator реализует итератор для обхода блоков памяти внутри аллокатора. благодаря этому можно использовать range-based for или стандартные алгоритмы.
    {
        friend class allocator_boundary_tags; // friend — ключевое слово, разрешающее классу allocator_boundary_tags обращаться к приватным и защищённым членам boundary_iterator. без этого allocator_boundary_tags не смог бы использовать приватные поля _occupied_ptr и _trusted_memory итератора.
        void* _occupied_ptr; // указатель на полезную нагрузку (payload) блока — адрес, который получил пользователь после allocate. если блок занят, это адрес после метаданных; если свободен — после заголовка size_t.
        bool _occupied;      // флаг: true — блок занят, false — свободен. нужен, чтобы итератор знал, как вычислять смещение до начала блока.
        void* _trusted_memory; // указатель на доверенную память аллокатора. нужен итератору, чтобы проверять границы региона и вычислять следующий/предыдущий блок.

    public:

        using iterator_category = std::bidirectional_iterator_tag; // using — создание псевдонима типа (аналог typedef). iterator_category сообщает стандартной библиотеке, какие операции поддерживает итератор. std::bidirectional_iterator_tag означает, что итератор поддерживает ++ (вперёд) и -- (назад), но не произвольный доступ (как у массива).
        using value_type = void*;     // тип значения, на которое указывает итератор. в нашем случае — void*, т.е. адрес полезной нагрузки блока.
        using reference = void*&;     // тип ссылки на value_type. используется стандартными алгоритмами для шаблонного вывода.
        using pointer = void**;       // тип указателя на value_type.
        using difference_type = ptrdiff_t; // тип разности двух итераторов. ptrdiff_t — стандартный знаковый целочисленный тип, результат вычитания указателей. используется в стандартных алгоритмах для вычисления расстояний.

        bool operator==(const boundary_iterator&) const noexcept; // оператор сравнения на равенство. два итератора равны, если указывают на один и тот же блок с одинаковым состоянием. const noexcept — не изменяет объект и не бросает исключений.

        bool operator!=(const boundary_iterator&) const noexcept; // оператор сравнения на неравенство. реализуется через operator== (возвращает !(*this == other)). позволяет писать в циклах: while (it != end_it).

        boundary_iterator& operator++() & noexcept; // префиксный инкремент (++it). перемещает итератор к следующему блоку в памяти. возвращает ссылку на себя (boundary_iterator&), чтобы можно было писать ++(++it). & после скобок означает, что метод может вызываться только у lvalue (именованного объекта), а не у временного объекта.

        boundary_iterator& operator--() & noexcept; // префиксный декремент (--it). перемещает итератор к предыдущему блоку. тоже возвращает ссылку и ограничен lvalue.

        boundary_iterator operator++(int n); // постфиксный инкремент (it++). int в скобках — фиктивный параметр, который отличает постфиксную версию от префиксной. возвращает копию итератора ДО перемещения (по значению, а не по ссылке), а сам объект сдвигает вперёд.

        boundary_iterator operator--(int n); // постфиксный декремент (it--). аналогично: фиктивный int, возвращает старую копию, затем сдвигает назад.

        size_t size() const noexcept; // возвращает размер текущего блока (включая метаданные) в байтах. нужен для отладки и тестов.

        bool occupied() const noexcept; // возвращает true, если текущий блок занят, и false, если свободен.

        void* operator*() const noexcept; // оператор разыменования итератора. возвращает указатель на полезную нагрузку текущего блока (payload). позволяет писать *it, как для обычных указателей и стандартных итераторов.

        void* get_ptr() const noexcept; // дополнительный метод для явного получения указателя на полезную нагрузку. аналогичен operator*.

        boundary_iterator(); // конструктор по умолчанию. создаёт «пустой» итератор (end-итератор), у которого _occupied_ptr == nullptr.

        boundary_iterator(void* trusted); // конструктор от указателя на доверенную память. устанавливает итератор на первый блок в регионе (если регион не пуст).
    };

    friend class boundary_iterator; // разрешаем классу boundary_iterator обращаться к приватным членам allocator_boundary_tags. хотя boundary_iterator вложен, friend делает доступ явным и двусторонним.

    boundary_iterator begin() const noexcept; // возвращает итератор, указывающий на первый блок в доверенной памяти. аналогично begin() у std::vector или std::string. const noexcept — не меняет аллокатор.

    boundary_iterator end() const noexcept;   // возвращает пустой итератор (boundary_iterator()), обозначающий конец последовательности. цикл for (auto it = alloc.begin(); it != alloc.end(); ++it) позволяет обойти все блоки.
};

#endif //MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_BOUNDARY_TAGS_H
