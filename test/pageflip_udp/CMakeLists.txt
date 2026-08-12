if(UNIX)
  add_executable(PageFlipUdpTransportTest
    PageFlipUdpTransportTest.cpp
    ${REPO_ROOT}/lib/PageFlip/PageFlipPacket.cpp
    ${REPO_ROOT}/lib/PageFlip/PageFlipUdpTransport.cpp
    ${REPO_ROOT}/lib/PageFlip/PageFlipTransportFactory.cpp
  )

  target_include_directories(PageFlipUdpTransportTest PRIVATE ${REPO_ROOT}/lib/Memory)

  target_link_libraries(PageFlipUdpTransportTest PRIVATE
    crosspoint_test_common
    GTest::gtest_main
  )

  # Binds fixed loopback ports and asserts that a duplicate bind fails, so it cannot share the
  # ports with a concurrently running case. Without this, ctest -j turns into phantom failures.
  gtest_discover_tests(PageFlipUdpTransportTest PROPERTIES RESOURCE_LOCK "pageflip-ports")
endif()
