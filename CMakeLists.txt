cmake_minimum_required(VERSION 3.20)
project(sshpad C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4    REQUIRED gtk4)
pkg_check_modules(WEBKIT  REQUIRED webkitgtk-6.0)
pkg_check_modules(MHD     REQUIRED libmicrohttpd)
pkg_check_modules(JSONC   REQUIRED json-c)
pkg_check_modules(OPENSSL REQUIRED openssl)
pkg_check_modules(X11 x11)

add_executable(sshpad
    src/main.c
    src/http_server.c
    src/config_parser.c
    src/process_manager.c
    src/sse.c
    src/terminal_launch.c
    src/askpass.c
    src/port_finder.c
    src/util.c
    src/window_state.c
    src/local_proxy.c
)

target_include_directories(sshpad PRIVATE
    ${GTK4_INCLUDE_DIRS}
    ${WEBKIT_INCLUDE_DIRS}
    ${MHD_INCLUDE_DIRS}
    ${JSONC_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIRS}
    src/
)

target_compile_options(sshpad PRIVATE
    ${GTK4_CFLAGS_OTHER}
    ${WEBKIT_CFLAGS_OTHER}
    ${MHD_CFLAGS_OTHER}
    ${JSONC_CFLAGS_OTHER}
    ${OPENSSL_CFLAGS_OTHER}
)

target_link_libraries(sshpad
    ${GTK4_LIBRARIES}
    ${WEBKIT_LIBRARIES}
    ${MHD_LIBRARIES}
    ${JSONC_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    ${X11_LIBRARIES}
    pthread
)

install(TARGETS sshpad DESTINATION bin)
install(DIRECTORY ui/ DESTINATION share/sshpad/ui)

# Copia l'eseguibile nella directory del progetto dopo il build
add_custom_command(TARGET sshpad POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:sshpad> ${CMAKE_SOURCE_DIR}/sshpad
    COMMENT "Copiato sshpad in ${CMAKE_SOURCE_DIR}/"
)
