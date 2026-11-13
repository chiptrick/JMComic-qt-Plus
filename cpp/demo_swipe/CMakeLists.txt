cmake_minimum_required(VERSION 3.21)
project(jm_swipe_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_executable(jm_swipe_demo
    main.cpp
    swipe_pager.cpp swipe_pager.h
    demo_widgets.cpp demo_widgets.h
    demo_window.cpp demo_window.h
)

target_link_libraries(jm_swipe_demo PRIVATE Qt6::Widgets)
