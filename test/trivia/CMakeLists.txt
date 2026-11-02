add_executable(TriviaCoreTest
  TriviaCoreTest.cpp
  TriviaReportTest.cpp
  ${REPO_ROOT}/src/trivia/TriviaCore.cpp
  ${REPO_ROOT}/src/trivia/TriviaReport.cpp
)

target_link_libraries(TriviaCoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TriviaCoreTest)
