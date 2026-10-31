# Fails when ui/src/generated/parameters.json is stale. Regenerate with:
#   build/<preset>/tools/sedef-params > ui/src/generated/parameters.json
execute_process(COMMAND ${TOOL} OUTPUT_VARIABLE actual RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "sedef-params failed")
endif()
file(READ ${EXPECTED} expected)
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "ui/src/generated/parameters.json is out of date; regenerate it with sedef-params")
endif()
