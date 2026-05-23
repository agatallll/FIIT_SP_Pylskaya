#include <not_implemented.h>
#include "../include/allocator_buddies_system.h"
#include <mutex>
#include <memory_resource>
#include <stdexcept>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <cstring>

namespace {
    // размер метаданных аллокатора (без padding, ручные смещения)
    constexpr size_t allocator_metadata_size_local = sizeof(allocator_dbg_helper*)
                                                   + sizeof(allocator_with_fit_mode::fit_mode)
                                                   + sizeof(unsigned char)
                                                   + sizeof(std::mutex);

    // смещения полей метаданных аллокатора
    constexpr size_t mtx_offset = 0;                                           // std::mutex
    constexpr size_t parent_offset = sizeof(std::mutex);                       // std::pmr::memory_resource*
    constexpr size_t mode_offset = parent_offset + sizeof(allocator_dbg_helper*); // fit_mode
    constexpr size_t max_k_offset = mode_offset + sizeof(allocator_with_fit_mode::fit_mode); // unsigned char

    std::mutex& get_mtx(void* trusted) noexcept {
        return *reinterpret_cast<std::mutex*>(static_cast<char*>(trusted) + mtx_offset);
    }
    std::pmr::memory_resource*& get_parent(void* trusted) noexcept {
        return *reinterpret_cast<std::pmr::memory_resource**>(static_cast<char*>(trusted) + parent_offset);
    }
    allocator_with_fit_mode::fit_mode& get_mode(void* trusted) noexcept {
        return *reinterpret_cast<allocator_with_fit_mode::fit_mode*>(static_cast<char*>(trusted) + mode_offset);
    }
    unsigned char& get_max_k(void* trusted) noexcept {
        return *reinterpret_cast<unsigned char*>(static_cast<char*>(trusted) + max_k_offset);
    }

    // начало области блоков (сразу за метаданными аллокатора)
    void* region_start(void* trusted) noexcept {
        return static_cast<char*>(trusted) + allocator_metadata_size_local;
    }

    // безопасное чтение/запись void* через memcpy
    // (адреса блоков могут быть невыровненными из-за allocator_metadata_size = 93)
    void* read_ptr(void* addr) noexcept {
        void* val;
        std::memcpy(&val, addr, sizeof(val));
        return val;
    }
    void write_ptr(void* addr, void* val) noexcept {
        std::memcpy(addr, &val, sizeof(val));
    }
}

// ==================== правило пяти ====================

allocator_buddies_system::~allocator_buddies_system()
{
    if (_trusted_memory == nullptr) {
        return;
    }
    std::pmr::memory_resource* parent = get_parent(_trusted_memory);
    unsigned char max_k_val = get_max_k(_trusted_memory);
    size_t total_size = (static_cast<size_t>(1) << max_k_val) + allocator_metadata_size_local;
    get_mtx(_trusted_memory).~mutex();
    if (parent != nullptr) {
        parent->deallocate(_trusted_memory, total_size, alignof(std::max_align_t));
    } else {
        ::operator delete(_trusted_memory, total_size, std::align_val_t{alignof(std::max_align_t)});
    }
}

allocator_buddies_system::allocator_buddies_system(
    allocator_buddies_system const &other)
{
    throw not_implemented("allocator_buddies_system::allocator_buddies_system(const allocator_buddies_system &)", "copying is not supported");
}

allocator_buddies_system &allocator_buddies_system::operator=(
    allocator_buddies_system const &other)
{
    throw not_implemented("allocator_buddies_system::operator=(const allocator_buddies_system &)", "copying is not supported");
}

allocator_buddies_system::allocator_buddies_system(
    allocator_buddies_system &&other) noexcept
    : _trusted_memory(other._trusted_memory)
{
    other._trusted_memory = nullptr;
}

