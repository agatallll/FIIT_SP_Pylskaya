#include <allocator_boundary_tags.h> // заголовок аллокатора с граничными тегами (boundary tags)
#include <iostream>                  // стандартный поток вывода (объяснялся ранее)
#include <stdexcept>                 // стандартные исключения (объяснялся ранее)

int main()
{
    try // блок перехвата исключений (объяснялся ранее)
    {
        // создаём аллокатор с доверенной памятью 4096 байт, режим first_fit
        allocator_boundary_tags alloc(4096, nullptr, allocator_with_fit_mode::fit_mode::first_fit); // создаём объект allocator_boundary_tags. первый аргумент 4096 — размер доверенной памяти в байтах (пользовательской области, которую аллокатор будет раздавать). второй аргумент nullptr — указатель на родительский аллокатор. nullptr означает, что доверенная память будет запрошена из глобальной кучи через std::pmr::get_default_resource(). третий аргумент allocator_with_fit_mode::fit_mode::first_fit — режим размещения. allocator_with_fit_mode::fit_mode — это enum class (перечисление с областью видимости), объявленное в базовом классе. first_fit — именованная константа: аллокатор будет искать первый подходящий свободный блок.

        // ---------- демонстрация 1: массив целых чисел ----------
        int const n_int = 8; // количество элементов
        void* raw_int = alloc.allocate(sizeof(int) * static_cast<size_t>(n_int)); // выделение памяти (объяснялось ранее)
        int* arr_int = static_cast<int*>(raw_int); // приведение типа (объяснялось ранее)
        for (int i = 0; i < n_int; ++i) {
            arr_int[i] = i * i; // заполняем квадратами чисел
        }
        std::cout << "int array: ";
        for (int i = 0; i < n_int; ++i) {
            std::cout << arr_int[i] << ' ';
        }
        std::cout << '\n';

        // ---------- демонстрация 2: массив double (best fit) ----------
        auto* fit = dynamic_cast<allocator_with_fit_mode*>(&alloc); // dynamic_cast — оператор безопасного приведения типов. здесь мы приводим указатель на allocator_boundary_tags (&alloc) к указателю на базовый класс allocator_with_fit_mode. поскольку allocator_boundary_tags public-наследуется от allocator_with_fit_mode, dynamic_cast гарантированно вернёт корректный указатель. auto* — автоматический вывод типа: компилятор сам определит, что fit имеет тип allocator_with_fit_mode*. dynamic_cast безопасен для полиморфных типов (с виртуальными функциями): если бы приведение было невозможно, вернулся бы nullptr.
        fit->set_fit_mode(allocator_with_fit_mode::fit_mode::the_best_fit); // через указатель на базовый класс вызываем виртуальный метод set_fit_mode(). переключаем режим на the_best_fit: аллокатор теперь будет искать наименьший свободный блок, который достаточно велик для запроса. это минимизирует внутреннюю фрагментацию (оставляет большие блоки нетронутыми).

        size_t n_double = 4; // количество элементов типа double
        void* raw_dbl = alloc.allocate(sizeof(double) * n_double); // выделение памяти
        double* arr_dbl = static_cast<double*>(raw_dbl); // приведение типа
        arr_dbl[0] = 2.71828; // основание натурального логарифма (число e)
        arr_dbl[1] = 3.14159; // число пи
        arr_dbl[2] = 1.41421; // корень из двух
        arr_dbl[3] = 1.73205; // корень из трёх
        std::cout << "double array: ";
        for (size_t i = 0; i < n_double; ++i) {
            std::cout << arr_dbl[i] << ' ';
        }
        std::cout << '\n';

        // ---------- демонстрация 3: строка символов ----------
        char const* src = "boundary tags allocator demo"; // исходная строка
        size_t len = std::char_traits<char>::length(src) + 1; // длина с нуль-терминатором (объяснялось ранее)
        void* raw_str = alloc.allocate(len); // выделение памяти
        char* str = static_cast<char*>(raw_str); // приведение типа
        for (size_t i = 0; i < len; ++i) {
            str[i] = src[i]; // побайтовое копирование
        }
        std::cout << "char string: " << str << '\n';

        // ---------- освобождение в произвольном порядке ----------
        alloc.deallocate(raw_str, len); // освобождение строки (объяснялось ранее)
        alloc.deallocate(raw_dbl, sizeof(double) * n_double); // освобождение массива double
        alloc.deallocate(raw_int, sizeof(int) * static_cast<size_t>(n_int)); // освобождение массива int. аллокатор выполнит слияние (coalesce) соседних свободных блоков, поэтому после всех deallocate внутренняя память снова станет одним большим свободным блоком.

        std::cout << "all blocks deallocated successfully\n";
    }
    catch (std::bad_alloc const& e) { // перехват нехватки памяти (объяснялся ранее)
        std::cerr << "memory allocation failed: " << e.what() << '\n';
        return 1;
    }
    catch (std::logic_error const& e) { // перехват логических ошибок. std::logic_error — базовый класс для ошибок в логике программы (например, not_implemented, invalid_argument, domain_error). здесь он поймает not_implemented, если вдруг случайно вызовется копирование, или invalid_argument из deallocate при неверном указателе.
        std::cerr << "logic error: " << e.what() << '\n';
        return 2;
    }
    catch (std::exception const& e) { // перехват прочих стандартных исключений (объяснялся ранее)
        std::cerr << "standard exception: " << e.what() << '\n';
        return 3;
    }
    catch (...) { // универсальный перехват (объяснялся ранее)
        std::cerr << "unknown exception occurred\n";
        return 4;
    }

    return 0; // успешное завершение
}
