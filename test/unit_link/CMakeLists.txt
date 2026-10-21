add_executable(UnitLinkTest
  UnitLinkProtocolTest.cpp
  UnitLinkSessionTest.cpp
  ${REPO_ROOT}/lib/UnitLink/UnitLinkSession.cpp
)

target_include_directories(UnitLinkTest PRIVATE ${REPO_ROOT}/lib/UnitLink)

target_link_libraries(UnitLinkTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(UnitLinkTest)
