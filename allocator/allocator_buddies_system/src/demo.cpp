#include <allocator_buddies_system.h> // заголовок реализованного аллокатора
#include <iostream>                  // стандартный поток вывода
#include <stdexcept>                 // стандартные исключения

int main()
{
    try // глобальный перехват исключений
    {
        // создаём аллокатор с доверенной памятью 4096 байт, режим first_fit
        allocator_buddies_system alloc(4096, nullptr, allocator_with_fit_mode::fit_mode::first_fit);

        // ---------- демонстрация 1: массив целых чисел ----------
        int const n_int = 8;
        void* raw_int = alloc.allocate(sizeof(int) * static_cast<size_t>(n_int));
        int* arr_int = static_cast<int*>(raw_int);
        for (int i = 0; i < n_int; ++i) {
            arr_int[i] = i * i; // заполняем квадратами
        }
        std::cout << "int array: ";
        for (int i = 0; i < n_int; ++i) {
            std::cout << arr_int[i] << ' ';
        }
        std::cout << '\n';

        // ---------- демонстрация 2: массив double (best fit) ----------
        auto* fit = dynamic_cast<allocator_with_fit_mode*>(&alloc);
        fit->set_fit_mode(allocator_with_fit_mode::fit_mode::the_best_fit);

        size_t n_double = 4;
        void* raw_dbl = alloc.allocate(sizeof(double) * n_double);
        double* arr_dbl = static_cast<double*>(raw_dbl);
        arr_dbl[0] = 2.71828;
        arr_dbl[1] = 3.14159;
        arr_dbl[2] = 1.41421;
        arr_dbl[3] = 1.73205;
        std::cout << "double array: ";
        for (size_t i = 0; i < n_double; ++i) {
            std::cout << arr_dbl[i] << ' ';
        }
        std::cout << '\n';

        // ---------- демонстрация 3: строка символов ----------
        char const* src = "buddy system allocator demo";
        size_t len = std::char_traits<char>::length(src) + 1;
        void* raw_str = alloc.allocate(len);
        char* str = static_cast<char*>(raw_str);
        for (size_t i = 0; i < len; ++i) {
            str[i] = src[i];
        }
        std::cout << "char string: " << str << '\n';

        // ---------- освобождение в произвольном порядке ----------
        alloc.deallocate(raw_str, len);
        alloc.deallocate(raw_dbl, sizeof(double) * n_double);
        alloc.deallocate(raw_int, sizeof(int) * static_cast<size_t>(n_int));

        std::cout << "all blocks deallocated successfully\n";
    }
    catch (std::bad_alloc const& e) {
        std::cerr << "memory allocation failed: " << e.what() << '\n';
        return 1;
    }
    catch (std::logic_error const& e) {
        std::cerr << "logic error: " << e.what() << '\n';
        return 2;
    }
    catch (std::exception const& e) {
        std::cerr << "standard exception: " << e.what() << '\n';
        return 3;
    }
    catch (...) {
        std::cerr << "unknown exception occurred\n";
        return 4;
    }

    return 0;
}