allocator_buddies_system &allocator_buddies_system::operator=(
    allocator_buddies_system &&other) noexcept
{
    if (this != &other) {
        if (_trusted_memory != nullptr) {
            std::pmr::memory_resource* parent = get_parent(_trusted_memory);
            unsigned char max_k_val = get_max_k(_trusted_memory);
            size_t total_size = (static_cast<size_t>(1) << max_k_val) + allocator_metadata_size_local;
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
    : _trusted_memory(nullptr)
{
    if (parent_allocator == nullptr) {
        parent_allocator = std::pmr::get_default_resource();
    }
    if (space_size < (static_cast<size_t>(1) << min_k)) {
        throw std::logic_error("space size is too small for buddy system");
    }

    // округляем размер до ближайшей степени двойки
    size_t max_k_val = min_k;
    size_t pow2 = static_cast<size_t>(1) << min_k;
    while (pow2 < space_size) {
        ++max_k_val;
        pow2 <<= 1;
    }

    size_t total_size = pow2 + allocator_metadata_size_local;
    _trusted_memory = parent_allocator->allocate(total_size, alignof(std::max_align_t));

    new (static_cast<char*>(_trusted_memory) + mtx_offset) std::mutex();
    get_parent(_trusted_memory) = parent_allocator;
    get_mode(_trusted_memory) = allocate_fit_mode;
    get_max_k(_trusted_memory) = static_cast<unsigned char>(max_k_val);

    void* first = region_start(_trusted_memory);
    block_metadata meta;
    meta.occupied = false;
    meta.size = static_cast<unsigned char>(max_k_val);
    std::memcpy(first, &meta, sizeof(meta));
}

// ==================== allocate / deallocate ====================

[[nodiscard]] void *allocator_buddies_system::do_allocate_sm(
    size_t size)
{
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory));

    size_t total_needed = size + occupied_block_metadata_size;
    unsigned char k_needed = min_k;
    while ((static_cast<size_t>(1) << k_needed) < total_needed) {
        ++k_needed;
    }

    unsigned char max_k_val = get_max_k(_trusted_memory);
    void* reg_start = region_start(_trusted_memory);
    void* reg_end = static_cast<char*>(reg_start) + (static_cast<size_t>(1) << max_k_val);

    void* best = nullptr;
    unsigned char best_k = 0;

    for (void* curr = reg_start; curr < reg_end; ) {
        block_metadata meta; std::memcpy(&meta, curr, sizeof(meta));
        unsigned char k = meta.size;
        if (!meta.occupied && k >= k_needed) {
            switch (get_mode(_trusted_memory)) {
                case fit_mode::first_fit:
                    best = curr;
                    best_k = k;
                    curr = reg_end; // выход из цикла
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
            curr = static_cast<char*>(curr) + (static_cast<size_t>(1) << k);
        }
    }

    if (best == nullptr) {
        throw std::bad_alloc();
    }

    // разбиваем блок до нужного размера
    while (best_k > k_needed) {
        --best_k;
        size_t half = static_cast<size_t>(1) << best_k;
        void* buddy = static_cast<char*>(best) + half;
        block_metadata buddy_meta;
        buddy_meta.occupied = false;
        buddy_meta.size = best_k;
        std::memcpy(buddy, &buddy_meta, sizeof(buddy_meta));

        block_metadata best_meta;
        best_meta.occupied = false;
        best_meta.size = best_k;
        std::memcpy(best, &best_meta, sizeof(best_meta));
    }

    block_metadata occupied_meta;
    occupied_meta.occupied = true;
    occupied_meta.size = best_k;
    std::memcpy(best, &occupied_meta, sizeof(occupied_meta));

    write_ptr(static_cast<char*>(best) + sizeof(block_metadata), get_parent(_trusted_memory));

    return static_cast<char*>(best) + occupied_block_metadata_size;
}

void allocator_buddies_system::do_deallocate_sm(
    void *at)
{
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory));

    void* block_start = static_cast<char*>(at) - occupied_block_metadata_size;

    unsigned char max_k_val = get_max_k(_trusted_memory);
    void* reg_start = region_start(_trusted_memory);
    void* reg_end = static_cast<char*>(reg_start) + (static_cast<size_t>(1) << max_k_val);

    // валидация: block_start должен быть началом одного из блоков
    void* curr = reg_start;
    while (curr < reg_end) {
        if (curr == block_start) {
            break;
        }
        block_metadata meta; std::memcpy(&meta, curr, sizeof(meta));
        size_t sz = static_cast<size_t>(1) << meta.size;
        if (sz == 0 || curr > block_start) {
            throw std::invalid_argument("invalid pointer to deallocate");
        }
        curr = static_cast<char*>(curr) + sz;
    }
    if (curr != block_start || curr >= reg_end) {
        throw std::invalid_argument("invalid pointer to deallocate");
    }

    block_metadata meta; std::memcpy(&meta, block_start, sizeof(meta));
    if (!meta.occupied) {
        throw std::invalid_argument("double free or corruption");
    }

    unsigned char k = meta.size;

    // слияние с buddy
    while (true) {
        size_t block_size = static_cast<size_t>(1) << k;
        uintptr_t offset = reinterpret_cast<uintptr_t>(block_start) - reinterpret_cast<uintptr_t>(reg_start);
        uintptr_t buddy_offset = offset ^ block_size;
        void* buddy = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(reg_start) + buddy_offset);

        if (buddy < reg_start || buddy >= reg_end) {
            break;
        }
        block_metadata buddy_meta; std::memcpy(&buddy_meta, buddy, sizeof(buddy_meta));
        if (buddy_meta.occupied || buddy_meta.size != k) {
            break;
        }
        if (buddy < block_start) {
            block_start = buddy;
        }
        ++k;
    }

    block_metadata free_meta;
    free_meta.occupied = false;
    free_meta.size = k;
    std::memcpy(block_start, &free_meta, sizeof(free_meta));
}

