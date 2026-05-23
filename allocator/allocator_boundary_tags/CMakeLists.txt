add_subdirectory(tests)

add_library(
        sys_prog_allctr_allctr_bndr_tgs
        src/allocator_boundary_tags.cpp)

target_include_directories(
        sys_prog_allctr_allctr_bndr_tgs
        PUBLIC
        ./include)

target_link_libraries(
        sys_prog_allctr_allctr_bndr_tgs
        PUBLIC
        sys_prog_cmmn)
target_link_libraries(
        sys_prog_allctr_allctr_bndr_tgs
        PUBLIC
        sys_prog_allctr_allctr)

add_executable(
        sys_prog_allctr_allctr_bndr_tgs_demo
        src/demo.cpp)

target_link_libraries(
        sys_prog_allctr_allctr_bndr_tgs_demo
        PRIVATE
        sys_prog_allctr_allctr_bndr_tgs)
