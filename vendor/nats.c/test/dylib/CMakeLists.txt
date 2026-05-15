if(NOT BUILD_TESTING)
  return()
endif()
message(FATAL_ERROR "vendored nats.c dylib tests are pruned")
