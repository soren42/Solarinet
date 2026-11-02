if(NOT EXISTS "${CLIENT_BIN}")
  message(FATAL_ERROR "Client binary does not exist: ${CLIENT_BIN}")
endif()

execute_process(
  COMMAND "${OBJDUMP}" -p "${CLIENT_BIN}"
  RESULT_VARIABLE _objdump_result
  OUTPUT_VARIABLE _objdump_output
  ERROR_VARIABLE _objdump_error)
if(NOT _objdump_result EQUAL 0)
  message(FATAL_ERROR "objdump failed: ${_objdump_error}")
endif()

string(REGEX MATCH "NEEDED[ \t]+[^\n]*(libnng|libmbed[^ \t\n]*|libcjson)[^ \t\n]*"
       _forbidden_needed "${_objdump_output}")
if(_forbidden_needed)
  message(FATAL_ERROR "Client has a dynamic vendored-library dependency: ${_forbidden_needed}")
endif()
message(STATUS "Client has no dynamic nng, mbedTLS, or cJSON dependency")
