#ifndef MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_BUDDIES_SYSTEM_H // include guard (объяснялся ранее)
#define MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_BUDDIES_SYSTEM_H

#include <pp_allocator.h>            // интерфейс памяти (объяснялся ранее)
#include <allocator_test_utils.h>    // тестовые утилиты (объяснялся ранее)
#include <allocator_with_fit_mode.h> // контракт режима размещения (объяснялся ранее)
#include <mutex>                     // примитив синхронизации (объяснялся ранее)
#include <cmath>                     // стандартный математический заголовок. в данной реализации напрямую не используется, но может понадобиться для математических функций (log2, pow и т.д.) при альтернативных способах вычисления степеней двойки.

namespace __detail // именованное пространство имён __detail (double underscore + detail). такое соглашение часто используется для внутренних деталей реализации, которые не являются частью публичного api. всё внутри видно только в этом заголовке и подключённых к нему файлах.
{
    constexpr size_t nearest_greater_k_of_2(size_t size) noexcept // constexpr функция, которая вычисляет позицию старшего значащего бита числа size (т.е. округлённый вверх log2). если size — степень двойки, возвращает её показатель; если нет — возвращает показатель следующей степени двойки. вычисляется на этапе компиляции, поэтому результат можно использовать в static_assert и как размер массива.
    {
        int ones_counter = 0, index = -1; // ones_counter считает количество установленных битов (единиц) в двоичном представлении size. index — позиция старшего установленного бита.

        constexpr const size_t o = 1; // константа 1 типа size_t. используется для битовых операций.

        for (int i = sizeof(size_t) * 8 - 1; i >= 0; --i) // цикл по всем битам size_t с конца (от старшего к младшему). sizeof(size_t) * 8 — количество бит в size_t (обычно 64). i >= 0 — условие продолжения; --i — декремент.
        {
            if (size & (o << i)) // битовая операция И (&). o << i — сдвиг единицы влево на i позиций, создаёт маску с единицей на i-м бите. size & (o << i) проверяет, установлен ли i-й бит в числе size. если результат ненулевой — бит установлен.
            {
                if (ones_counter == 0) // если это первый найденный установленный бит (идём с конца)...
                    index = i;         // ...запоминаем его позицию как старший значащий бит.
                ++ones_counter;        // увеличиваем счётчик установленных битов.
            }
        }

        return ones_counter <= 1 ? index : index + 1; // тернарный оператор. если в числе size не более одной единицы (ones_counter <= 1), значит size является степенью двойки (или нулём), и возвращаем index — показатель степени. если единиц больше одного, число не является степенью двойки, и возвращаем index + 1 — показатель ближайшей большей степени двойки.
    }
}

