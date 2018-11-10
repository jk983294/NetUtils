file( GLOB BALANCER_SOURCES "*.cpp" )
add_executable( balancer ${BALANCER_SOURCES} )
target_link_libraries( balancer ${Boost_LIBRARIES} net_utils_common_lib pthread )