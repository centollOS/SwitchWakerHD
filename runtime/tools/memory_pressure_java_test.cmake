file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND "${JAVAC}" -d "${WORK}"
  "${SOURCE}/android/app/src/main/java/org/wwhdrecomp/wwhd/MemoryPressurePolicy.java"
  "${SOURCE}/runtime/tools/MemoryPressureTest.java" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Memory pressure Java test compilation failed")
endif()
execute_process(COMMAND "${JAVA}" -cp "${WORK}" org.wwhdrecomp.wwhd.MemoryPressureTest RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Memory pressure test failed")
endif()
