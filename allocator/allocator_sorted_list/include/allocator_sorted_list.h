#ifndef MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_SORTED_LIST_H // include guard (объяснялся ранее)
#define MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_SORTED_LIST_H

#include <pp_allocator.h>            // интерфейс памяти (объяснялся ранее)
#include <allocator_test_utils.h>    // тестовые утилиты (объяснялся ранее)
#include <allocator_with_fit_mode.h> // контракт режима размещения (объяснялся ранее)
#include <iterator>                  // теги итераторов (объяснялся ранее)
#include <mutex>                     // примитив синхронизации (объяснялся ранее)

class allocator_sorted_list final:
    public smart_mem_resource,       // интерфейс полиморфного ресурса (объяснялся ранее)
    public allocator_test_utils,     // тестовый интерфейс с block_info (объяснялся ранее)
    public allocator_with_fit_mode   // контракт fit_mode (объяснялся ранее)
{

private:
    
    void *_trusted_memory; // единственное поле, разрешённое заданием. указатель на доверенную область памяти, выделенную из родительского аллокатора или глобальной кучи. вся служебная информация (мьютекс, размер, режим, указатель на первый свободный) хранится прямо в начале этой области.

    static constexpr const size_t allocator_metadata_size = sizeof(std::pmr::memory_resource *) + sizeof(fit_mode) + sizeof(size_t) + sizeof(std::mutex) + sizeof(void*); // суммарный размер служебных полей аллокатора в начале доверенной памяти (объяснялся ранее)

    static constexpr const size_t block_metadata_size = sizeof(void*) + sizeof(size_t); // размер заголовка одного блока. size_t хранит размер блока. void* хранит указатель на следующий свободный блок (используется только когда блок свободен; при занятости это поле не используется, но место под него резервируется).

public:

    explicit allocator_sorted_list(
            size_t space_size,
            std::pmr::memory_resource *parent_allocator = nullptr,
            allocator_with_fit_mode::fit_mode allocate_fit_mode = allocator_with_fit_mode::fit_mode::first_fit); // конструктор (объяснялся ранее)
    
    allocator_sorted_list(
        allocator_sorted_list const &other); // конструктор копирования. в отличие от allocator_boundary_tags, где копирование запрещено (throw not_implemented), здесь оно реализовано: создаётся новая доверенная память того же размера, содержимое копируется побайтно, а указатели в списке свободных блоков пересчитываются с учётом нового базового адреса.
    
    allocator_sorted_list &operator=(
        allocator_sorted_list const &other); // оператор копирующего присваивания. реализован по идиоме «создать копию, затем обменять с текущим». если old_trusted не nullptr, старая память освобождается.

    allocator_sorted_list(
        allocator_sorted_list &&other) noexcept; // перемещение (объяснялось ранее)
    
    allocator_sorted_list &operator=(
        allocator_sorted_list &&other) noexcept; // перемещающее присваивание (объяснялось ранее)

    ~allocator_sorted_list() override; // деструктор (объяснялся ранее)

private:
    
    [[nodiscard]] void *do_allocate_sm(
        size_t size) override; // выделение памяти с учётом fit_mode (объяснялся ранее)
    
    void do_deallocate_sm(
        void *at) override; // освобождение с проверкой принадлежности, защитой от double free, и слиянием (coalesce) смежных свободных блоков. список свободных блоков поддерживается отсортированным по возрастанию адреса.

    bool do_is_equal(const std::pmr::memory_resource&) const noexcept override; // сравнение ресурсов (объяснялся ранее)
    
    inline void set_fit_mode(
        allocator_with_fit_mode::fit_mode mode) override; // смена режима (объяснялся ранее)

    std::vector<allocator_test_utils::block_info> get_blocks_info() const noexcept override; // возвращает информацию о блоках. const noexcept — метод гарантированно не бросает исключений наружу; если внутри произойдёт ошибка, она перехватывается и возвращается пустой вектор.

private:

    std::vector<allocator_test_utils::block_info> get_blocks_info_inner() const override; // реальная реализация сбора информации без синхронизации. занятость блока определяется через сравнение адреса блока с адресом текущего свободного блока из explicit списка: если совпадает — блок свободен, иначе — занят.

    class sorted_free_iterator // вложенный класс-итератор для обхода только свободных блоков через explicit список (по указателям next). в отличие от boundary_iterator из второй задачи, который обходил все блоки физически, этот итератор «прыгает» только по свободным, используя внутриблочные указатели.
    {
        friend class allocator_sorted_list;
        void* _free_ptr; // указатель на заголовок текущего свободного блока

    public:

        using iterator_category = std::forward_iterator_tag; // категория итератора: однонаправленный (только вперёд). в отличие от std::bidirectional_iterator_tag во второй задаче, forward_iterator_tag не требует реализации operator-- (движение назад). достаточно для однопроходных алгоритмов (поиск, обход списка).
        using value_type = void*;     // тип значения (объяснялся ранее)
        using reference = void*&;     // тип ссылки (объяснялся ранее)
        using pointer = void**;       // тип указателя (объяснялся ранее)
        using difference_type = ptrdiff_t; // тип разности (объяснялся ранее)

        bool operator==(const sorted_free_iterator&) const noexcept; // сравнение (объяснялось ранее)
        bool operator!=(const sorted_free_iterator&) const noexcept; // сравнение (объяснялось ранее)

        sorted_free_iterator& operator++() & noexcept; // префиксный инкремент: переходит к следующему свободному блоку через blk_next. & после скобок — метод может вызываться только у lvalue (объяснялось ранее).

        sorted_free_iterator operator++(int n); // постфиксный инкремент (объяснялся ранее)

        size_t size() const noexcept; // размер текущего свободного блока (включая метаданные)

        void* operator*() const noexcept; // разыменование: возвращает указатель на полезную нагрузку (payload) свободного блока

        sorted_free_iterator(); // конструктор по умолчанию: пустой итератор (end)
        sorted_free_iterator(void* trusted); // конструктор от trusted_memory: устанавливает _free_ptr на первый свободный блок
    };

    class sorted_iterator // вложенный класс-итератор для последовательного обхода ВСЕХ блоков (занятых и свободных) в физическом порядке в памяти. в отличие от sorted_free_iterator, он идёт от блока к блоку через их размеры, а для определения занятости сравнивает адрес с текущим свободным из explicit списка.
    {
        friend class allocator_sorted_list;
        void* _free_ptr;      // указатель на текущий свободный блок (из explicit списка). нужен, чтобы отличить свободный блок от занятого при физическом обходе.
        void* _current_ptr;   // указатель на заголовок текущего блока (всех подряд в памяти)
        void* _trusted_memory; // указатель на доверенную память (для вычисления конца региона)

    public:

        using iterator_category = std::forward_iterator_tag; // только вперёд, без operator--. forward_iterator_tag достаточно для обхода всех блоков в одну сторону.
        using value_type = void*;     // (объяснялось ранее)
        using reference = void*&;     // (объяснялось ранее)
        using pointer = void**;       // (объяснялось ранее)
        using difference_type = ptrdiff_t; // (объяснялось ранее)

        bool operator==(const sorted_iterator&) const noexcept; // (объяснялось ранее)
        bool operator!=(const sorted_iterator&) const noexcept; // (объяснялось ранее)

        sorted_iterator& operator++() & noexcept; // префиксный инкремент: _current_ptr сдвигается на blk_size(_current_ptr), переходя к следующему физическому блоку. если _current_ptr совпал с _free_ptr, значит блок свободный, и _free_ptr продвигается дальше по списку свободных (blk_next).

        sorted_iterator operator++(int n); // постфиксный инкремент (объяснялся ранее)

        size_t size() const noexcept; // размер текущего блока

        void* operator*() const noexcept; // указатель на payload текущего блока

        bool occupied()const noexcept; // true, если текущий блок занят. логика: _current_ptr != _free_ptr. если адреса равны — блок находится в explicit списке свободных, значит он свободен.

        sorted_iterator(); // пустой итератор (end)
        sorted_iterator(void* trusted); // устанавливает _current_ptr на первый блок, _free_ptr — на первый свободный
    };

    friend class sorted_iterator;     // (объяснялось ранее)
    friend class sorted_free_iterator; // (объяснялось ранее)

    sorted_free_iterator free_begin() const noexcept; // возвращает итератор на первый свободный блок (голову explicit списка). аналог begin(), но только для свободных.
    sorted_free_iterator free_end() const noexcept;   // возвращает пустой sorted_free_iterator (nullptr), обозначающий конец списка свободных.

    sorted_iterator begin() const noexcept; // (объяснялось ранее)
    sorted_iterator end() const noexcept;   // (объяснялось ранее)
};

#endif //MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_SORTED_LIST_H
