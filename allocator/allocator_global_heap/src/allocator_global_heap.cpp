#include "../include/allocator_global_heap.h" // собственный заголовок с объявлением класса
#include <utility>                            // заголовок для вспомогательных функций (std::exchange и др.)

allocator_global_heap::allocator_global_heap() :
    _mutex(std::make_unique<std::mutex>()) // создаём мьютекс в динамической памяти для синхронизации потоков
{
    // при нехватке памяти std::make_unique выбросит std::bad_alloc, что позволит вызывающему коду обработать ошибку
}

allocator_global_heap::~allocator_global_heap()
{
    // мьютекс освобождается автоматически деструктором unique_ptr,
    // глобальная куча не требует очистки при уничтожении аллокатора
}

[[nodiscard]] void *allocator_global_heap::do_allocate_sm(
    size_t size)
{
    std::lock_guard<std::mutex> lock(*_mutex); // захватываем мьютекс: гарантируем, что в одном объекте
                                               // выделение/освобождение выполняется максимум в одном потоке
    
    return ::operator new(size);               // делегируем выделение глобальному оператору new;
                                               // при нехватке памяти будет выброшено std::bad_alloc
}

void allocator_global_heap::do_deallocate_sm(
    void *at)
{
    std::lock_guard<std::mutex> lock(*_mutex); // синхронизируем доступ к освобождению памяти
    
    ::operator delete(at);                     // возвращаем блок в глобальную кучу;
                                               // передача nullptr корректно обрабатывается как no-op
}

allocator_global_heap::allocator_global_heap(const allocator_global_heap &other) :
    allocator_dbg_helper(other),          // инициализируем базовый отладочный класс (пустая операция)
    _mutex(std::make_unique<std::mutex>()) // создаём новый независимый мьютекс для копии объекта
{
    (void)other; // явно подавляем предупреждение о неиспользуемом параметре (состояния global_heap нет)
}

allocator_global_heap &allocator_global_heap::operator=(const allocator_global_heap &other)
{
    if (this != &other) // проверяем самоприсваивание, чтобы избежать лишних операций
    {
        // объект allocator_global_heap не хранит пользовательских данных,
        // поэтому достаточно оставить текущий мьютекс нетронутым;
        // если мьютекс был украден перемещением, создаём новый
        if (!_mutex)
        {
            _mutex = std::make_unique<std::mutex>(); // восстанавливаем мьютекс при необходимости
        }
    }
    
    return *this; // возвращаем ссылку на текущий объект по стандарту c++
}

bool allocator_global_heap::do_is_equal(const std::pmr::memory_resource &other) const noexcept
{
    return dynamic_cast<const allocator_global_heap *>(&other) != nullptr; // все экземпляры данного типа считаем
                                                                           // эквивалентными (общая глобальная куча)
}

allocator_global_heap::allocator_global_heap(allocator_global_heap &&other) noexcept :
    allocator_dbg_helper(std::move(other)), // перемещаем базовый класс (формально)
    _mutex(other._mutex.release())          // забираем владение мьютексом у источника,
                                            // оставляя other с нулевым unique_ptr
{
}

allocator_global_heap &allocator_global_heap::operator=(allocator_global_heap &&other) noexcept
{
    if (this != &other) // защита от самоперемещения
    {
        _mutex.reset(other._mutex.release()); // освобождаем старый мьютекс (если есть) и забираем чужой
    }
    
    return *this; // возвращаем ссылку на текущий объект
}
