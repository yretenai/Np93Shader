# SPDX-FileCopyrightText: 2025 Np93-237
#
# SPDX-License-Identifier: EUPL-1.2

cmake_minimum_required(VERSION 3.10)
project(np93shader CXX)

find_package(Vulkan REQUIRED)
include_directories(${Vulkan_INCLUDE_DIR})

set(GLSLANG_LIBS
	glslang
	OSDependent
	OGLCompiler
	SPIRV
	SPIRV-Tools
)

add_library(np93shader SHARED
	np93shader.cpp
	vendor/SPIRV-Cross/spirv_cfg.cpp
	vendor/SPIRV-Cross/spirv_cross.cpp
	vendor/SPIRV-Cross/spirv_cross_parsed_ir.cpp
	vendor/SPIRV-Cross/spirv_glsl.cpp
	vendor/SPIRV-Cross/spirv_parser.cpp)

set_property(TARGET np93shader PROPERTY CXX_STANDARD 20)

target_include_directories(np93shader PRIVATE 
	vendor/glslang
	vendor/glslang/glslang/Public
	vendor/glslang/SPIRV
	vendor/glslang/External
	vendor/SPIRV-Cross/include
)

target_link_libraries(np93shader PRIVATE
	Vulkan::Vulkan
	glslang
	SPIRV-Tools
)
