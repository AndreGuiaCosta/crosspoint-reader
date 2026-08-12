if(UNIX)
  add_executable(PageFlipCompatTest
    PageFlipCompatTest.cpp
    ${REPO_ROOT}/lib/PageFlip/PageFlipCompat.cpp
  )

  # ReaderRenderSpec is included as <Epub/ReaderRenderSpec.h>, matching how the firmware sees it
  # (PlatformIO's LDF puts lib/Epub on the include path).
  target_include_directories(PageFlipCompatTest PRIVATE ${REPO_ROOT}/lib/Epub)

  target_link_libraries(PageFlipCompatTest PRIVATE
    crosspoint_test_common
    GTest::gtest_main
  )

  # Pure function, no sockets: safe to run alongside anything.
  gtest_discover_tests(PageFlipCompatTest)
endif()
