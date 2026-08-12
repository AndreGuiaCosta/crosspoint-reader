if(UNIX)
  add_executable(PageFlipUdpTransportTest
    PageFlipUdpTransportTest.cpp
    ${REPO_ROOT}/lib/PageFlip/PageFlipPacket.cpp
    ${REPO_ROOT}/lib/PageFlip/PageFlipUdpTransport.cpp
  )

  target_link_libraries(PageFlipUdpTransportTest PRIVATE
    crosspoint_test_common
    GTest::gtest_main
  )

  gtest_discover_tests(PageFlipUdpTransportTest)
endif()
