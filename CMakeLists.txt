cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  # Forbid in-tree builds (which clobber Makefile)
  message(FATAL_ERROR "Build in a separate directory: cmake -S . -B build")
endif()

project(nvcx VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_BUILD_TYPE RelWithDebInfo)
include(GNUInstallDirs)

add_library(nvcx_warnings INTERFACE)
target_compile_options(nvcx_warnings INTERFACE -Wall -Wextra -Wno-missing-field-initializers -Werror)

# Core: the interface every backend implements (nvcx::Core)
add_library(nvcx_core STATIC src/core/core.cpp)
target_link_libraries(nvcx_core PRIVATE nvcx_warnings)
target_include_directories(nvcx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# xsim backend (the kernel runs on a Boost.Context fiber)
find_package(Boost CONFIG REQUIRED COMPONENTS context)
add_library(nvcx_xsim STATIC src/xsim/kernel.cpp src/xsim/vivado.cpp src/xsim/simulation.cpp)
target_link_libraries(nvcx_xsim PUBLIC nvcx_core ${CMAKE_DL_LIBS} Boost::context PRIVATE nvcx_warnings)

# Verilator backend: loads the models nvcx.tcl builds with the harness in
# src/verilator/shim, which ships with the Tcl package (NVCX_VERILATOR_SHIM)
add_library(nvcx_verilator STATIC src/verilator/model.cpp)
target_include_directories(nvcx_verilator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/verilator/shim)
target_link_libraries(nvcx_verilator PUBLIC nvcx_core ${CMAKE_DL_LIBS} PRIVATE nvcx_warnings)

# open_core: every backend, chosen by what a snapshot holds
add_library(nvcx_backends STATIC src/core/open.cpp)
target_link_libraries(nvcx_backends PUBLIC nvcx_xsim nvcx_verilator PRIVATE nvcx_warnings)

# VHPI plugin for NVC, built against the vhpi_user.h NVC installs, as its
# pkg-config file says (nvc.pc: NVC 1.24, or contrib/0005 for 1.23)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NVC REQUIRED IMPORTED_TARGET nvc)
add_library(nvcx_plugin SHARED src/bridge/bridge.cpp)
target_link_libraries(nvcx_plugin PRIVATE nvcx_backends nvcx_warnings PkgConfig::NVC)
target_link_options(nvcx_plugin PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/bridge/exports.map" -Wl,-Bsymbolic)
set_target_properties(nvcx_plugin PROPERTIES OUTPUT_NAME nvcx LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/bridge/exports.map)

# WDB stub to reduce Vivado's shared-library blast radius, under the bridge
# library's name since 2025.1 and before
add_library(wdbstub SHARED src/wdbstub.cpp)
set_target_properties(wdbstub PROPERTIES OUTPUT_NAME xv_simbridge_kernel)
add_library(wdbstub_rdi SHARED src/wdbstub.cpp)
set_target_properties(wdbstub_rdi PROPERTIES OUTPUT_NAME rdi_simbridge_kernel)
# they use nothing of libc's, but a library should still name it (lintian)
target_link_options(wdbstub PRIVATE -Wl,--no-as-needed)
target_link_options(wdbstub_rdi PRIVATE -Wl,--no-as-needed)

# Before 2025.1 the kernel has no soname. This stub carries its name, and
# NEEDs a placeholder that load_closure() overwrites with the kernel's path;
# kernelpath only supplies that NEEDED entry when linking.
string(REPEAT "X" 1000 NVCX_KERNEL_PATH_ROOM)
add_library(kernelpath SHARED src/kernelstub.cpp)
set_target_properties(kernelpath PROPERTIES NO_SONAME ON)
target_link_options(kernelpath PRIVATE "-Wl,-soname,nvcx-kernel-path-${NVCX_KERNEL_PATH_ROOM}")

add_library(kernelstub SHARED src/kernelstub.cpp)
target_link_libraries(kernelstub PRIVATE kernelpath)
target_link_options(kernelstub PRIVATE -Wl,--no-as-needed)
set_target_properties(kernelstub PROPERTIES OUTPUT_NAME rdi_simulator_kernel SKIP_BUILD_RPATH ON)

# the core's VHDL entity, beside the build tree's plugin
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/nvcx.vhd
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/vhdl/nvcx.vhd ${CMAKE_BINARY_DIR}/nvcx.vhd
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/vhdl/nvcx.vhd)
add_custom_target(nvcx_vhd ALL DEPENDS ${CMAKE_BINARY_DIR}/nvcx.vhd)

# Tcl scripts for `nvc --do`, beside the build tree's executable, and the
# harness nvcx::verilate compiles with each verilated model
file(GLOB NVCX_TCL CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tcl/*.tcl)
file(GLOB NVCX_VERILATOR_SHIM CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/verilator/shim/*)
list(APPEND NVCX_TCL ${NVCX_VERILATOR_SHIM})
configure_file(tcl/pkgIndex.tcl.in ${CMAKE_BINARY_DIR}/generated/pkgIndex.tcl @ONLY)
list(APPEND NVCX_TCL ${CMAKE_BINARY_DIR}/generated/pkgIndex.tcl)
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/tcl.stamp
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/tcl
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/tcl
  COMMAND ${CMAKE_COMMAND} -E copy ${NVCX_TCL} ${CMAKE_BINARY_DIR}/tcl
  COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/tcl.stamp
  DEPENDS ${NVCX_TCL})
add_custom_target(nvcx_tcl ALL DEPENDS ${CMAKE_BINARY_DIR}/tcl.stamp)

# xsim backend tests
add_executable(nvcx-tests tests/engine_tests.cpp)
target_link_libraries(nvcx-tests PRIVATE nvcx_xsim nvcx_warnings)

# Tcl searches a TCLLIBPATH directory's subdirectories, so the package goes in nvcx<version>/ under
# NVCX_TCL_DIR. Packagers can point that at their distribution's Tcl package path.
set(NVCX_TCL_DIR ${CMAKE_INSTALL_DATADIR}/nvcx/tcl CACHE STRING
  "Directory (absolute, or relative to the prefix) holding the nvcx Tcl package")
cmake_path(ABSOLUTE_PATH NVCX_TCL_DIR BASE_DIRECTORY ${CMAKE_INSTALL_PREFIX}
  OUTPUT_VARIABLE NVCX_TCL_FULL_DIR)
configure_file(nvcx.pc.in ${CMAKE_BINARY_DIR}/generated/nvcx.pc @ONLY)

install(TARGETS nvcx_plugin LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/nvcx)
install(TARGETS wdbstub wdbstub_rdi kernelstub LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/nvcx)
install(FILES vhdl/nvcx.vhd DESTINATION ${CMAKE_INSTALL_DATADIR}/nvcx)
install(FILES ${NVCX_TCL} DESTINATION ${NVCX_TCL_DIR}/nvcx${PROJECT_VERSION})
install(FILES ${CMAKE_BINARY_DIR}/generated/nvcx.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
install(FILES man/nvcx.7 DESTINATION ${CMAKE_INSTALL_MANDIR}/man7)
