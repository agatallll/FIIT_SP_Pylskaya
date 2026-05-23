#include <not_implemented.h>
#include "../include/allocator_boundary_tags.h"
#include <mutex>
#include <memory_resource>
#include <stdexcept>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <cstring>

namespace {
    // вычисляем размер метаданных аллокатора вручную (совпадает с allocator_metadata_size)
    constexpr size_t allocator_metadata_size_local = sizeof(std::mutex)
                                                   + sizeof(std::pmr::memory_resource*)
                                                   + sizeof(size_t)
                                                   + sizeof(void*)
                                                   + sizeof(allocator_with_fit_mode::fit_mode);

    // смещения полей метаданных аллокатора в доверенной памяти
    constexpr size_t mtx_offset = 0;
    constexpr size_t parent_offset = sizeof(std::mutex); // 80
    constexpr size_t space_size_offset = parent_offset + sizeof(std::pmr::memory_resource*); // 88
    constexpr size_t first_free_offset = space_size_offset + sizeof(size_t); // 96
    constexpr size_t mode_offset = first_free_offset + sizeof(void*); // 104

    // доступ к полям метаданных аллокатора
    std::mutex& get_mtx(void* trusted) noexcept {
        return *reinterpret_cast<std::mutex*>(static_cast<char*>(trusted) + mtx_offset);
    }
    std::pmr::memory_resource*& get_parent(void* trusted) noexcept {
        return *reinterpret_cast<std::pmr::memory_resource**>(static_cast<char*>(trusted) + parent_offset);
    }
    size_t& get_space_size(void* trusted) noexcept {
        return *reinterpret_cast<size_t*>(static_cast<char*>(trusted) + space_size_offset);
    }
    void*& get_first_free(void* trusted) noexcept {
        return *reinterpret_cast<void**>(static_cast<char*>(trusted) + first_free_offset);
    }
    allocator_with_fit_mode::fit_mode& get_mode(void* trusted) noexcept {
        return *reinterpret_cast<allocator_with_fit_mode::fit_mode*>(static_cast<char*>(trusted) + mode_offset);
    }

    // начало области блоков (ровно за метаданными аллокатора)
    void* region_start(void* trusted) noexcept {
        return static_cast<char*>(trusted) + allocator_metadata_size_local;
    }

    // безопасное чтение/запись size_t и void* через memcpy
    // (адреса блоков могут быть невыровненными, т.к. allocator_metadata_size = 108)
    size_t read_size_t(void* addr) noexcept {
        size_t val;
        std::memcpy(&val, addr, sizeof(val));
        return val;
    }
    void write_size_t(void* addr, size_t val) noexcept {
        std::memcpy(addr, &val, sizeof(val));
    }
    void* read_ptr(void* addr) noexcept {
        void* val;
        std::memcpy(&val, addr, sizeof(val));
        return val;
    }
    void write_ptr(void* addr, void* val) noexcept {
        std::memcpy(addr, &val, sizeof(val));
    }

    // размер блока из заголовка (без младшего бита)
    size_t blk_size(void* block) noexcept {
        return read_size_t(block) & ~static_cast<size_t>(1);
    }
    // флаг занятости из заголовка
    bool blk_occupied(void* block) noexcept {
        return (read_size_t(block) & 1) != 0;
    }
    // установить заголовок блока
    void set_blk(void* block, size_t sz, bool occ) noexcept {
        write_size_t(block, occ ? (sz | 1) : sz);
    }

    // чтение/запись указателей внутри блока
    void* blk_prev(void* block) noexcept {
        return read_ptr(static_cast<char*>(block) + sizeof(size_t));
    }
    void set_blk_prev(void* block, void* val) noexcept {
        write_ptr(static_cast<char*>(block) + sizeof(size_t), val);
    }
    void* blk_next(void* block) noexcept {
        return read_ptr(static_cast<char*>(block) + sizeof(size_t) + sizeof(void*));
    }
    void set_blk_next(void* block, void* val) noexcept {
        write_ptr(static_cast<char*>(block) + sizeof(size_t) + sizeof(void*), val);
    }
    void* blk_parent(void* block) noexcept {
        return read_ptr(static_cast<char*>(block) + sizeof(size_t) + 2 * sizeof(void*));
    }
    void set_blk_parent(void* block, void* val) noexcept {
        write_ptr(static_cast<char*>(block) + sizeof(size_t) + 2 * sizeof(void*), val);
    }

