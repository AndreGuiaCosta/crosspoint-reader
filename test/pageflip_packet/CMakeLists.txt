add_executable(PageFlipPacketTest
  PageFlipPacketTest.cpp
  ${REPO_ROOT}/lib/PageFlip/PageFlipMac.cpp
  ${REPO_ROOT}/lib/PageFlip/PageFlipPacket.cpp
)

target_link_libraries(PageFlipPacketTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PageFlipPacketTest)
