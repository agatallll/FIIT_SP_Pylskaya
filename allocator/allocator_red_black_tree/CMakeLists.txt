add_subdirectory(tests)

add_library(
        sys_prog_allctr_allctr_rb_tr
        src/allocator_red_black_tree.cpp)

add_executable(
        sys_prog_allctr_allctr_rb_tr_demo
        src/demo.cpp)

target_link_libraries(
        sys_prog_allctr_allctr_rb_tr_demo
        PRIVATE
        sys_prog_allctr_allctr_rb_tr)

target_include_directories(
        sys_prog_allctr_allctr_rb_tr
        PUBLIC
        ./include)

target_link_libraries(
        sys_prog_allctr_allctr_rb_tr
        PUBLIC
        sys_prog_cmmn)
target_link_libraries(
        sys_prog_allctr_allctr_rb_tr
        PUBLIC
        sys_prog_allctr_allctr)


