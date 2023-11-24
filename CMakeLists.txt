cmake_minimum_required(VERSION 3.15)
project(RocksCachelibWrapper CXX)

add_compile_options(-Wall -Wextra -fexceptions)

aux_source_directory(src SRCS)
add_library(${PROJECT_NAME} ${SRCS})

target_include_directories(${PROJECT_NAME} PUBLIC include)
target_link_libraries(${PROJECT_NAME}
	PRIVATE
		cachelib_allocator
		cachelib_navy
		cachelib_common
		cachelib_shm
	PUBLIC
		rocksdb

		# Deps of cachelib_allocator
		numa
		folly
		thriftprotocol

		# Deps of cachelib_navy
		boost_context

		# Deps of cachelib_shm
		rt

		# Deps of folly
		fmt
		double-conversion
		glog

		# Deps of thriftprotocol
		rpcmetadata
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)
