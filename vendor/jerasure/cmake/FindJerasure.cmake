# -*- cmake -*-

# - Find Apache Portable Runtime
# Find the APR includes and libraries
# This module defines
#  APRUTIL_INCLUDE_DIR and APRUTIL_INCLUDE_DIR, where to find apr.h, etc.
#  APRUTIL_LIBRARIES and APRUTIL_LIBRARIES, the libraries needed to use APR.
#  APRUTIL_FOUND and APRUTIL_FOUND, If false, do not try to use APR.
# also defined, but not for general use are
#  APRUTIL_LIBRARY and APRUTIL_LIBRARY, where to find the APR library.

# APR first.

FIND_PATH(APRUTIL_INCLUDE_DIR apr.h
/usr/local/include/apr-1
/usr/local/include/apr-1.0
/usr/include/apr-1
/usr/include/apr-1.0
$ENV{HOME}/include/apr-1
$ENV{HOME}/include/apr-1.0
$ENV{CMAKE_PREFIX_PATH}/include/apr-1
$ENV{CMAKE_PREFIX_PATH}/include/apr-1.0
$ENV{CMAKE_INCLUDE_PATH}/apr-1
$ENV{CMAKE_INCLUDE_PATH}/apr-1.0
)

SET(APRUTIL_NAMES ${APRUTIL_NAMES} aprutil-1)
FIND_LIBRARY(APRUTIL_LIBRARY
  NAMES ${APRUTIL_NAMES}
  PATHS /usr/lib /usr/local/lib $ENV{HOME}/lib $ENV{CMAKE_PREFIX_PATH}/lib $ENV{CMAKE_LIBRARY_PATH}
  )

IF (APRUTIL_LIBRARY AND APRUTIL_INCLUDE_DIR)
    SET(APRUTIL_LIBRARIES ${APRUTIL_LIBRARY})
    SET(APRUTIL_FOUND "YES")
ELSE (APRUTIL_LIBRARY AND APRUTIL_INCLUDE_DIR)
  SET(APRUTIL_FOUND "NO")
ENDIF (APRUTIL_LIBRARY AND APRUTIL_INCLUDE_DIR)


IF (APRUTIL_FOUND)
   IF (NOT APRUTIL_FIND_QUIETLY)
      MESSAGE(STATUS "Found APRUtil: ${APRUTIL_LIBRARIES}")
   ENDIF (NOT APRUTIL_FIND_QUIETLY)
ELSE (APRUTIL_FOUND)
   IF (APRUTIL_FIND_REQUIRED)
      MESSAGE(FATAL_ERROR "Could not find APRUtil library")
   ENDIF (APRUTIL_FIND_REQUIRED)
ENDIF (APRUTIL_FOUND)

# Deprecated declarations.
SET (NATIVE_APRUTIL_INCLUDE_PATH ${APRUTIL_INCLUDE_DIR} )
GET_FILENAME_COMPONENT (NATIVE_APRUTIL_LIB_PATH ${APRUTIL_LIBRARY} PATH)

MARK_AS_ADVANCED(
  APRUTIL_LIBRARY
  APRUTIL_INCLUDE_DIR
  )