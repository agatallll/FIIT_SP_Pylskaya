#include <allocator_buddies_system.h> // заголовок аллокатора системы двойников 
#include <iostream>                 // стандартный поток вывода 
#include <stdexcept>                // стандартные исключени

int main()
{
    try // перехват исключений 
    {
        // создаём аллокатор с доверенной памятью 4096 байт, режим first_fit
        allocator_buddies_system alloc(4096, nullptr, allocator_with_fit_mode::fit_mode::first_fit); // 4096 = 2^12 max_k = 12 nullptr — родитель не указан, память из глобальной кучи

    
        int const n_int = 8; // количество элементов
        void* raw_int = alloc.allocate(sizeof(int) * static_cast<size_t>(n_int)); // при выделении аллокатор найдёт первый подходящий блок, разобьёт его пополам несколько раз
        int* arr_int = static_cast<int*>(raw_int); // приведение типа 
        for (int i = 0; i < n_int; ++i) {
            arr_int[i] = i * i; // заполняем квадратами индексов
        }
        std::cout << "int array: ";
        for (int i = 0; i < n_int; ++i) {
            std::cout << arr_int[i] << ' ';
        }
        std::cout << '\n';

        // массив double (best fit)
        auto* fit = dynamic_cast<allocator_with_fit_mode*>(&alloc); // dynamic_cast к базовому классу 
        fit->set_fit_mode(allocator_with_fit_mode::fit_mode::the_best_fit); // переключаем режим на the_best_fit 

        size_t n_double = 4; // количество элементов
        void* raw_dbl = alloc.allocate(sizeof(double) * n_double); // выделение памяти
        double* arr_dbl = static_cast<double*>(raw_dbl); // приведение типа
        arr_dbl[0] = 2.71828; // число e
        arr_dbl[1] = 3.14159; // число π
        arr_dbl[2] = 1.41421; // √2
        arr_dbl[3] = 1.73205; // √3
        std::cout << "double array: ";
        for (size_t i = 0; i < n_double; ++i) {
            std::cout << arr_dbl[i] << ' ';
        }
        std::cout << '\n';

        // строка символов
        char const* src = "buddy system allocator demo"; // исходная строка
        size_t len = std::char_traits<char>::length(src) + 1; // длина с нуль-терминатором 
        void* raw_str = alloc.allocate(len); // выделение памяти
        char* str = static_cast<char*>(raw_str); // приведение типа
        for (size_t i = 0; i < len; ++i) {
            str[i] = src[i]; // побайтовое копирование
        }
        std::cout << "char string: " << str << '\n';

        // освобождение в произвольном поряд
        alloc.deallocate(raw_str, len);          // освобождаем строку. аллокатор вычислит buddy через XOR-смещения и, если buddy свободен, выполнит слияние (coalesce).
        alloc.deallocate(raw_dbl, sizeof(double) * n_double); // освобождаем double. возможно слияние с соседними свободными блоками.
        alloc.deallocate(raw_int, sizeof(int) * static_cast<size_t>(n_int)); // освобождаем int. после всех deallocate блоки могут сливаться обратно в один большой блок 4096 байт.

        std::cout << "all blocks deallocated successfully\n";
    }
    catch (std::bad_alloc const& e) { // нехватка памяти 
        std::cerr << "memory allocation failed: " << e.what() << '\n';
        return 1;
    }
    catch (std::logic_error const& e) { // логические ошибки (например, space size too small) 
        std::cerr << "logic error: " << e.what() << '\n';
        return 2;
    }
    catch (std::exception const& e) { // прочие стандартные исключени
        std::cerr << "standard exception: " << e.what() << '\n';
        return 3;
    }
    catch (...) { // неизвестные исключени
        std::cerr << "unknown exception occurred\n";
        return 4;
    }

    return 0; // успешное завершение
}
