function(add_study_test target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE crosspoint_test_common)
  add_test(NAME ${target} COMMAND ${target})
endfunction()

set(STUDY_CORE ${REPO_ROOT}/src/apps_local/study)

add_study_test(StudyFsrsTest
  test_fsrs.cpp
  ${STUDY_CORE}/StudyFsrs.cpp
)

add_study_test(StudySchedulerTest
  test_scheduler.cpp
  ${STUDY_CORE}/StudyScheduler.cpp
  ${STUDY_CORE}/StudyDeck.cpp
  ${STUDY_CORE}/StudyFsrs.cpp
)

add_study_test(StudyStatsTest
  test_stats.cpp
  ${STUDY_CORE}/StudyStats.cpp
  ${STUDY_CORE}/StudyDeck.cpp
  ${STUDY_CORE}/StudyFsrs.cpp
)

add_study_test(StudyTextTest test_text.cpp)