class allocator_buddies_system final:
    public smart_mem_resource,       // интерфейс полиморфного ресурса (объяснялся ранее)
    public allocator_test_utils,     // тестовый интерфейс (объяснялся ранее)
    public allocator_with_fit_mode   // контракт fit_mode (объяснялся ранее)
{

private:

    struct block_metadata // структура с битовыми полями (bit fields). битовые поля позволяют упаковать несколько членов структуры в одно целое число, экономя память. здесь оба поля занимают ровно 1 байт (8 бит): occupied — 1 бит, size — 7 бит.
    {
        bool occupied : 1;          // битовое поле: флаг занятости блока. : 1 означает, что поле занимает ровно 1 бит. может принимать значения 0 (false) или 1 (true). благодаря этому экономится память: вместо целого bool (обычно 1 байт) используется 1 бит.
        unsigned char size : 7;     // битовое поле: показатель степени двойки k (размер блока = 2^k). : 7 означает 7 бит, что позволяет хранить значения от 0 до 127. этого достаточно для блоков размером до 2^127 байт (гораздо больше любой реальной памяти).
    };                              // вся структура block_metadata упаковывается компилятором в минимально возможное целое число. sizeof(block_metadata) здесь равен 1 байт.

    void *_trusted_memory; // единственное поле, разрешённое заданием. указатель на доверенную область памяти. все служебные данные (мьютекс, родитель, режим, max_k) хранятся в начале этой области; сами блоки идут следом.

    /**
     * TODO: You must improve it for alignment support
     */

    static constexpr const size_t allocator_metadata_size = sizeof(allocator_dbg_helper*) + sizeof(fit_mode) + sizeof(unsigned char) + sizeof(std::mutex); // размер метаданных аллокатора. unsigned char используется для хранения max_k (максимальной степени двойки, определяющей размер всей доверенной области).

    static constexpr const size_t occupied_block_metadata_size = sizeof(block_metadata) + sizeof(void*); // размер метаданных занятого блока: 1 байт (bit fields) + указатель на родителя (void*).

    static constexpr const size_t free_block_metadata_size = sizeof(block_metadata); // размер метаданных свободного блока: только 1 байт bit fields, без дополнительных указателей.

    static constexpr const size_t min_k = __detail::nearest_greater_k_of_2(occupied_block_metadata_size); // вызов constexpr функции на этапе компиляции. min_k — минимально возможный показатель степени двойки для блока. он определяется как ближайшая большая или равная степень двойки, способная вместить occupied_block_metadata_size. т.е. минимальный блок имеет размер 2^min_k байт.

public:

    explicit allocator_buddies_system(
            size_t space_size_power_of_two,
            std::pmr::memory_resource *parent_allocator = nullptr,
            allocator_with_fit_mode::fit_mode allocate_fit_mode = allocator_with_fit_mode::fit_mode::first_fit); // конструктор. space_size_power_of_two — желаемый размер доверенной памяти. аллокатор округлит его вверх до ближайшей степени двойки (т.к. система двойников работает только с такими размерами).

    allocator_buddies_system(
        allocator_buddies_system const &other); // конструктор копирования. в реализации бросает not_implemented (копирование не поддерживается).
    
    allocator_buddies_system &operator=(
        allocator_buddies_system const &other); // оператор копирования. тоже бросает not_implemented.
    
    allocator_buddies_system(
        allocator_buddies_system &&other) noexcept; // перемещение (объяснялось ранее)
    
    allocator_buddies_system &operator=(
        allocator_buddies_system &&other) noexcept; // перемещающее присваивание (объяснялось ранее)

    ~allocator_buddies_system() override; // деструктор (объяснялся ранее)

private:
    
    [[nodiscard]] void *do_allocate_sm(
        size_t size) override; // выделение памяти. ищет свободный блок размером 2^k, где 2^k >= size + occupied_block_metadata_size. если блок слишком велик, рекурсивно (циклически) разбивает его пополам (split) до нужного размера. при этом создаётся «двойник» (buddy) — второй половинный блок, который помечается свободным.
    
    void do_deallocate_sm(
        void *at) override; // освобождение памяти. вычисляет адрес «двойника» (buddy) через xor-операцию над смещением от начала региона. если buddy свободен и того же размера — происходит слияние (coalesce). процесс повторяется рекурсивно вверх по дереву блоков.

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override; // сравнение (объяснялось ранее)

    inline void set_fit_mode(
        allocator_with_fit_mode::fit_mode mode) override; // смена режима (объяснялась ранее)

    std::vector<allocator_test_utils::block_info> get_blocks_info() const noexcept override; // информация о блоках (объяснялась ранее)

private:

    std::vector<allocator_test_utils::block_info> get_blocks_info_inner() const override; // внутренний сбор информации (объяснялся ранее)

    class buddy_iterator // вложенный класс-итератор для обхода всех блоков в физическом порядке. в отличие от boundary_iterator (2-я задача) и sorted_iterator (3-я задача), здесь нет отдельного итератора по свободным блокам — только общий обход всех блоков через их метаданные.
    {
        void* _block; // указатель на заголовок текущего блока (block_metadata)

    public:

        using iterator_category = std::forward_iterator_tag; // только вперёд (объяснялось в 3-й задаче)
        using value_type = void*;     // (объяснялось ранее)
        using reference = void*&;     // (объяснялось ранее)
        using pointer = void**;       // (объяснялось ранее)
        using difference_type = ptrdiff_t; // (объяснялось ранее)

        bool operator==(const buddy_iterator&) const noexcept; // (объяснялось ранее)
        bool operator!=(const buddy_iterator&) const noexcept; // (объяснялось ранее)

        buddy_iterator& operator++() & noexcept; // префиксный инкремент: сдвигается на размер текущего блока (1 << meta.size). (объяснялось ранее)
        buddy_iterator operator++(int n); // постфиксный инкремент (объяснялось ранее)

        size_t size() const noexcept; // возвращает размер блока как 1 << meta.size (т.е. 2 в степени k).
        bool occupied() const noexcept; // возвращает meta.occupied из bit fields.
        void* operator*() const noexcept; // возвращает payload (адрес после occupied_block_metadata_size).

        buddy_iterator(); // пустой итератор (end)
        buddy_iterator(void* start); // конструктор от указателя на начало блока
    };

    friend class buddy_iterator; // (объяснялось ранее)

    buddy_iterator begin() const noexcept; // (объяснялось ранее)
    buddy_iterator end() const noexcept;   // end указывает за пределы региона (reg_start + 2^max_k)
    
};

#endif //MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_BUDDIES_SYSTEM_H
