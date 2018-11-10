file( GLOB PROXY_SOURCES "*.cpp" )
add_executable( proxy ${PROXY_SOURCES} )
target_link_libraries( proxy ${Boost_LIBRARIES} net_utils_common_lib pthread )