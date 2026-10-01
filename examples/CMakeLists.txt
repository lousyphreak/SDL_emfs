# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(sdl_emfs_sync_iostream sync_iostream.cpp)
target_link_libraries(sdl_emfs_sync_iostream PRIVATE SDL_emfs::SDL_emfs)

add_executable(sdl_emfs_sync_read_at sync_read_at.cpp)
target_link_libraries(sdl_emfs_sync_read_at PRIVATE SDL_emfs::SDL_emfs)

add_executable(sdl_emfs_async_callbacks async_callbacks.cpp)
target_link_libraries(sdl_emfs_async_callbacks PRIVATE SDL_emfs::SDL_emfs)

add_executable(sdl_emfs_user_save user_save.cpp)
target_link_libraries(sdl_emfs_user_save PRIVATE SDL_emfs::SDL_emfs)

add_executable(sdl_emfs_async_await async_await_adapter.cpp)
target_compile_features(sdl_emfs_async_await PRIVATE cxx_std_20)
target_link_libraries(sdl_emfs_async_await PRIVATE SDL_emfs::SDL_emfs)

if(EMSCRIPTEN)
    set_target_properties(
        sdl_emfs_sync_iostream
        sdl_emfs_sync_read_at
        sdl_emfs_async_callbacks
        sdl_emfs_async_await
        sdl_emfs_user_save
        PROPERTIES SUFFIX ".html"
    )
    target_link_options(sdl_emfs_sync_iostream PRIVATE "SHELL:-sASYNCIFY")
    target_link_options(sdl_emfs_sync_read_at PRIVATE "SHELL:-sASYNCIFY")
    target_link_options(sdl_emfs_user_save PRIVATE "SHELL:-sASYNCIFY")
endif()
