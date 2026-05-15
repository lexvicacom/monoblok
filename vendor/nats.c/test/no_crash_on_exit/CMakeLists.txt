if(NOT BUILD_TESTING)
  return()
endif()
message(FATAL_ERROR "vendored nats.c no_crash_on_exit tests are pruned")
