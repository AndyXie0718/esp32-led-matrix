# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS components/Middlewares)
add_compile_options(-fdiagnostics-color=always)

# Try to resolve ESP-IDF path when CMake is launched outside ESP-IDF shell.
if(NOT EXISTS "$ENV{IDF_PATH}/tools/cmake/project.cmake")
	if(DEFINED ENV{ESP_IDF_PATH} AND EXISTS "$ENV{ESP_IDF_PATH}/tools/cmake/project.cmake")
		set(ENV{IDF_PATH} "$ENV{ESP_IDF_PATH}")
	elseif(EXISTS "C:/Tools/Espressif/frameworks/esp-idf-v5.5.3/tools/cmake/project.cmake")
		set(ENV{IDF_PATH} "C:/Tools/Espressif/frameworks/esp-idf-v5.5.3")
	elseif(EXISTS "D:/Tools/.espressif/v5.5.3/esp-idf/tools/cmake/project.cmake")
		set(ENV{IDF_PATH} "D:/Tools/.espressif/v5.5.3/esp-idf")
	endif()
endif()

if(NOT EXISTS "$ENV{IDF_PATH}/tools/cmake/project.cmake")
	message(FATAL_ERROR "ESP-IDF not found. Please set IDF_PATH to your esp-idf root folder.")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firststep)
