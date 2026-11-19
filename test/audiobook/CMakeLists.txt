add_executable(BtModuleProtocolTest
  BtModuleProtocolTest.cpp
  ${REPO_ROOT}/lib/MetalioBtModule/BtModuleProtocol.cpp
)

target_include_directories(BtModuleProtocolTest PRIVATE
  ${REPO_ROOT}/lib/MetalioBtModule
)

target_link_libraries(BtModuleProtocolTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BtModuleProtocolTest)

add_executable(PcmPipelineTest
  PcmPipelineTest.cpp
  ${REPO_ROOT}/lib/Audiobook/PcmPipeline.cpp
  ${REPO_ROOT}/lib/sonic/sonic.c
)

target_include_directories(PcmPipelineTest PRIVATE
  ${REPO_ROOT}/lib/Audiobook
  ${REPO_ROOT}/lib/sonic
)

target_link_libraries(PcmPipelineTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  m
)

gtest_discover_tests(PcmPipelineTest)

# Vendored upstream source: keep it unmodified and quiet.
set_source_files_properties(${REPO_ROOT}/lib/sonic/sonic.c PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter")

set(AUDIOBOOK_READER_SOURCES
  ${REPO_ROOT}/lib/Audiobook/ByteSource.cpp
  ${REPO_ROOT}/lib/Audiobook/TrackReader.cpp
  ${REPO_ROOT}/lib/Audiobook/Mp3Reader.cpp
  ${REPO_ROOT}/lib/Audiobook/Mp4Reader.cpp
)

foreach(test_name Mp3ReaderTest Mp4ReaderTest)
  add_executable(${test_name} ${test_name}.cpp ${AUDIOBOOK_READER_SOURCES})
  target_include_directories(${test_name} PRIVATE
    ${REPO_ROOT}/lib/Audiobook
    ${REPO_ROOT}/lib/Memory
  )
  target_compile_definitions(${test_name} PRIVATE
    AUDIOBOOK_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  )
  target_link_libraries(${test_name} PRIVATE
    crosspoint_test_common
    GTest::gtest_main
  )
  gtest_discover_tests(${test_name})
endforeach()

# Vendored Helix decoders (RPSL), built with their stdlib path and host stubs
# for <Arduino.h>/<pgmspace.h>.
file(GLOB HELIX_SOURCES ${REPO_ROOT}/lib/helix/src/libhelix-mp3/*.c ${REPO_ROOT}/lib/helix/src/libhelix-aac/*.c)
add_library(helix_host STATIC ${HELIX_SOURCES})
target_include_directories(helix_host PUBLIC
  ${REPO_ROOT}/lib/helix/src
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)
target_compile_definitions(helix_host PRIVATE USE_DEFAULT_STDLIB)
# Helix only knows embedded targets; ARDUINO selects its portable C path.
target_compile_definitions(helix_host PUBLIC ARDUINO=1)
target_compile_options(helix_host PRIVATE -w)

add_executable(DecodeChainTest
  DecodeChainTest.cpp
  ${AUDIOBOOK_READER_SOURCES}
  ${REPO_ROOT}/lib/Audiobook/AudioDecoder.cpp
  ${REPO_ROOT}/lib/Audiobook/PcmPipeline.cpp
  ${REPO_ROOT}/lib/sonic/sonic.c
)
target_include_directories(DecodeChainTest PRIVATE
  ${REPO_ROOT}/lib/Audiobook
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/sonic
)
target_compile_definitions(DecodeChainTest PRIVATE
  AUDIOBOOK_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_link_libraries(DecodeChainTest PRIVATE
  crosspoint_test_common
  helix_host
  GTest::gtest_main
  m
)
gtest_discover_tests(DecodeChainTest)

add_executable(BookModelTest BookModelTest.cpp ${REPO_ROOT}/lib/Audiobook/BookModel.cpp)
target_include_directories(BookModelTest PRIVATE ${REPO_ROOT}/lib/Audiobook)
target_link_libraries(BookModelTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BookModelTest)
