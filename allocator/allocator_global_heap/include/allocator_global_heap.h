#ifndef MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_GLOBAL_HEAP_H // директива препроцессора: если макрос ещё не определён ltkfnm вает файл до компиляции.
#define MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_GLOBAL_HEAP_H //макрос-флаг, чтобы при повторном включении этого файла #ifndef сработал как «уже было» include guard.

#include <allocator_dbg_helper.h> // препроцессор вставляет содержимое заголовка с базовым классом отладки
#include <pp_allocator.h>         // smart 0em_resource — прослойка между аллокатором и стандартным std::pmr::memory_resource из c++17
#include <mutex>                  // std::мьютекс 
#include <memory>                 //умный указатель std::unique_pt

class allocator_global_heap final: //final запрещает наследование от этого класса.
    private allocator_dbg_helper,  // 
    public smart_mem_resource      
{

private:

    static constexpr const size_t size_t_size = sizeof(size_t); // constexpr — значение вычисляется на этапе компиляции. const — нельзя изменить. 

    std::unique_ptr<std::mutex> _mutex;  
                                        // std::mutex нельзя копировать и перемещать. 

public:
    
    explicit allocator_global_heap(); //explicit запрещает неявное автоматическое преобразование типов компилятором
    
    ~allocator_global_heap() override; // деструктор — автоматически вызывается при уничтожении, переопределять метод базового класса
    
    allocator_global_heap(
        allocator_global_heap const &other); 
    
    allocator_global_heap &operator=(
        allocator_global_heap const &other); // оператор присваивания копированием. вызывается, когда объект уже существует, ссылку на текущий объект, чтобы можно было писать цепочки: a = b = c;.
    
    allocator_global_heap(
        allocator_global_heap &&other) noexcept; // конструктор пер вызывается при allocator_global_heap a = std::move(b);. noexcept — обещание, что метод не бросает исключений; это позволяет стандартным контейнерам (vector) перемещать объект вместо копирования.
    
    allocator_global_heap &operator=(
        allocator_global_heap &&other) noexcept; // оператор присваивания перемещением. a = std::move(b);. забирает ресурсы (мьютекс) у other

private:
    
    [[nodiscard]] void *do_allocate_sm(
        size_t size) override; // функция выделения памяти nodiscard атрибут: компилятор выдаст предупреждение, если результат функции (указатель) будет проигнорирован
    
    void do_deallocate_sm(
        void *at) override; // void *at — указатель на блок, который нужно вернуть системе. override — переопределяем метод из smart_mem_resource.

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override; // проверка эквивалентности ресурсов памяти. const после скобок означает, что метод не изменяет поля объекта. noexcept — не бросает исключений. std::pmr::memory_resource& — константная ссылка на базовый класс стандартного полиморфного ресурса памяти. bool — возвращает true/false. override — переопределение.

};

#endif //MATH_PRACTICE_AND_OPERATING_SYSTEMS_ALLOCATOR_ALLOCATOR_GLOBAL_HEAP_H
