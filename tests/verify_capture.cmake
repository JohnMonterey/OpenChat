if(NOT DEFINED OPENCHAT_EXECUTABLE OR NOT DEFINED CAPTURE_OUTPUT
   OR NOT DEFINED CAPTURE_WIDTH OR NOT DEFINED CAPTURE_HEIGHT)
    message(FATAL_ERROR
        "OPENCHAT_EXECUTABLE, CAPTURE_OUTPUT, CAPTURE_WIDTH, and CAPTURE_HEIGHT are required")
endif()

file(REMOVE "${CAPTURE_OUTPUT}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            QT_QPA_PLATFORM=offscreen
            QT_QUICK_BACKEND=software
            "${OPENCHAT_EXECUTABLE}"
            --capture "${CAPTURE_OUTPUT}"
            --capture-delay 100
            --width "${CAPTURE_WIDTH}"
            --height "${CAPTURE_HEIGHT}"
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

file(READ "${CAPTURE_OUTPUT}" png_dimensions OFFSET 16 LIMIT 8 HEX)
string(SUBSTRING "${png_dimensions}" 0 8 png_width_hex)
string(SUBSTRING "${png_dimensions}" 8 8 png_height_hex)
math(EXPR png_width "0x${png_width_hex}")
math(EXPR png_height "0x${png_height_hex}")
if(NOT png_width EQUAL CAPTURE_WIDTH OR NOT png_height EQUAL CAPTURE_HEIGHT)
    message(FATAL_ERROR
        "OpenChat capture is ${png_width}x${png_height}; expected ${CAPTURE_WIDTH}x${CAPTURE_HEIGHT}")
endif()
