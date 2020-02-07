cmake_minimum_required(VERSION 3.10)
project(RTSPClient)

set(CMAKE_CXX_STANDARD 11)

find_package(OpenCV REQUIRED)
include_directories(${OpenCV_INCLUDE_DIRS})

set(LOCAL_INC "/usr/local/include")
include_directories(${LOCAL_INC}/liveMedia)
include_directories(${LOCAL_INC}/BasicUsageEnvironment)
include_directories(${LOCAL_INC}/UsageEnvironment)
include_directories(${LOCAL_INC}/groupsock)
# link_directories("${LOCAL_LIB}/UsageEnvironment") 
# link_directories("${LOCAL_LIB}/groupsock") 
# link_directories("${LOCAL_LIB}/liveMedia")

add_executable(RTSP_265_Client RTSP_265_Client.cpp)
set(LIVE_LIBRARIES liveMedia groupsock  BasicUsageEnvironment UsageEnvironment)
target_link_libraries(RTSP_265_Client  ${OpenCV_LIBS} ${LIVE_LIBRARIES} -lssl -lcrypto)
# target_link_libraries(CaptureIPCamera ${OpenCV_LIBS})
