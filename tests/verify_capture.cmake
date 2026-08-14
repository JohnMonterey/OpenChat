if(NOT DEFINED OPENCHAT_EXECUTABLE OR NOT DEFINED CAPTURE_OUTPUT)
    message(FATAL_ERROR "OPENCHAT_EXECUTABLE and CAPTURE_OUTPUT are required")
endif()

file(REMOVE "${CAPTURE_OUTPUT}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            QT_QPA_PLATFORM=offscreen
            QT_QUICK_BACKEND=software
            "${OPENCHAT_EXECUTABLE}"
            --capture "${CAPTURE_OUTPUT}"
            --capture-delay 100
    RESULT_VARIABLE capture_result
    TIMEOUT 3
)

if(NOT capture_result EQUAL 0)
    message(FATAL_ERROR "OpenChat capture failed with result ${capture_result}")
endif()

if(NOT EXISTS "${CAPTURE_OUTPUT}")
    message(FATAL_ERROR "OpenChat did not create ${CAPTURE_OUTPUT}")
endif()

file(SIZE "${CAPTURE_OUTPUT}" capture_size)
if(capture_size LESS 10000)
    message(FATAL_ERROR "OpenChat capture is unexpectedly small: ${capture_size} bytes")
endif()