// ==================== вспомогательные методы ====================

void allocator_buddies_system::set_fit_mode(
    allocator_with_fit_mode::fit_mode mode)
{
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory));
    get_mode(_trusted_memory) = mode;
}

bool allocator_buddies_system::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return this == &other;
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info() const noexcept
{
    std::lock_guard<std::mutex> lock(get_mtx(_trusted_memory));
    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_buddies_system::get_blocks_info_inner() const
{
    std::vector<block_info> result;
    unsigned char max_k_val = get_max_k(_trusted_memory);
    void* curr = region_start(_trusted_memory);
    void* end = static_cast<char*>(curr) + (static_cast<size_t>(1) << max_k_val);
    while (curr < end) {
        block_metadata meta; std::memcpy(&meta, curr, sizeof(meta));
        size_t sz = static_cast<size_t>(1) << meta.size;
        result.push_back({sz, meta.occupied});
        curr = static_cast<char*>(curr) + sz;
    }
    return result;
}

// ==================== итераторы ====================

allocator_buddies_system::buddy_iterator allocator_buddies_system::begin() const noexcept
{
    return buddy_iterator(region_start(_trusted_memory));
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::end() const noexcept
{
    unsigned char max_k_val = get_max_k(_trusted_memory);
    void* reg_start = region_start(_trusted_memory);
    void* reg_end = static_cast<char*>(reg_start) + (static_cast<size_t>(1) << max_k_val);
    return buddy_iterator(reg_end);
}

// --- buddy_iterator ---

allocator_buddies_system::buddy_iterator::buddy_iterator()
    : _block(nullptr)
{
}

allocator_buddies_system::buddy_iterator::buddy_iterator(void* start)
    : _block(start)
{
}

bool allocator_buddies_system::buddy_iterator::operator==(
        const buddy_iterator& other) const noexcept
{
    return _block == other._block;
}

bool allocator_buddies_system::buddy_iterator::operator!=(
        const buddy_iterator& other) const noexcept
{
    return _block != other._block;
}

allocator_buddies_system::buddy_iterator& allocator_buddies_system::buddy_iterator::operator++() & noexcept
{
    if (_block == nullptr) {
        return *this;
    }
    block_metadata meta; std::memcpy(&meta, _block, sizeof(meta));
    _block = static_cast<char*>(_block) + (static_cast<size_t>(1) << meta.size);
    return *this;
}

allocator_buddies_system::buddy_iterator allocator_buddies_system::buddy_iterator::operator++(int)
{
    buddy_iterator tmp = *this;
    ++(*this);
    return tmp;
}

size_t allocator_buddies_system::buddy_iterator::size() const noexcept
{
    if (_block == nullptr) {
        return 0;
    }
    block_metadata meta; std::memcpy(&meta, _block, sizeof(meta));
    return static_cast<size_t>(1) << meta.size;
}

bool allocator_buddies_system::buddy_iterator::occupied() const noexcept
{
    if (_block == nullptr) {
        return false;
    }
    block_metadata meta; std::memcpy(&meta, _block, sizeof(meta));
    return meta.occupied;
}

void* allocator_buddies_system::buddy_iterator::operator*() const noexcept
{
    if (_block == nullptr) {
        return nullptr;
    }
    return static_cast<char*>(_block) + allocator_buddies_system::occupied_block_metadata_size;
}
