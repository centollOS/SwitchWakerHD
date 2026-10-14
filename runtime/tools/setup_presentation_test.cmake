file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND "${JAVAC}" -d "${WORK}"
  "${SOURCE}/android/app/src/main/java/org/wwhdrecomp/wwhd/SetupPresentation.java"
  "${SOURCE}/tools/android/SetupPresentationTest.java" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Setup presentation test compilation failed")
endif()
execute_process(COMMAND "${JAVA}" -cp "${WORK}" SetupPresentationTest RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Setup presentation test failed")
endif()
