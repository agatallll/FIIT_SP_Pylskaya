#include "../include/allocator_global_heap.h" 
#include <utility>                            // std::move и std::exchange.

allocator_global_heap::allocator_global_heap() : // реализация конструктора по умолчанию.начальные значения полей ДО входа в тело конструкт
    _mutex(std::make_unique<std::mutex>())     
{
    // gecn, в списке инициализации.
}

allocator_global_heap::~allocator_global_heap()
{
    // пуст при уничтожении объекта автоматически вызовется деструктор поля _mutex.
    // деструктор std::unique_ptr удалит мьютекс из динамической памяти. RAII
}

[[nodiscard]] void *allocator_global_heap::do_allocate_sm(
    size_t size)
{
    std::lock_guard<std::mutex> lock(*_mutex); // локальный объект lock типа std::lock_guard. реализующий RAII для мьютекса
                                               
                                               
    
    return ::operator new(size);               // ::operator new — глобальный оператор выделения сырой памяти
                                               
}

void allocator_global_heap::do_deallocate_sm(
    void *at)
{
    std::lock_guard<std::mutex> lock(*_mutex); 
    
    ::operator delete(at);                     
}

allocator_global_heap::allocator_global_heap(const allocator_global_heap &other) :
    allocator_dbg_helper(other),          // конструктор копирования базового класса allocator_dbg_helper
    _mutex(std::make_unique<std::mutex>()) // копия
{
    (void)other; // явное приведение other к void. - предупреждение компилятора «неиспользуемый параметр». параметр other нужен в сигнатуре (контракт копирования)
}

allocator_global_heap &allocator_global_heap::operator=(const allocator_global_heap &other)
{
    if (this != &other) // проверка на самоприсваивание (a = a)
    {
        // объект allocator_global_heap не хранит пользовательских данных (кроме мьютекса
        if (!_mutex) // к умному. если _mutex пустой , то !_mutex вернёт true.
        {
            _mutex = std::make_unique<std::mutex>(); // восстанавливаем мьютекс
    }
    
    return *this; // разыменование this: *this 
}

bool allocator_global_heap::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return dynamic_cast<const allocator_global_heap *>(&other) != nullptr; // dynamic_cast пытается превратить указатель/ссылку базового класса (std::pmr::memory_resource) в указатель на производный класс (allocator_global_heap) вернёт nullptr
}

allocator_global_heap::allocator_global_heap(allocator_global_heap &&other) noexcept :
    allocator_dbg_helper(std::move(other)), // вызываем перемещающий конструктор базового класса rvalue
    _mutex(other._mutex.release())          // other._mutex.release() — метод unique_ptr::release() отдаёт сырой указатель на управляемый объект и обнуляет other._mutex (теперь other не владеет мьютексом). этот сырой указатель передаётся в конструктор _mutex текущего объекта. текущий объект забирает владение мьютексом у other.
{
    // other остаётся с пустым unique_ptr. это корректное after-move состояние: объект валиден, но не содержит ресурса.
}

allocator_global_heap &allocator_global_heap::operator=(allocator_global_heap &&other) noexcept
{
    if (this != &other) // защита от самоперемещения: a = std::move(a). без этой проверки мы могли бы случайно удалить свой собственный мьютекс через reset().
    {
        _mutex.reset(other._mutex.release()); // other._mutex.release() — забираем указатель у other, оставляя other с пустым unique_ptr. _mutex.reset(...) — метод unique_ptr::reset удаляет старый объект (если _mutex не пуст) и начинает владеть новым указателем. так мы передаём владение мьютексом от other к текущему объекту.
    }
    
    return *this; // возвращаем ссылку на текущий объект для цепочек присваивания.
}
