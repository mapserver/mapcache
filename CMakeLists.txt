cmake_minimum_required (VERSION 2.6)

project (MapCache)

include(CheckFunctionExists)
include(CheckIncludeFile)
include(CheckCSourceCompiles)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall")
if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug") 
   add_definitions(-DDEBUG) 
endif () 


set (MAPCACHE_VERSION_MAJOR 1)
set (MAPCACHE_VERSION_MINOR 3)
set (MAPCACHE_VERSION_REVISION 0)


if(NOT DEFINED CMAKE_INSTALL_LIBDIR)
   set(CMAKE_INSTALL_LIBDIR lib)
endif(NOT DEFINED CMAKE_INSTALL_LIBDIR)
if(NOT DEFINED CMAKE_INSTALL_BINDIR)
   set(CMAKE_INSTALL_BINDIR bin)
endif(NOT DEFINED CMAKE_INSTALL_BINDIR)

MATH(EXPR MAPCACHE_IS_DEV_VERSION "1-${MAPCACHE_VERSION_MINOR}%2")
if(MAPCACHE_IS_DEV_VERSION)
  set (MAPCACHE_VERSION_STRING "${MAPCACHE_VERSION_MAJOR}.${MAPCACHE_VERSION_MINOR}.${MAPCACHE_VERSION_REVISION}")
else(MAPCACHE_IS_DEV_VERSION)
  set (MAPCACHE_VERSION_STRING "${MAPCACHE_VERSION_MAJOR}.${MAPCACHE_VERSION_MINOR}dev")
endif(MAPCACHE_IS_DEV_VERSION)
MATH(EXPR MAPCACHE_VERSION_NUM "${MAPCACHE_VERSION_MAJOR}*10000+${MAPCACHE_VERSION_MINOR}*100+${MAPCACHE_VERSION_REVISION}")

SET(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake ${CMAKE_MODULE_PATH})
if (APPLE)
  set(CMAKE_FIND_FRAMEWORK "LAST")
endif (APPLE)

