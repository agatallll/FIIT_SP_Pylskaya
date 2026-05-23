#include <allocator_red_black_tree.h>
#include <iostream>
#include <stdexcept>

int main()
{
    try {
        allocator_red_black_tree alloc(4096, nullptr, allocator_with_fit_mode::fit_mode::first_fit);

        // выделяем блоки достаточного размера (метаданные внутри блока)
        void* raw_int = alloc.allocate(256);
        int* arr_int = static_cast<int*>(raw_int);
        for (int i = 0; i < 8; ++i) arr_int[i] = i * i;
        std::cout << "int array: ";
        for (int i = 0; i < 8; ++i) std::cout << arr_int[i] << ' ';
        std::cout << '\n';

        auto* fit = static_cast<allocator_with_fit_mode*>(&alloc);
        fit->set_fit_mode(allocator_with_fit_mode::fit_mode::the_best_fit);

        void* raw_dbl = alloc.allocate(256);
        double* arr_dbl = static_cast<double*>(raw_dbl);
        arr_dbl[0] = 2.71828; arr_dbl[1] = 3.14159; arr_dbl[2] = 1.41421; arr_dbl[3] = 1.73205;
        std::cout << "double array: ";
        for (size_t i = 0; i < 4; ++i) std::cout << arr_dbl[i] << ' ';
        std::cout << '\n';

        void* raw_str = alloc.allocate(256);
        char* str = static_cast<char*>(raw_str);
        char const* src = "red-black tree allocator demo";
        for (size_t i = 0; i < 30; ++i) str[i] = src[i];
        std::cout << "char string: " << str << '\n';

        std::cout << "blocks via iterator:\n";
        for (auto it = alloc.begin(); it != alloc.end(); ++it) {
            std::cout << "  block size=" << it.size()
                      << " occupied=" << (it.occupied() ? "yes" : "no") << '\n';
        }

        alloc.deallocate(raw_str, 1);
        alloc.deallocate(raw_dbl, 1);
        alloc.deallocate(raw_int, 1);

        std::cout << "all blocks deallocated successfully\n";
    }
    catch (std::bad_alloc const& e) {
        std::cerr << "memory allocation failed: " << e.what() << '\n';
        return 1;
    }
    catch (std::exception const& e) {
        std::cerr << "standard exception: " << e.what() << '\n';
        return 3;
    }

    return 0;
}
