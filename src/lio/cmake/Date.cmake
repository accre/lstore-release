MACRO (Date RESULT)
    IF (WIN32)
        EXECUTE_PROCESS(COMMAND "date" "/T" OUTPUT_VARIABLE TODAY_E)
        EXECUTE_PROCESS(COMMAND "time" "/T" OUTPUT_VARIABLE TIME_NOW_E)
        set(temp "${TODAY} ${TIME_NOW}")
        string(REGEX REPLACE "\r" "" temp1 ${temp})
        string(REGEX REPLACE "\n" "" ${RESULT} ${temp1})
    ELSEIF(UNIX)
        EXECUTE_PROCESS(COMMAND "date" OUTPUT_VARIABLE TIME_E)
        string(REGEX REPLACE "\n" "" ${RESULT} ${TIME_E})
    ELSE (WIN32)
        MESSAGE(SEND_ERROR "date not implemented")
        SET(${RESULT} 000000)
    ENDIF (WIN32)

#    message(STATUS "Date: ${BUILD_DATE}")
ENDMACRO (Date)
