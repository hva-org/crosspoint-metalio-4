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

# Vendored decoders, built as on the device: faad2 takes its settings from
# lib/faad2/config.h, and minimp3.c sets the minimp3 build options.
file(GLOB FAAD2_SOURCES ${REPO_ROOT}/lib/faad2/libfaad/*.c)
add_library(decoders_host STATIC ${FAAD2_SOURCES} ${REPO_ROOT}/lib/minimp3/src/minimp3.c)
target_include_directories(decoders_host PUBLIC
  ${REPO_ROOT}/lib/faad2/include
  ${REPO_ROOT}/lib/minimp3/src
)
target_include_directories(decoders_host PRIVATE ${REPO_ROOT}/lib/faad2/libfaad)
target_compile_definitions(decoders_host PRIVATE HAVE_CONFIG_H)
target_compile_options(decoders_host PRIVATE -w)

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
  decoders_host
  GTest::gtest_main
  m
)
gtest_discover_tests(DecodeChainTest)

add_executable(BookModelTest BookModelTest.cpp ${REPO_ROOT}/lib/Audiobook/BookModel.cpp)
target_include_directories(BookModelTest PRIVATE ${REPO_ROOT}/lib/Audiobook)
target_link_libraries(BookModelTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BookModelTest)
