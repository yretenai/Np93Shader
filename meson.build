# SPDX-FileCopyrightText: 2025 Np93-237
#
# SPDX-License-Identifier: EUPL-1.2

project(
  'np93shader',
  'cpp',
  version : '0.1',
  meson_version : '>= 1.3.0',
  default_options : ['warning_level=3', 'cpp_std=c++20'],
)

dependencies = [
  dependency('vulkan', required: true),
  dependency('glslang', required: true, fallback : ['glslang', 'glslang_dep'], modules : ['glslang::glslang', 'glslang::glslang-default-resource-limits'])
]

shared_library(
  'np93shader',
  [
    'np93shader.cpp',
    'vendor/SPIRV-Cross/spirv_cfg.cpp',
    'vendor/SPIRV-Cross/spirv_cross.cpp',
    'vendor/SPIRV-Cross/spirv_cross_parsed_ir.cpp',
    'vendor/SPIRV-Cross/spirv_glsl.cpp',
    'vendor/SPIRV-Cross/spirv_parser.cpp'
  ],
  include_directories : [
    'vendor/SPIRV-Cross/include'
  ],
  install : true,
  dependencies : dependencies,
)
