#include <allocator_global_heap.h> // заголовок реализованного аллокатора глобальной кучи
#include <iostream>                // стандартный поток вывода для печати результатов
#include <string>                  // строковый тип для формирования сообщений
#include <stdexcept>               // стандартные исключения для обработки ошибок
#include <limits>                  // заголовок для numeric_limits (проверка переполнения)

int main()
{
    try // внешний блок перехвата: приложение не должно завершаться аварийно
    {
        allocator_global_heap allocator_instance; // создаём объект аллокатора (собственный мьютекс)
        
        // ---------- демонстрация 1: массив целых чисел ----------
        int const int_count = 5; // количество элементов типа int для размещения
        if (int_count < 0) // валидация: количество элементов не может быть отрицательным
        {
            throw std::invalid_argument("negative element count for int array"); // сообщаем об ошибке параметра
        }
        
        void *raw_int = allocator_instance.allocate(sizeof(int) * static_cast<size_t>(int_count)); // выделяем память под массив int
        int *int_array = static_cast<int *>(raw_int); // приводим void* к типизированному указателю
        
        for (int i = 0; i < int_count; ++i) // заполняем массив в отдельном контексте работы с данными
        {
            int_array[i] = i * 10; // записываем значения в выделенный блок
        }
        
        std::cout << "int array: "; // начинаем вывод результатов
        for (int i = 0; i < int_count; ++i) // итерируемся по размещённым элементам
        {
            std::cout << int_array[i] << ' '; // отправляем данные в поток вывода
        }
        std::cout << '\n'; // завершаем строку вывода
        
        // ---------- демонстрация 2: массив вещественных чисел ----------
        size_t double_count = 3; // количество элементов типа double
        if (double_count > std::numeric_limits<size_t>::max() / sizeof(double)) // проверка на переполнение при умножении
        {
            throw std::overflow_error("size overflow for double array"); // предотвращаем переполнение
        }
        
        void *raw_double = allocator_instance.allocate(sizeof(double) * double_count); // выделяем память под double массив
        double *double_array = static_cast<double *>(raw_double); // приводим указатель к типу double*
        
        double_array[0] = 3.14; // записываем первое вещественное значение
        double_array[1] = 2.71; // записываем второе вещественное значение
        double_array[2] = 1.41; // записываем третье вещественное значение
        
        std::cout << "double array: " << double_array[0] << ", " // выводим размещённые double-значения
                  << double_array[1] << ", " << double_array[2] << '\n';
        
        // ---------- демонстрация 3: строка символов ----------
        char const *source = "hello, global heap!"; // исходная строка для копирования
        size_t str_len = std::char_traits<char>::length(source) + 1; // длина с учётом нуль-терминатора
        
        void *raw_str = allocator_instance.allocate(str_len); // выделяем память под строку
        char *str = static_cast<char *>(raw_str); // приводим указатель к char*
        
        for (size_t i = 0; i < str_len; ++i) // копируем строку побайтно в выделенный блок
        {
            str[i] = source[i]; // копируем очередной символ
        }
        
        std::cout << "char string: " << str << '\n'; // выводим скопированную строку
        
        // ---------- освобождение ресурсов ----------
        allocator_instance.deallocate(str, str_len); // освобождаем память строки (байты передаются для совместимости интерфейса)
        allocator_instance.deallocate(double_array, sizeof(double) * double_count); // освобождаем блок double
        allocator_instance.deallocate(int_array, sizeof(int) * static_cast<size_t>(int_count)); // освобождаем блок int
        
        std::cout << "all blocks deallocated successfully\n"; // подтверждаем корректное освобождение памяти
    }
    catch (std::bad_alloc const &e) // перехватываем ошибку нехватки памяти
    {
        std::cerr << "memory allocation failed: " << e.what() << '\n'; // выводим диагностику в поток ошибок
        return 1; // возвращаем ненулевой код завершения
    }
    catch (std::invalid_argument const &e) // перехватываем ошибки валидации аргументов
    {
        std::cerr << "invalid argument: " << e.what() << '\n'; // сообщаем о некорректных входных данных
        return 2;
    }
    catch (std::overflow_error const &e) // перехватываем арифметические переполнения
    {
        std::cerr << "overflow error: " << e.what() << '\n'; // сообщаем о переполнении размера
        return 3;
    }
    catch (std::exception const &e) // перехватываем прочие стандартные исключения
    {
        std::cerr << "standard exception: " << e.what() << '\n'; // печатаем описание ошибки
        return 4;
    }
    catch (...) // перехватываем все остальные неизвестные исключения
    {
        std::cerr << "unknown exception occurred\n"; // сообщаем о непредвиденной ошибке
        return 5;
    }
    
    return 0; // успешное завершение приложения
}
