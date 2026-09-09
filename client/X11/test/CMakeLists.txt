find_program(XVFB_RUN_EXECUTABLE xvfb-run)
if(XVFB_RUN_EXECUTABLE AND WITH_XSHM AND X11_XShm_FOUND)
  add_executable(TestX11Shm TestX11Shm.c)
  target_link_libraries(TestX11Shm PRIVATE xfreerdp-client)
  add_test(NAME TestX11Shm
           COMMAND ${XVFB_RUN_EXECUTABLE} -a -s "-screen 0 2560x1440x24 -nolisten tcp -extension MIT-SHM" sh
                   ${CMAKE_CURRENT_SOURCE_DIR}/test-shm.sh ${XVFB_RUN_EXECUTABLE} $<TARGET_FILE:TestX11Shm>
  )
endif()
