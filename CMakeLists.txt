cmake_minimum_required(VERSION 3.10)
project(RTSPClient)

set(CMAKE_CXX_STANDARD 11)
set( CMAKE_VERBOSE_MAKEFILE on )
set(CMAKE_EXPORT_COMPILE_COMMANDS on)
set( LINKER_FLAGS "${LINKER_FLAGS} -g -O2 -W -Wall" )
set(COMMON_FLAGS "-O2 -W -Wall")

find_package(OpenCV REQUIRED)
include_directories(${OpenCV_INCLUDE_DIRS})

find_package(SDL2 REQUIRED)
include_directories(${SDL2_INCLUDE_DIRS})

set(LOCAL_INC "/usr/local/include")
include_directories(${LOCAL_INC}/liveMedia)
include_directories(${LOCAL_INC}/BasicUsageEnvironment)
include_directories(${LOCAL_INC}/UsageEnvironment)
include_directories(${LOCAL_INC}/groupsock)
# link_directories("${LOCAL_LIB}/UsageEnvironment") 
# link_directories("${LOCAL_LIB}/groupsock") 
# link_directories("${LOCAL_LIB}/liveMedia")

add_executable(RTSPClient RTSPClient.cpp)
set(LIVE_LIBRARIES liveMedia groupsock  BasicUsageEnvironment UsageEnvironment)
target_link_libraries(RTSPClient  ${OpenCV_LIBS} ${LIVE_LIBRARIES} -lssl -lcrypto  avcodec avformat avutil ${SDL2_LIBRARIES} swscale)
# target_link_libraries(CaptureIPCamera ${OpenCV_LIBS})
