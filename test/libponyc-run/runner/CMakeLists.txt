project(libponyc.tests.runner)

set(RUNNER_EXECUTABLE "runner${CMAKE_EXECUTABLE_SUFFIX}")

add_custom_target(tests.runner ALL DEPENDS ${RUNNER_EXECUTABLE})

add_custom_command(OUTPUT ${RUNNER_EXECUTABLE}
  COMMAND $<TARGET_FILE:ponyc> ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS
    main.pony
    _build_process_notify.pony
    _cli_options.pony
    _coordinator.pony
    _find_executable.pony
    _test_definitions.pony
    _test_process_notify.pony
    _tester_timer_notify.pony
    _tester.pony
)