macro( report_optional_not_found component )
  message(SEND_ERROR "${component} library/component/dependency could not be found.
  HINTS:
  - disable ${component} support by adding -DWITH_${component}=0
  - add the ${component} install directory to the CMAKE_PREFIX_PATH variable (-DCMAKE_PREFIX_PATH=\"/path/to/${component}-install-dir;/path/to/other/dirs\"")
endmacro()
macro( report_mandatory_not_found component )
  message(SEND_ERROR "${component} library/component could not be found and is a mandatory dependency
  HINT:
  - add the ${component} install directory to the CMAKE_PREFIX_PATH variable (-DCMAKE_PREFIX_PATH=\"/path/to/${component}-install-dir;/path/to/other/dirs\"")
endmacro()
macro( report_dependency_error component dependency)
  message(SEND_ERROR "${component} support requires ${dependency} support, however ${dependency} support has been disabled.
  HINTS:
  - re-run with -DWITH_${dependency}=1 (or without -DWITH_${dependency}=0)
  - disable ${component} support by adding -DWITH_${component}=0"
  )
endmacro()

check_function_exists("strncasecmp"  HAVE_STRNCASECMP)
check_function_exists("symlink"  HAVE_SYMLINK)


set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_LINK_INTERFACE_LIBRARY "")

file(GLOB mapcache_SOURCES lib/*.c )

add_library(mapcache SHARED ${mapcache_SOURCES})
set_target_properties(mapcache PROPERTIES
  VERSION ${MAPCACHE_VERSION_STRING}
  SOVERSION 1
)

#options suported by the cmake builder
option(WITH_PIXMAN "Use pixman for SSE optimized image manipulations" ON)
option(WITH_SQLITE "Use sqlite as a cache backend" ON)
option(WITH_BERKELEY_DB "Use Berkeley DB as a cache backend" OFF)
option(WITH_MEMCACHE "Use memcache as a cache backend (requires recent apr-util)" OFF)
option(WITH_REDIS "Use redis as a cache backend (requires hiredis library)" OFF)
option(WITH_TIFF "Use TIFFs as a cache backend" OFF)
option(WITH_TIFF_WRITE_SUPPORT "Enable (experimental) support for writable TIFF cache backends" OFF)
option(WITH_GEOTIFF "Allow GeoTIFF metadata creation for TIFF cache backends" OFF)
option(WITH_PCRE "Use PCRE for regex tests" OFF)
option(WITH_MAPSERVER "Enable (experimental) support for the mapserver library" OFF)

find_package(PNG)
if(PNG_FOUND)
  include_directories(${PNG_INCLUDE_DIR})
  target_link_libraries(mapcache ${PNG_LIBRARY})
else(PNG_FOUND)
  report_mandatory_not_found(PNG)
endif(PNG_FOUND)

find_package(JPEG)
if(JPEG_FOUND)
  include_directories(${JPEG_INCLUDE_DIR})
  target_link_libraries(mapcache ${JPEG_LIBRARY})
else(JPEG_FOUND)
endif(JPEG_FOUND)
   
find_package(CURL)
if(CURL_FOUND)
   include_directories(${CURL_INCLUDE_DIR})
   target_link_libraries(mapcache ${CURL_LIBRARY})
else(CURL_FOUND)
   report_mandatory_not_found(CURL)
endif(CURL_FOUND)

find_package(APR)
if(APR_FOUND)
   include_directories(${APR_INCLUDE_DIR} ${APU_INCLUDE_DIR})
   target_link_libraries(mapcache ${APR_LIBRARY} ${APU_LIBRARY})
   if(DEFINED APR_CPPFLAGS)
     add_definitions("${APR_CPPFLAGS}")
   endif(DEFINED APR_CPPFLAGS)
else(APR_FOUND)
   report_mandatory_not_found(APR)
endif(APR_FOUND)

if(WITH_MEMCACHE)
  include(CheckSymbolExists)
   FIND_PATH(tmp_APU_MEMCACHE_DIR
     NAMES apr_memcache.h
     HINTS ${APU_INCLUDE_DIR}
  )
  if(tmp_APU_MEMCACHE_DIR)
    set(CMAKE_REQUIRED_INCLUDE ${APR_INCLUDE_DIR} ${APU_INCLUDE_DIR})
    set(CMAKE_REQUIRED_LIBRARIES ${APR_LIBRARY} ${APU_LIBRARY})
    check_symbol_exists(apr_memcache_hash "${tmp_APU_MEMCACHE_DIR}/apr_memcache.h" tmp_MEMCACHE_USABLE)
    if(tmp_MEMCACHE_USABLE)
      set(USE_MEMCACHE 1)
    else(tmp_MEMCACHE_USABLE)
      MESSAGE(SEND_ERROR "apr_memcache.h found, but seems too old (missing apr_memcache_hash function)")
      report_optional_not_found(MEMCACHE)
    endif(tmp_MEMCACHE_USABLE)
  else(tmp_APU_MEMCACHE_DIR)
    MESSAGE(SEND_ERROR "apr_memcache.h not found, your apr-util version may be configured without memcache support")
    report_optional_not_found(MEMCACHE)
  endif(tmp_APU_MEMCACHE_DIR)
  
endif(WITH_MEMCACHE)

if(WITH_REDIS)
  find_package(Redis)
  if(HIREDIS_FOUND)
    include_directories(${HIREDIS_INCLUDE_DIR})
    target_link_libraries(mapcache ${HIREDIS_LIBRARIES})
    set (USE_REDIS 1)
  else(HIREDIS_FOUND)
    report_optional_not_found(Redis)
  endif(HIREDIS_FOUND)
endif(WITH_REDIS)

if(WITH_PIXMAN)
  find_package(Pixman)
  if(PIXMAN_FOUND)
    include_directories(${PIXMAN_INCLUDE_DIR})
    target_link_libraries(mapcache ${PIXMAN_LIBRARY})
    set (USE_PIXMAN 1)
  else(PIXMAN_FOUND)
    report_optional_not_found(PIXMAN)
  endif(PIXMAN_FOUND)
endif (WITH_PIXMAN)

if(WITH_PCRE)
   find_package(PCRE)
  if(PCRE_FOUND)
    include_directories(${PCRE_INCLUDE_DIR})
    target_link_libraries(mapcache ${PCRE_LIBRARY})
    set (USE_PCRE 1)
  else(PCRE_FOUND)
    report_optional_not_found(PCRE)
  endif(PCRE_FOUND)
endif (WITH_PCRE)

if(WITH_SQLITE)
  find_package(SQLITE)
  if(SQLITE_FOUND)
    include_directories(${SQLITE_INCLUDE_DIR})
    target_link_libraries(mapcache ${SQLITE_LIBRARY})
    set (USE_SQLITE 1)
  else(SQLITE_FOUND)
    report_optional_not_found(SQLITE)
  endif(SQLITE_FOUND)
endif (WITH_SQLITE)

if(WITH_BERKELEY_DB)
  if(NOT BERKELEYDB_FIND_VERSION)
    set(BERKELEYDB_FIND_VERSION "4.6")
  endif(NOT BERKELEYDB_FIND_VERSION)
  find_package(BerkeleyDB)
  if(BERKELEYDB_FOUND)
    include_directories(${BERKELEYDB_INCLUDE_DIR})
    target_link_libraries(mapcache ${BERKELEYDB_LIBRARY})
    set (USE_BDB 1)
  else(BERKELEYDB_FOUND)
    report_optional_not_found(BERKELEY_DB)
  endif(BERKELEYDB_FOUND)
endif (WITH_BERKELEY_DB)

if(WITH_TIFF)
  find_package(TIFF)
  if(TIFF_FOUND)
    include_directories(${TIFF_INCLUDE_DIR})
    target_link_libraries(mapcache ${TIFF_LIBRARY})
    set (USE_TIFF 1)
  else(TIFF_FOUND)
    report_optional_not_found(TIFF)
  endif(TIFF_FOUND)
endif (WITH_TIFF)

if(WITH_TIFF_WRITE_SUPPORT)
  if(USE_TIFF)
    set (USE_TIFF_WRITE 1)
  else(USE_TIFF)
    report_dependency_error(TIFF_WRITE_SUPPORT TIFF)
  endif(USE_TIFF)
endif(WITH_TIFF_WRITE_SUPPORT)


if(WITH_GEOTIFF)
  find_package(GEOTIFF)
  if(GEOTIFF_FOUND)
    include_directories(${GEOTIFF_INCLUDE_DIR})
    target_link_libraries(mapcache ${GEOTIFF_LIBRARY})
    set (USE_GEOTIFF 1)
  else(GEOTIFF_FOUND)
    report_optional_not_found(GEOTIFF)
  endif(GEOTIFF_FOUND)
endif (WITH_GEOTIFF)

if(WITH_MAPSERVER)
   find_package(MAPSERVER)
  if(MAPSERVER_FOUND)
    include_directories(${MAPSERVER_INCLUDE_DIR})
    target_link_libraries(mapcache ${MAPSERVER_LIBRARY})
    set (USE_MAPSERVER 1)
  else(MAPSERVER_FOUND)
    report_optional_not_found(MAPSERVER)
  endif(MAPSERVER_FOUND)
endif (WITH_MAPSERVER)


if(UNIX)
target_link_libraries(mapcache ${CMAKE_DL_LIBS} m )
endif(UNIX)

configure_file (
  "${PROJECT_SOURCE_DIR}/include/mapcache-config.h.in"
  "${PROJECT_BINARY_DIR}/include/mapcache-config.h"
  )

configure_file (
  "${PROJECT_SOURCE_DIR}/include/mapcache-version.h.in"
  "${PROJECT_BINARY_DIR}/include/mapcache-version.h"
  )

include_directories(include ${PROJECT_BINARY_DIR}/include)

macro(status_optional_component component enabled libpath)
  if("${enabled}" EQUAL "1")
    message(STATUS "  * ${component}: ${libpath}")
  else()
    message(STATUS "  * ${component}: disabled")
  endif()
endmacro()
macro(status_optional_feature feature enabled)
  if("${enabled}" EQUAL "1")
    message(STATUS "  * ${feature}: ENABLED")
  else()
    message(STATUS "  * ${feature}: disabled")
  endif()
endmacro()

message(STATUS "* Configured options for the mapcache library")
message(STATUS " * Mandatory components")
message(STATUS "  * png: ${PNG_LIBRARY}")
message(STATUS "  * jpeg: ${JPEG_LIBRARY}")
message(STATUS "  * Curl: ${CURL_LIBRARY}")
message(STATUS "  * Apr: ${APR_LIBRARY}")
message(STATUS " * Optional components")
status_optional_component("PIXMAN" "${USE_PIXMAN}" "${PIXMAN_LIBRARY}")
status_optional_component("SQLITE" "${USE_SQLITE}" "${SQLITE_LIBRARY}")
status_optional_component("Berkeley DB" "${USE_BDB}" "${BERKELEYDB_LIBRARY}")
status_optional_component("Memcache" "${USE_MEMCACHE}" "${APU_LIBRARY}")
status_optional_component("Redis" "${USE_REDIS}" "${HIREDIS_LIBRARIES} ${HIREDIS_INCLUDE_DIR}")
status_optional_component("TIFF" "${USE_TIFF}" "${TIFF_LIBRARY}")
status_optional_component("GeoTIFF" "${USE_GEOTIFF}" "${GEOTIFF_LIBRARY}")
status_optional_component("Experimental TIFF write support" "${USE_TIFF_WRITE}" "${TIFF_LIBRARY}")
status_optional_component("PCRE" "${USE_PCRE}" "${PCRE_LIBRARY}")
status_optional_component("Experimental mapserver support" "${USE_MAPSERVER}" "${MAPSERVER_LIBRARY}")

INSTALL(TARGETS mapcache DESTINATION ${CMAKE_INSTALL_LIBDIR})

add_subdirectory(util)
add_subdirectory(cgi)
add_subdirectory(apache)
add_subdirectory(nginx)
