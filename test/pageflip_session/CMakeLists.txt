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

  gtest_discover_tests(PageFlipSessionTest)
endif()
