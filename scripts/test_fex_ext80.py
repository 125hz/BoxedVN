#!/usr/bin/env python3
"""Compare separate-TU reference arithmetic with the production unity build."""
from pathlib import Path
import subprocess, tempfile, platform
root=Path(__file__).resolve().parent.parent
source=root/'third_party/fex64/fex/External/SoftFloat-3e'
with tempfile.TemporaryDirectory(prefix='boxedvn-ext80-') as temporary:
    path=Path(temporary)
    namespace=path/'namespace.h'
    subprocess.run(['python3',str(root/'scripts/fex-softfloat-namespace.py'),str(source.parent.parent),str(namespace)],check=True)
    baseline=path/'baseline.h'
    baseline.write_text(namespace.read_text().replace('boxedvn_fex_', 'boxedvn_fex_reference_'))
    (path/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(Ext80Check C CXX)
set(ARCHITECTURE_arm64 {'ON' if platform.machine() in ('arm64','aarch64') else 'OFF'})
set(HAS_CLANG_PRESERVE_ALL ON)
add_subdirectory("{source.as_posix()}" reference)
get_target_property(sources softfloat_3e SOURCES)
list(TRANSFORM sources PREPEND "{source.as_posix()}/")
get_target_property(includes softfloat_3e INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(defines softfloat_3e INTERFACE_COMPILE_DEFINITIONS)
add_library(baseline STATIC ${{sources}})
target_include_directories(baseline PUBLIC ${{includes}})
target_compile_definitions(baseline PUBLIC ${{defines}})
target_compile_options(baseline PRIVATE -include "{baseline.as_posix()}")
target_compile_options(softfloat_3e PRIVATE -include "{namespace.as_posix()}")
add_executable(check "{root.as_posix()}/scripts/tests/fex_ext80_differential.cpp")
target_compile_options(check PRIVATE -include "{namespace.as_posix()}")
target_compile_features(check PRIVATE cxx_std_17)
target_link_libraries(check PRIVATE softfloat_3e baseline)
''')
    subprocess.run(['cmake','-S',str(path),'-B',str(path/'build'),'-DCMAKE_BUILD_TYPE=Release',
                    '-DCMAKE_C_COMPILER=clang','-DCMAKE_CXX_COMPILER=clang++',
                    '-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES='+str(root/'scripts/fex-softfloat-overlay.cmake')],check=True,stdout=subprocess.DEVNULL)
    subprocess.run(['cmake','--build',str(path/'build'),'-j','4'],check=True,stdout=subprocess.DEVNULL)
    subprocess.run([str(path/'build/check')],check=True)
