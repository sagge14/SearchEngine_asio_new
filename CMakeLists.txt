cmake_minimum_required(VERSION 3.21)
project(SearchEngine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Установка путей к Boost

set(BOOST_ROOT "C:/Boost/windows32")
set(Boost_NO_SYSTEM_PATHS ON)
set(Boost_COMPILER "-vc142")

# Указание пути к Boost в `find_package`
set(CMAKE_PREFIX_PATH ${BOOST_ROOT})

set(MINIZIP_DIR ${PROJECT_SOURCE_DIR}/minizip)
include_directories(${MINIZIP_DIR}/include)
link_directories(${MINIZIP_DIR}/lib)

# Поиск Boost библиотек
find_package(Boost 1.85 REQUIRED COMPONENTS system filesystem serialization headers)

if(Boost_FOUND)
    message(STATUS "Boost_INCLUDE_DIRS: ${Boost_INCLUDE_DIRS}")
    message(STATUS "Boost_LIBRARY_DIRS: ${Boost_LIBRARY_DIRS}")
    include_directories(${Boost_INCLUDE_DIRS})
    link_directories(${Boost_LIBRARY_DIRS})
else()
    message(FATAL_ERROR "Could not find Boost libraries. Please check your Boost installation.")
endif()

# Установка путей к OpenSSL
set(OPENSSL_ROOT_DIR "C:/Program Files/OpenSSL-Win64")
set(OPENSSL_INCLUDE_DIR "C:/Program Files/OpenSSL-Win64/include")
set(OPENSSL_CRYPTO_LIBRARY "C:/Program Files/OpenSSL-Win64/lib/VC/x64/MT/libcrypto.lib")
set(OPENSSL_SSL_LIBRARY "C:/Program Files/OpenSSL-Win64/lib/VC/x64/MT/libssl.lib")

include_directories(${OPENSSL_INCLUDE_DIR})

include_directories(lib)
include_directories(src)

# Использование GLOB_RECURSE для включения всех .cpp файлов во вложенных директориях
file(GLOB_RECURSE all_SRCS
        "${PROJECT_SOURCE_DIR}/src/*.cpp"
        "${PROJECT_SOURCE_DIR}/src/*.c"
        )

include_directories(${PROJECT_SOURCE_DIR}/include)

# Подключение библиотек GoogleTest и nlohmann_json
add_subdirectory(lib/googletest)
add_subdirectory(lib/nlohmann_json)

include(GoogleTest)
enable_testing()

# Создание исполняемого файла для основного проекта
add_executable(SearchEngine ${all_SRCS} main.cpp)


# Линковка с библиотеками Boost, GoogleTest и nlohmann_json
target_link_libraries(SearchEngine PUBLIC
        nlohmann_json::nlohmann_json ${Boost_LIBRARIES}
        ${OPENSSL_CRYPTO_LIBRARY}
        ${OPENSSL_SSL_LIBRARY}
     #   F:/SearchEngine_asio_new/myLib.lib
        )

# Настройка тестового проекта
file(GLOB_RECURSE test_SRCS
        "${PROJECT_SOURCE_DIR}/test/*.cpp"
        )

# Shared src/ files needed by tests (OEMCase, Encoding used by test/index_atomic_lock_free)
set(SHARED_TEST_SRCS
        "${PROJECT_SOURCE_DIR}/src/MyUtils/OEMCase.cpp"
        "${PROJECT_SOURCE_DIR}/src/MyUtils/Encoding.cpp"
)

add_executable(SearchEngine_test ${test_SRCS} ${SHARED_TEST_SRCS})

add_compile_definitions(BOOST_ASIO_HAS_CO_AWAIT)
#add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)

# Линковка с библиотеками для тестов
add_definitions(-DUTF8PROC_STATIC=1)

# Пути до библиотеки и заголовков
target_include_directories(SearchEngine PRIVATE ${CMAKE_SOURCE_DIR}/utf8proc)
target_link_directories(SearchEngine PRIVATE ${CMAKE_SOURCE_DIR}/utf8proc)
target_compile_definitions(SearchEngine PRIVATE UTF8PROC_STATIC=1)
target_link_libraries(SearchEngine PRIVATE utf8proc_static)

target_link_libraries(SearchEngine_test PRIVATE gtest gtest_main nlohmann_json::nlohmann_json ${Boost_LIBRARIES} ws2_32 wsock32)

option(TEST_MODE "option for debug" OFF)

if (MSVC)
    add_compile_options(/arch:AVX2 /utf-8)
endif()


if (TEST_MODE)
    add_definitions(-DTEST_MODE)
endif(TEST_MODE)

set(OUTPUT_DIR "${CMAKE_BINARY_DIR}/out")
set(OUTPUT_NAME "SearchEngine") # или имя без расширения

add_custom_command(
        TARGET SearchEngine
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:SearchEngine>/out
        COMMAND powershell -ExecutionPolicy Bypass
        -File ${CMAKE_SOURCE_DIR}/copy_with_version.ps1
        $<TARGET_FILE:SearchEngine>
        $<TARGET_FILE_DIR:SearchEngine>/out
        SearchEngine
)


if(MSVC)
    add_definitions(-D_X86_)
endif()


# Добавление тестов
gtest_discover_tests(SearchEngine_test)

