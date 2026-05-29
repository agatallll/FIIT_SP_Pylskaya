#include <allocator_sorted_list.h> // заголовок аллокатора на отсортированном списке свободных блоков
#include <iostream>                // поток вывода (объяснялся ранее)
#include <stdexcept>               // исключения (объяснялся ранее)
#include <string>                  // строковый тип (объяснялся ранее)

int main()
{
    try // глобальный перехват исключений (объяснялся ранее)
    {
        // создаём аллокатор с доверенной памятью 4096 байт, first_fit по умолчанию
        allocator_sorted_list alloc(4096, nullptr, allocator_with_fit_mode::fit_mode::first_fit); // создаём аллокатор с доверенной памятью 4096 байт. nullptr — родительский аллокатор не указан, память берётся из глобальной кучи. fit_mode::first_fit — начальный режим: первый подходящий свободный блок. внутри аллокатора создаётся единственный свободный блок на всю доверенную память за вычетом служебного заголовка.

        // ---------- демонстрация 1: массив целых чисел ----------
        int const int_count = 10;
        void* raw_int = alloc.allocate(sizeof(int) * static_cast<size_t>(int_count)); // выделение памяти (объяснялось ранее)
        int* int_arr = static_cast<int*>(raw_int); // приведение типа (объяснялось ранее)
        for (int i = 0; i < int_count; ++i) {
            int_arr[i] = i * i; // заполняем квадратами индексов
        }
        std::cout << "int array: ";
        for (int i = 0; i < int_count; ++i) {
            std::cout << int_arr[i] << ' ';
        }
        std::cout << '\n';

        // ---------- демонстрация 2: массив double (best fit) ----------
        auto* fit_iface = dynamic_cast<allocator_with_fit_mode*>(&alloc); // dynamic_cast к базовому классу (объяснялось ранее). auto* — автоматический вывод типа указателя.
        fit_iface->set_fit_mode(allocator_with_fit_mode::fit_mode::the_best_fit); // переключаем режим на the_best_fit. теперь аллокатор будет искать наименьший подходящий свободный блок, оставляя большие блоки нетронутыми для более крупных запросов.

        size_t double_count = 5;
        void* raw_dbl = alloc.allocate(sizeof(double) * double_count); // выделение памяти
        double* dbl_arr = static_cast<double*>(raw_dbl); // приведение типа
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
        char const* src = "sorted list allocator demo"; // исходная строка
        size_t str_len = std::char_traits<char>::length(src) + 1; // длина с нуль-терминатором (объяснялось ранее)
        void* raw_str = alloc.allocate(str_len); // выделение памяти
        char* str = static_cast<char*>(raw_str); // приведение типа
        for (size_t i = 0; i < str_len; ++i) {
            str[i] = src[i]; // побайтовое копирование
        }
        std::cout << "char string: " << str << '\n';

        // ---------- освобождение в произвольном порядке ----------
        alloc.deallocate(raw_str, str_len);          // освобождаем строку. аллокатор проверит принадлежность указателя, найдёт блок в отсортированном списке свободных, вставит его в правильное место (сохраняя сортировку по адресу) и выполнит слияние (coalesce) со смежными свободными блоками.
        alloc.deallocate(raw_dbl, sizeof(double) * double_count); // освобождаем double. после этого произойдёт слияние с соседними свободными блоками, если они примыкают вплотную.
        alloc.deallocate(raw_int, sizeof(int) * static_cast<size_t>(int_count)); // освобождаем int. после всех deallocate вся память снова станет одним большим свободным блоком (благодаря coalescing).

        std::cout << "all blocks deallocated successfully\n";
    }
    catch (std::bad_alloc const& e) { // нехватка памяти (объяснялось ранее)
        std::cerr << "memory allocation failed: " << e.what() << '\n';
        return 1;
    }
    catch (std::logic_error const& e) { // логические ошибки: invalid pointer, double free, corrupted structure (объяснялось ранее)
        std::cerr << "logic error: " << e.what() << '\n';
        return 2;
    }
    catch (std::exception const& e) { // прочие стандартные исключения (объяснялось ранее)
        std::cerr << "standard exception: " << e.what() << '\n';
        return 3;
    }
    catch (...) { // неизвестные исключения (объяснялось ранее)
        std::cerr << "unknown exception occurred\n";
        return 4;
    }

    return 0; // успешное завершение
}
