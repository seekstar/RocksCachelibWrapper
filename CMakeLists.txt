cmake_minimum_required(VERSION 3.15)
project(RocksCachelibWrapper CXX)

if(NOT DEFINED CACHELIB_HOME)
	if(NOT DEFINED ENV{CACHELIB_HOME})
		message(FATAL_ERROR "CACHELIB_HOME not set")  
	endif()
	set(CACHELIB_HOME $ENV{CACHELIB_HOME})
endif()

if(NOT DEFINED ROCKSDB_INCLUDE)
	if(NOT DEFINED ENV{ROCKSDB_INCLUDE})
		message(FATAL_ERROR "ROCKSDB_INCLUDE not set")  
	endif()
	set(ROCKSDB_INCLUDE $ENV{ROCKSDB_INCLUDE})
endif()

if(NOT DEFINED ROCKSDB_LIB)
	if(NOT DEFINED ENV{ROCKSDB_LIB})
		message(FATAL_ERROR "ROCKSDB_LIB not set")  
	endif()
	set(ROCKSDB_LIB $ENV{ROCKSDB_LIB})
endif()

add_compile_options(-Wall -Wextra -fexceptions)

aux_source_directory(src SRCS)
add_library(${PROJECT_NAME} ${SRCS})

target_include_directories(${PROJECT_NAME}
	PUBLIC
		include
		${ROCKSDB_INCLUDE}
		${CACHELIB_HOME}/include
)
target_link_directories(${PROJECT_NAME}
	PUBLIC
		${ROCKSDB_LIB}
		${CACHELIB_HOME}/lib
)
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
