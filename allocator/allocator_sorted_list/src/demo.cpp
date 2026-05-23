#include <allocator_sorted_list.h> // заголовок реализованного аллокатора на отсортированном списке
#include <iostream>                // поток вывода для демонстрации результатов
#include <stdexcept>               // стандартные исключения
#include <string>                  // строковый тип для сообщений

int main()
{
    try // глобальный перехват: приложение не должно аварийно завершаться
    {
        // создаём аллокатор с доверенной памятью 4096 байт, first_fit по умолчанию
        allocator_sorted_list alloc(4096, nullptr, allocator_with_fit_mode::fit_mode::first_fit);

        // ---------- демонстрация 1: массив целых чисел ----------
        int const int_count = 10;
        void* raw_int = alloc.allocate(sizeof(int) * static_cast<size_t>(int_count)); // выделяем память под int
        int* int_arr = static_cast<int*>(raw_int);
        for (int i = 0; i < int_count; ++i) {
            int_arr[i] = i * i; // заполняем квадратами индексов
        }
        std::cout << "int array: ";
        for (int i = 0; i < int_count; ++i) {
            std::cout << int_arr[i] << ' ';
        }
        std::cout << '\n';

        // ---------- демонстрация 2: массив double (best fit) ----------
        auto* fit_iface = dynamic_cast<allocator_with_fit_mode*>(&alloc);
        fit_iface->set_fit_mode(allocator_with_fit_mode::fit_mode::the_best_fit); // переключаем режим

        size_t double_count = 5;
        void* raw_dbl = alloc.allocate(sizeof(double) * double_count);
        double* dbl_arr = static_cast<double*>(raw_dbl);
        dbl_arr[0] = 3.14159;
        dbl_arr[1] = 2.71828;
        dbl_arr[2] = 1.41421;
        dbl_arr[3] = 1.73205;
        dbl_arr[4] = 2.23607;
        std::cout << "double array: ";
        for (size_t i = 0; i < double_count; ++i) {
            std::cout << dbl_arr[i] << ' ';
        }
        std::cout << '\n';

        // ---------- демонстрация 3: строка символов ----------
        char const* src = "sorted list allocator demo";
        size_t str_len = std::char_traits<char>::length(src) + 1; // с нуль-терминатором
        void* raw_str = alloc.allocate(str_len);
        char* str = static_cast<char*>(raw_str);
        for (size_t i = 0; i < str_len; ++i) {
            str[i] = src[i]; // побайтовое копирование
        }
        std::cout << "char string: " << str << '\n';

        // ---------- освобождение в произвольном порядке ----------
        alloc.deallocate(raw_str, str_len);          // строка
        alloc.deallocate(raw_dbl, sizeof(double) * double_count); // double
        alloc.deallocate(raw_int, sizeof(int) * static_cast<size_t>(int_count)); // int

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
