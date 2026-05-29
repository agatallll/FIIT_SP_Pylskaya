#include <allocator_red_black_tree.h> // заголовок аллокатора на красно-чёрном дереве
#include <iostream>                 // стандартный поток вывода (объяснялся ранее)
#include <stdexcept>                // стандартные исключения (объяснялся ранее)

int main()
{
    try { // глобальный перехват исключений (объяснялся ранее)
        allocator_red_black_tree alloc(4096, nullptr, allocator_with_fit_mode::fit_mode::first_fit); // создаём аллокатор с доверенной памятью 4096 байт. nullptr — родитель не указан, память из глобальной кучи. внутри создаётся одно большое свободное блок размером 4096, который вставляется в красно-чёрное дерево.

        // выделяем блоки достаточного размера (метаданные внутри блока)
        void* raw_int = alloc.allocate(256); // запрашиваем 256 байт. аллокатор ищет подходящий свободный блок. в режиме first_fit это линейный обход физических блоков; в best/worst_fit — поиск в красно-чёрном дереве за o(log n).
        int* arr_int = static_cast<int*>(raw_int); // приведение типа (объяснялось ранее)
        for (int i = 0; i < 8; ++i) arr_int[i] = i * i; // заполняем массив квадратами индексов
        std::cout << "int array: ";
        for (int i = 0; i < 8; ++i) std::cout << arr_int[i] << ' ';
        std::cout << '\n';

        auto* fit = static_cast<allocator_with_fit_mode*>(&alloc); // static_cast — оператор приведения типов времени компиляции. в отличие от dynamic_cast, использовавшегося в предыдущих задачах, static_cast не проверяет тип во время выполнения: компилятор просто «обещает», что указатель на allocator_red_black_tree можно привести к указателю на allocator_with_fit_mode. это безопасно и быстро, т.к. allocator_red_black_tree напрямую наследуется от allocator_with_fit_mode (public inheritance), и такое приведение всегда корректно. static_cast не имеет runtime-оверхеда в отличие от dynamic_cast.
        fit->set_fit_mode(allocator_with_fit_mode::fit_mode::the_best_fit); // переключаем режим на the_best_fit. теперь аллокатор будет использовать tree_search — поиск lower_bound в красно-чёрном дереве за o(log n), что гораздо быстрее линейного поиска o(n) при большом числе блоков.

        void* raw_dbl = alloc.allocate(256); // выделяем ещё 256 байт в режиме best_fit. дерево найдёт минимальный блок, достаточно большой для 256 байт.
        double* arr_dbl = static_cast<double*>(raw_dbl); // приведение типа
        arr_dbl[0] = 2.71828; arr_dbl[1] = 3.14159; arr_dbl[2] = 1.41421; arr_dbl[3] = 1.73205; // заполняем значениями
        std::cout << "double array: ";
        for (size_t i = 0; i < 4; ++i) std::cout << arr_dbl[i] << ' ';
        std::cout << '\n';

        void* raw_str = alloc.allocate(256); // выделяем строку
        char* str = static_cast<char*>(raw_str); // приведение типа
        char const* src = "red-black tree allocator demo"; // исходная строка
        for (size_t i = 0; i < 30; ++i) str[i] = src[i]; // побайтовое копирование (30 символов)
        std::cout << "char string: " << str << '\n';

        std::cout << "blocks via iterator:\n"; // демонстрация итераторного обхода всех блоков
        for (auto it = alloc.begin(); it != alloc.end(); ++it) { // цикл for с итератором. auto — автоматический вывод типа (здесь rb_iterator). alloc.begin() возвращает итератор на первый блок, alloc.end() — пустой итератор (nullptr). ++it — префиксный инкремент, двигающий итератор к следующему физическому блоку через blk_next.
            std::cout << "  block size=" << it.size() // it.size() — вычисляет размер текущего блока через blk_size (разность адресов).
                      << " occupied=" << (it.occupied() ? "yes" : "no") << '\n'; // it.occupied() — читает флаг занятости из 1-байтовых bit fields.
        }

        alloc.deallocate(raw_str, 1); // освобождаем строку. аллокатор удалит блок из списка занятых, выполнит слияние (coalesce) с соседними свободными блоками если есть, и вставит объединённый блок в красно-чёрное дерево.
        alloc.deallocate(raw_dbl, 1); // освобождаем double
        alloc.deallocate(raw_int, 1); // освобождаем int. после всех deallocate блоки могут сливаться обратно в один большой блок 4096 байт, который снова будет единственным узлом в дереве.

        std::cout << "all blocks deallocated successfully\n";
    }
    catch (std::bad_alloc const& e) { // нехватка памяти (объяснялось ранее)
        std::cerr << "memory allocation failed: " << e.what() << '\n';
        return 1;
    }
    catch (std::exception const& e) { // прочие стандартные исключения (объяснялось ранее)
        std::cerr << "standard exception: " << e.what() << '\n';
        return 3;
    }

    return 0; // успешное завершение
}
