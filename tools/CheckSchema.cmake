# Fails when a schema file in ui/src/generated is stale. Regenerate with:
#   build/<preset>/tools/sedef-params > ui/src/generated/parameters.json
#   build/<preset>/tools/sedef-params --midi > ui/src/generated/midi-parameters.json
execute_process(COMMAND ${TOOL} ${ARGS} OUTPUT_VARIABLE actual RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "sedef-params failed")
endif()
file(READ ${EXPECTED} expected)
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "${EXPECTED} is out of date; regenerate it with sedef-params ${ARGS}")
endif()
