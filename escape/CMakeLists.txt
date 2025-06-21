cmake_minimum_required (VERSION 3.5.0)
project (
  "My Game"
  VERSION 2.0.0
  DESCRIPTION "My game"
  LANGUAGES CXX
)

set(TARGET game)
set(SOURCE game.cpp)
set(HEADERS )

if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  	add_compile_options(/D IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
	add_compile_options(/D_CRT_SECURE_NO_WARNINGS) #for assimp
	add_compile_options(/EHsc /DEBUG /Zi /Istb) #for assimp
	set(BUILD_TYPE /${CMAKE_BUILD_TYPE})
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
	add_compile_options(-D IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
	add_compile_options(-Wno-nullability-completeness)
	add_compile_options(-D_CRT_SECURE_NO_WARNINGS) #for assimp
	add_compile_options(-fexperimental-library)
	set(BUILD_TYPE )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
	add_compile_options(-D IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
	add_compile_options(-Wno-nullability-completeness)
	add_compile_options(-D_CRT_SECURE_NO_WARNINGS) #for assimp
	set(BUILD_TYPE )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	add_compile_options(-D IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
	add_compile_options(-Wno-nullability-completeness)
	add_compile_options(-D_CRT_SECURE_NO_WARNINGS) #for assimp
	set(BUILD_TYPE )
endif()

find_package(Vulkan REQUIRED)
set(VULKAN_SDK $ENV{VULKAN_SDK})

if (WIN32)
	set(VULKAN_INCLUDE Include)
	set(VULKAN_LIB Lib)
elseif (APPLE)
	set(VULKAN_INCLUDE include)
    set(VULKAN_LIB lib)
elseif (UNIX AND NOT APPLE)
	set(VULKAN_INCLUDE include)
	set(VULKAN_LIB lib)
endif()

include_directories(${VULKAN_SDK}/${VULKAN_INCLUDE})
include_directories(${VULKAN_SDK}/${VULKAN_INCLUDE}/volk)
include_directories(${VULKAN_SDK}/${VULKAN_INCLUDE}/vma)
link_directories(${VULKAN_SDK}/lib)

set(VVE ${PROJECT_SOURCE_DIR}/../ViennaVulkanEngine)
set(DEPS ${VVE}/build/_deps)
set(FFMPEG ../ffmpeg)


include_directories(${VVE}/include)
include_directories(${DEPS}/stb_image-src)
include_directories(${DEPS}/l2dfiledialog-src)
include_directories(${DEPS}/viennaentitycomponentsystem-src/include)
include_directories(${DEPS}/viennastrongtype-src/include)
include_directories(${DEPS}/viennatypelistlibrary-src/include)
include_directories(${DEPS}/sdl2-src/include)
include_directories(${DEPS}/assimp-src/include)
include_directories(${DEPS}/assimp-build/include)
include_directories(${DEPS}/imgui-src)
include_directories(${DEPS}/imgui-src/backends)
include_directories(${DEPS}/glm-src)
include_directories(${DEPS}/vkbootstrap-src/src)
include_directories(${FFMPEG}/include)

link_directories(${VVE}/build/src${BUILDTYPE})
link_directories(${DEPS}/assimp-build/lib${BUILDTYPE})
link_directories(${DEPS}/assimp-build/contrib/zlib${BUILDTYPE})
link_directories(${DEPS}/vkbootstrap-build${BUILDTYPE})
link_directories(${DEPS}/SDL2-build${BUILDTYPE})
link_directories(${FFMPEG}/lib)


add_executable(${TARGET} ${SOURCE} ${HEADERS})

target_compile_features(${TARGET} PUBLIC cxx_std_20)

target_link_libraries(${TARGET} viennavulkanengined SDL2d assimp-vc143-mtd zlibstaticd vk-bootstrapd volk avcodec avutil avformat swscale)