    // указатели prev/next для явного списка свободных блоков (хранятся в payload)
    void* free_prev(void* block) noexcept {
        return read_ptr(static_cast<char*>(block) + sizeof(size_t));
    }
    void set_free_prev(void* block, void* val) noexcept {
        write_ptr(static_cast<char*>(block) + sizeof(size_t), val);
    }
    void* free_next(void* block) noexcept {
        return read_ptr(static_cast<char*>(block) + sizeof(size_t) + sizeof(void*));
    }
    void set_free_next(void* block, void* val) noexcept {
        write_ptr(static_cast<char*>(block) + sizeof(size_t) + sizeof(void*), val);
    }

    // вставить блок в начало списка свободных
    void insert_free(void* trusted, void* block) noexcept {
        void*& first = get_first_free(trusted);
        set_free_prev(block, nullptr);
        set_free_next(block, first);
        if (first != nullptr) {
            set_free_prev(first, block);
        }
        first = block;
    }

    // удалить блок из списка свободных
    void remove_free(void* trusted, void* block) noexcept {
        void* prev = free_prev(block);
        void* next = free_next(block);
        if (prev != nullptr) {
            set_free_next(prev, next);
        } else {
            get_first_free(trusted) = next;
        }
        if (next != nullptr) {
            set_free_prev(next, prev);
        }
    }
}

// ==================== правило пяти ====================

allocator_boundary_tags::~allocator_boundary_tags()
{
    if (_trusted_memory == nullptr) {
        return;
    }
    std::pmr::memory_resource* parent = get_parent(_trusted_memory);
    size_t total = get_space_size(_trusted_memory) + allocator_metadata_size_local;
    get_mtx(_trusted_memory).~mutex();
    if (parent != nullptr) {
        parent->deallocate(_trusted_memory, total, alignof(std::max_align_t));
    } else {
        ::operator delete(_trusted_memory, total, std::align_val_t{alignof(std::max_align_t)});
    }
}

allocator_boundary_tags::allocator_boundary_tags(
    allocator_boundary_tags const &other)
{
    throw not_implemented("allocator_boundary_tags::allocator_boundary_tags(const allocator_boundary_tags &)", "copying is not supported");
}

allocator_boundary_tags &allocator_boundary_tags::operator=(
    allocator_boundary_tags const &other)
{
    throw not_implemented("allocator_boundary_tags::operator=(const allocator_boundary_tags &)", "copying is not supported");
}

allocator_boundary_tags::allocator_boundary_tags(
    allocator_boundary_tags &&other) noexcept
    : _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_boundary_tags &allocator_boundary_tags::operator=(
    allocator_boundary_tags &&other) noexcept
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

// ==================== конструктор ====================

allocator_boundary_tags::allocator_boundary_tags(
        size_t space_size,
        std::pmr::memory_resource *parent_allocator,
        allocator_with_fit_mode::fit_mode allocate_fit_mode)
    : _trusted_memory(nullptr)
{
    if (parent_allocator == nullptr) {
        parent_allocator = std::pmr::get_default_resource();
    }
    _trusted_memory = parent_allocator->allocate(space_size + allocator_metadata_size_local, alignof(std::max_align_t));

    new (static_cast<char*>(_trusted_memory) + mtx_offset) std::mutex();
    get_parent(_trusted_memory) = parent_allocator;
    get_space_size(_trusted_memory) = space_size;
    get_first_free(_trusted_memory) = nullptr;
    get_mode(_trusted_memory) = allocate_fit_mode;

    void* first_block = region_start(_trusted_memory);
    set_blk(first_block, space_size, false);
    set_free_prev(first_block, nullptr);
    set_free_next(first_block, nullptr);
    insert_free(_trusted_memory, first_block);
}

// ==================== allocate / deallocate ====================

