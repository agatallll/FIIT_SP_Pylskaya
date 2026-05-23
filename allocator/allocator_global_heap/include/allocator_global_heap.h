#ifndef MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_GLOBAL_HEAP_H
#define MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_GLOBAL_HEAP_H

#include <allocator_dbg_helper.h> // базовый класс с отладочными утилитами аллокатора
#include <pp_allocator.h>         // интерфейс polymorphic memory resource
#include <mutex>                  // заголовок примитива синхронизации потоков
#include <memory>                 // заголовок для управления динамической памятью (std::unique_ptr)

class allocator_global_heap final: // финальный класс аллокатора глобальной кучи
    private allocator_dbg_helper,  // наследуем отладочный функционал (пустая реализация)
    public smart_mem_resource      // наследуем стандартный интерфейс памяти
{

private:

    static constexpr const size_t size_t_size = sizeof(size_t); // размер size_t в байтах (сохранено для совместимости интерфейса)

    std::unique_ptr<std::mutex> _mutex; // указатель на мьютекс для обеспечения потокобезопасности объекта

public:
    
    explicit allocator_global_heap(); // конструктор по умолчанию, инициализирует мьютекс
    
    ~allocator_global_heap() override; // деструктор, освобождает мьютекс через unique_ptr
    
    allocator_global_heap(
        allocator_global_heap const &other); // конструктор копирования, создаёт независимый мьютекс
    
    allocator_global_heap &operator=(
        allocator_global_heap const &other); // оператор копирующего присваивания (состояние кучи неизменно)
    
    allocator_global_heap(
        allocator_global_heap &&other) noexcept; // конструктор перемещения, переносит владение мьютексом
    
    allocator_global_heap &operator=(
        allocator_global_heap &&other) noexcept; // оператор перемещающего присваивания, переносит владение мьютексом

private:
    
    [[nodiscard]] void *do_allocate_sm(
        size_t size) override; // выделение блока из глобальной кучи (синхронизировано)
    
    void do_deallocate_sm(
        void *at) override; // освобождение блока в глобальную кучу (синхронизировано)

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override; // проверка эквивалентности ресурсов

};

#endif //MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_GLOBAL_HEAP_H
