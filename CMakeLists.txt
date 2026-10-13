cmake_minimum_required(VERSION 3.22.1)
project(matrixrain LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Oboe llega como paquete Prefab desde com.google.oboe:oboe (Maven de Google)
find_package(oboe REQUIRED CONFIG)

# Pegamento oficial de NativeActivity incluido en el NDK
set(APP_GLUE_DIR ${ANDROID_NDK}/sources/android/native_app_glue)
add_library(app_glue STATIC ${APP_GLUE_DIR}/android_native_app_glue.c)
target_include_directories(app_glue PUBLIC ${APP_GLUE_DIR})

add_library(matrixrain SHARED
        main.cpp
        vulkan_engine.cpp
        audio_processor.cpp)

target_compile_definitions(matrixrain PRIVATE VK_USE_PLATFORM_ANDROID_KHR=1)
target_compile_options(matrixrain PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Necesario para que ANativeActivity_onCreate (en app_glue) no sea descartado
target_link_options(matrixrain PRIVATE
        -u ANativeActivity_onCreate
        -Wl,-z,max-page-size=16384)

target_link_libraries(matrixrain PRIVATE
        app_glue
        android
        log
        vulkan
        jnigraphics
        oboe::oboe)