[[nodiscard]] void *allocator_boundary_tags::do_allocate_sm(
    size_t size)
{
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory));

    size_t needed = size + occupied_block_metadata_size;
    void* best = nullptr;
    size_t best_sz = 0;

    for (void* curr = get_first_free(_trusted_memory); curr != nullptr; curr = free_next(curr)) {
        size_t curr_sz = blk_size(curr);
        if (curr_sz < needed) {
            continue;
        }
        switch (get_mode(_trusted_memory)) {
            case fit_mode::first_fit:
                best = curr;
                best_sz = curr_sz;
                curr = nullptr; // пометка для выхода
                break;
            case fit_mode::the_best_fit:
                if (best == nullptr || curr_sz < best_sz) {
                    best = curr;
                    best_sz = curr_sz;
                }
                break;
            case fit_mode::the_worst_fit:
                if (best == nullptr || curr_sz > best_sz) {
                    best = curr;
                    best_sz = curr_sz;
                }
                break;
        }
        if (curr == nullptr) {
            break;
        }
    }

    if (best == nullptr) {
        throw std::bad_alloc();
    }

    remove_free(_trusted_memory, best);
    size_t remaining = best_sz - needed;
    if (remaining >= occupied_block_metadata_size) {
        // разбиваем блок
        void* new_free = static_cast<char*>(best) + needed;
        set_blk(new_free, remaining, false);
        set_free_prev(new_free, nullptr);
        set_free_next(new_free, nullptr);
        insert_free(_trusted_memory, new_free);

        set_blk(best, needed, true);
    } else {
        // занимаем весь блок
        set_blk(best, best_sz, true);
    }

    set_blk_prev(best, nullptr);
    set_blk_next(best, nullptr);
    set_blk_parent(best, get_parent(_trusted_memory));

    return static_cast<char*>(best) + occupied_block_metadata_size;
}

void allocator_boundary_tags::do_deallocate_sm(
    void *at)
{
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory));

    void* reg_start = region_start(_trusted_memory);
    void* reg_end = static_cast<char*>(reg_start) + get_space_size(_trusted_memory);

    void* block_start = static_cast<char*>(at) - occupied_block_metadata_size;

    // валидация: block_start должен совпадать с началом одного из блоков
    void* curr = reg_start;
    while (curr < reg_end) {
        if (curr == block_start) {
            break;
        }
        size_t sz = blk_size(curr);
        if (sz == 0 || curr > block_start) {
            throw std::invalid_argument("invalid pointer to deallocate");
        }
        curr = static_cast<char*>(curr) + sz;
    }
    if (curr != block_start || curr >= reg_end) {
        throw std::invalid_argument("invalid pointer to deallocate");
    }
    if (!blk_occupied(block_start)) {
        throw std::invalid_argument("double free or corruption");
    }

    size_t sz = blk_size(block_start);

    // слияние со следующим блоком
    void* next = static_cast<char*>(block_start) + sz;
    if (next < reg_end && !blk_occupied(next)) {
        size_t next_sz = blk_size(next);
        remove_free(_trusted_memory, next);
        sz += next_sz;
    }

    // слияние с предыдущим блоком (линейный поиск с начала)
    void* prev = nullptr;
    curr = reg_start;
    while (curr < block_start) {
        prev = curr;
        size_t prev_sz = blk_size(curr);
        if (prev_sz == 0) {
            throw std::invalid_argument("corrupted block structure");
        }
        curr = static_cast<char*>(curr) + prev_sz;
    }
    if (prev != nullptr && !blk_occupied(prev)) {
        size_t prev_sz = blk_size(prev);
        remove_free(_trusted_memory, prev);
        sz += prev_sz;
        block_start = prev;
    }

    set_blk(block_start, sz, false);
    set_free_prev(block_start, nullptr);
    set_free_next(block_start, nullptr);
    insert_free(_trusted_memory, block_start);
}

// ==================== вспомогательные методы ====================

void allocator_boundary_tags::set_fit_mode(
    allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory));
    get_mode(_trusted_memory) = mode;
}

bool allocator_boundary_tags::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return this == &other;
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info() const
{
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory));
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info_inner() const
{
    std::vector<block_info> result;
    void* curr = region_start(_trusted_memory);
    void* end = static_cast<char*>(curr) + get_space_size(_trusted_memory);
    while (curr < end) {
        size_t sz = blk_size(curr);
        bool occ = blk_occupied(curr);
        result.push_back({sz, occ});
        curr = static_cast<char*>(curr) + sz;
    }
    return result;
}

