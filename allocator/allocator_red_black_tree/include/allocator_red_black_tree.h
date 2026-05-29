#ifndef MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_RED_BLACK_TREE_H // include guard (объяснялся ранее)
#define MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_RED_BLACK_TREE_H

#include <pp_allocator.h>            // интерфейс памяти (объяснялся ранее)
#include <allocator_test_utils.h>    // тестовые утилиты (объяснялся ранее)
#include <allocator_with_fit_mode.h> // контракт режима размещения (объяснялся ранее)
#include <mutex>                     // примитив синхронизации (объяснялся ранее)

class allocator_red_black_tree final:
    public smart_mem_resource,       // интерфейс полиморфного ресурса (объяснялся ранее)
    public allocator_test_utils,     // тестовый интерфейс (объяснялся ранее)
    public allocator_with_fit_mode   // контракт fit_mode (объяснялся ранее)
{

public:

    enum class block_color : unsigned char // enum class с явным указанием базового типа. : unsigned char означает, что каждое значение перечисления хранится в 1 байте (вместо int по умолчанию). это экономит память, т.к. цвет блока (RED или BLACK) может принимать только 2 значения.
    { RED, BLACK };                      // RED (красный) и BLACK (чёрный) — цвета узлов дерева. свойства красно-чёрного дерева: корень чёрный, красный узел имеет двух чёрных детей, все пути от узла к листьям содержат одинаковое число чёрных узлов.

    struct block_data // структура с битовыми полями для хранения состояния блока в 1 байте.
    {
        bool occupied : 4;          // битовое поле: флаг занятости блока. : 4 означает, что поле занимает 4 бита (хотя bool нужен только 1, компилятор выравнивает до 4 бит). это сделано для симметрии с color.
        block_color color : 4;      // битовое поле: цвет узла в красно-чёрном дереве. : 4 означает 4 бита, достаточных для хранения enum class на базе unsigned char (значения 0 и 1).
    };                              // суммарно block_data занимает ровно 1 байт (4 + 4 = 8 бит). sizeof(block_data) == 1.

private:

    void *_trusted_memory; // единственное поле, разрешённое заданием. указатель на доверенную область памяти, в начале которой хранятся мьютекс, родительский аллокатор, размер, корень дерева и режим.

    static constexpr const size_t allocator_metadata_size = sizeof(allocator_dbg_helper*) + sizeof(fit_mode) + sizeof(size_t) + sizeof(std::mutex) + sizeof(void*); // размер метаданных аллокатора (объяснялся ранее). sizeof(void*) — место под указатель на корень красно-чёрного дерева.
    static constexpr const size_t occupied_block_metadata_size = sizeof(block_data) + 3 * sizeof(void*); // размер метаданных занятого блока: 1 байт (block_data) + 3 указателя: prev, next (физические связи) и parent_alloc (указатель на родительский аллокатор для проверки принадлежности).
    static constexpr const size_t free_block_metadata_size = sizeof(block_data) + 5 * sizeof(void*); // размер метаданных свободного блока: 1 байт + 5 указателей. prev/next (физические связи с соседями в памяти) + left/right/parent_tree (структура красно-чёрного дерева для быстрого поиска по размеру).

public:
    
    ~allocator_red_black_tree() override; // деструктор (объяснялся ранее)
    
    allocator_red_black_tree(
        allocator_red_black_tree const &other); // конструктор копирования. не поддерживается (бросает not_implemented).
    
    allocator_red_black_tree &operator=(
        allocator_red_black_tree const &other); // оператор копирования. не поддерживается.
    
    allocator_red_black_tree(
        allocator_red_black_tree &&other) noexcept; // перемещение (объяснялось ранее)
    
    allocator_red_black_tree &operator=(
        allocator_red_black_tree &&other) noexcept; // перемещающее присваивание (объяснялось ранее)

public:
    
    explicit allocator_red_black_tree(
            size_t space_size,
            std::pmr::memory_resource *parent_allocator = nullptr,
            allocator_with_fit_mode::fit_mode allocate_fit_mode = allocator_with_fit_mode::fit_mode::first_fit); // конструктор (объяснялся ранее). space_size — размер пользовательской области (не обязательно степень двойки, в отличие от buddy system).

private:
    
    [[nodiscard]] void *do_allocate_sm(
        size_t size) override; // выделение памяти. в режиме best_fit использует поиск в красно-чёрном дереве (lower_bound по размеру) за o(log n). в режиме worst_fit — поиск максимального элемента. в first_fit — линейный обход физического списка блоков.
    
    void do_deallocate_sm(
        void *at) override; // освобождение памяти с проверкой принадлежности, слиянием с соседями (coalesce) и балансировкой дерева (удаление старых блоков из дерева + вставка объединённого).

    bool do_is_equal(const std::pmr::memory_resource&) const noexcept override; // (объяснялось ранее)

    std::vector<allocator_test_utils::block_info> get_blocks_info() const override; // (объяснялось ранее)
    
    inline void set_fit_mode(allocator_with_fit_mode::fit_mode mode) override; // (объяснялось ранее)

private:

    std::vector<allocator_test_utils::block_info> get_blocks_info_inner() const override; // (объяснялось ранее)

    class rb_iterator // вложенный класс-итератор для обхода всех блоков по физическим связям next (как в sorted_list). в отличие от sorted_free_iterator (3-я задача), который обходил только свободные, этот идёт подряд по всем блокам в памяти.
    {
        void* _block_ptr; // указатель на текущий блок
        void* _trusted;   // указатель на доверенную память (нужен для вычисления размера блока через blk_size)

    public:

        using iterator_category = std::forward_iterator_tag; // только вперёд (объяснялось в 3-й задаче)
        using value_type = void*;     // (объяснялось ранее)
        using reference = void*&;     // (объяснялось ранее)
        using pointer = void**;       // (объяснялось ранее)
        using difference_type = ptrdiff_t; // (объяснялось ранее)

        bool operator==(const rb_iterator&) const noexcept; // (объяснялось ранее)
        bool operator!=(const rb_iterator&) const noexcept; // (объяснялось ранее)

        rb_iterator& operator++() & noexcept; // префиксный инкремент: двигается к следующему блоку через blk_next (физическая связь). & после скобок — ограничение lvalue (объяснялось ранее).
        rb_iterator operator++(int n); // постфиксный инкремент (объяснялось ранее)

        size_t size() const noexcept; // размер текущего блока (вычисляется через blk_size, а не хранится явно).
        void* operator*() const noexcept; // payload текущего блока: для занятого — после occupied_block_metadata_size, для свободного — после free_block_metadata_size.

        bool occupied()const noexcept; // возвращает blk_occupied(_block_ptr) — флаг занятости из bit fields.

        rb_iterator(); // пустой итератор (end)
        rb_iterator(void* trusted); // устанавливает _block_ptr на первый блок в регионе
    };

    friend class rb_iterator; // (объяснялось ранее)

public:

    rb_iterator begin() const noexcept; // (объяснялось ранее)
    rb_iterator end() const noexcept;   // (объяснялось ранее)

};

#endif //MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_RED_BLACK_TREE_H
