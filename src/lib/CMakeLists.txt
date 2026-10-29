file(GLOB_RECURSE HDR_FILES *.hpp)
file(GLOB_RECURSE SRC_FILES *.cpp)

add_library(common-lib ${HDR_FILES} ${SRC_FILES})

target_compile_options(common-lib PRIVATE -Wall -Wextra -pedantic -Wno-unused-parameter)

target_include_directories(common-lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(common-lib utils)
target_link_libraries(common-lib asn-ngap)
target_link_libraries(common-lib asn-rrc)

find_path(SCTP_INCLUDE_DIR netinet/sctp.h)
find_library(SCTP_LIBRARY NAMES sctp)
if(NOT SCTP_INCLUDE_DIR OR NOT SCTP_LIBRARY)
    message(FATAL_ERROR "SCTP development files are required. Install libsctp-dev (Ubuntu/Debian), or set SCTP_INCLUDE_DIR and SCTP_LIBRARY.")
endif()
target_include_directories(common-lib PRIVATE ${SCTP_INCLUDE_DIR})
target_link_libraries(common-lib ${SCTP_LIBRARY})