// ==================== итераторы ====================

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::begin() const noexcept
{
    return boundary_iterator(_trusted_memory);
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::end() const noexcept
{
    return boundary_iterator();
}

// --- boundary_iterator ---

allocator_boundary_tags::boundary_iterator::boundary_iterator()
    : _occupied_ptr(nullptr), _occupied(false), _trusted_memory(nullptr)
{
}

allocator_boundary_tags::boundary_iterator::boundary_iterator(void* trusted)
    : _occupied_ptr(nullptr), _occupied(false), _trusted_memory(trusted)
{
    if (trusted == nullptr) {
        return;
    }
    void* first = region_start(trusted);
    void* end = static_cast<char*>(first) + get_space_size(trusted);
    if (first < end) {
        _occupied = blk_occupied(first);
        _occupied_ptr = _occupied
            ? static_cast<char*>(first) + allocator_boundary_tags::occupied_block_metadata_size
            : static_cast<char*>(first) + sizeof(size_t);
    } else {
        _trusted_memory = nullptr;
    }
}

bool allocator_boundary_tags::boundary_iterator::operator==(
        const boundary_iterator& other) const noexcept
{
    return _occupied_ptr == other._occupied_ptr && _occupied == other._occupied && _trusted_memory == other._trusted_memory;
}

bool allocator_boundary_tags::boundary_iterator::operator!=(
        const boundary_iterator& other) const noexcept
{
    return !(*this == other);
}

allocator_boundary_tags::boundary_iterator& allocator_boundary_tags::boundary_iterator::operator++() & noexcept
{
    if (_occupied_ptr == nullptr || _trusted_memory == nullptr) {
        return *this;
    }
    void* block_start = _occupied
        ? static_cast<char*>(_occupied_ptr) - allocator_boundary_tags::occupied_block_metadata_size
        : static_cast<char*>(_occupied_ptr) - sizeof(size_t);
    size_t sz = blk_size(block_start);
    void* next_block = static_cast<char*>(block_start) + sz;
    void* end = static_cast<char*>(region_start(_trusted_memory)) + get_space_size(_trusted_memory);
    if (next_block >= end) {
        *this = boundary_iterator();
    } else {
        _occupied = blk_occupied(next_block);
        _occupied_ptr = _occupied
            ? static_cast<char*>(next_block) + allocator_boundary_tags::occupied_block_metadata_size
            : static_cast<char*>(next_block) + sizeof(size_t);
    }
    return *this;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator++(int)
{
    boundary_iterator tmp = *this;
    ++(*this);
    return tmp;
}

allocator_boundary_tags::boundary_iterator& allocator_boundary_tags::boundary_iterator::operator--() & noexcept
{
    if (_occupied_ptr == nullptr || _trusted_memory == nullptr) {
        return *this;
    }
    void* block_start = _occupied
        ? static_cast<char*>(_occupied_ptr) - allocator_boundary_tags::occupied_block_metadata_size
        : static_cast<char*>(_occupied_ptr) - sizeof(size_t);
    void* reg_start = region_start(_trusted_memory);
    if (block_start <= reg_start) {
        *this = boundary_iterator();
        return *this;
    }
    void* prev_block = reg_start;
    while (true) {
        size_t prev_sz = blk_size(prev_block);
        void* next = static_cast<char*>(prev_block) + prev_sz;
        if (next >= block_start) {
            break;
        }
        prev_block = next;
    }
    _occupied = blk_occupied(prev_block);
    _occupied_ptr = _occupied
        ? static_cast<char*>(prev_block) + allocator_boundary_tags::occupied_block_metadata_size
        : static_cast<char*>(prev_block) + sizeof(size_t);
    return *this;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator--(int)
{
    boundary_iterator tmp = *this;
    --(*this);
    return tmp;
}

size_t allocator_boundary_tags::boundary_iterator::size() const noexcept
{
    if (_occupied_ptr == nullptr) {
        return 0;
    }
    void* block_start = _occupied
        ? static_cast<char*>(_occupied_ptr) - allocator_boundary_tags::occupied_block_metadata_size
        : static_cast<char*>(_occupied_ptr) - sizeof(size_t);
    return blk_size(block_start);
}

bool allocator_boundary_tags::boundary_iterator::occupied() const noexcept
{
    return _occupied;
}

void* allocator_boundary_tags::boundary_iterator::operator*() const noexcept
{
    return _occupied_ptr;
}

void* allocator_boundary_tags::boundary_iterator::get_ptr() const noexcept
{
    return _occupied_ptr;
}
