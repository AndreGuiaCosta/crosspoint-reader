if(UNIX)
  add_executable(PageFlipSessionTest
    PageFlipSessionTest.cpp
    ${REPO_ROOT}/lib/PageFlip/PageFlipPacket.cpp
    ${REPO_ROOT}/lib/PageFlip/PageFlipSession.cpp
    ${REPO_ROOT}/lib/PageFlip/PageFlipUdpTransport.cpp
  )

  target_link_libraries(PageFlipSessionTest PRIVATE
    crosspoint_test_common
    GTest::gtest_main
  )

  # Every case here binds the same loopback ports, and the transport deliberately refuses a
  # duplicate bind, so these must not run concurrently with each other or with the transport suite.
  gtest_discover_tests(PageFlipSessionTest PROPERTIES RESOURCE_LOCK "pageflip-ports")
endif()
