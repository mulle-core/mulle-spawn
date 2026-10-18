#
# You rarely need to edit this file. To add or remove files in your
# project edit the "src" folders contents and run `mulle-sde reflect`
#
#
# A convenient way to add a new source file is:
#    mulle-sde add src/foo.c
#
cmake_minimum_required( VERSION 3.13...99.99)

project( mulle-spawn VERSION 1.0.0 LANGUAGES C)

# Force-link this library into its consumers. Objective-C libraries with
# atinit/atexit constructors want this ON, so their objects end up in the
# consumer even when no symbol of them is referenced. A plain C library that
# has no such constructors should turn it OFF, so consumers do not
# whole-archive it needlessly.
option( MULLE_LIBRARY_FORCE_LINK
        "Force-link this library into consumers"
        ON)

# if use embedded project with symlinks and you want to distribute their
# headers you probably need to turn this on
SET( RESOLVE_INSTALLABLE_HEADER_SYMLINKS OFF CACHE BOOL "Copy headers as files")

set( MULLE_PROJECT_TYPE "Library")

#
# The library advertises C11 (see the "std" badge) and XSI/X-Open (see the
# "apis: xopen/windows" badge, which is where realpath and PATH_MAX live).
# Compile it as C11 with extensions off and ask for exactly that namespace:
# _XOPEN_SOURCE=700 gives POSIX.1-2008 plus XSI and hides GNU extensions, so
# anything beyond the badge is a compile error rather than a silent ratchet.
#
#
# On Apple, _DARWIN_C_SOURCE is the counterpart of glibc's _DEFAULT_SOURCE:
# strict C mode hides the POSIX and BSD declarations there, including the
# SO_NOSIGPIPE socket option this library sets on macOS.
#
# A standard given on the command line wins, so other dialects remain testable
# with e.g. -DCMAKE_C_STANDARD=23 or CFLAGS=-std=c23.
#
if( NOT CMAKE_C_STANDARD
    AND NOT CMAKE_C_FLAGS MATCHES "(^| )-std="
    AND NOT "$ENV{CFLAGS}" MATCHES "(^| )-std=")
   set( CMAKE_C_STANDARD 11)
   set( CMAKE_C_EXTENSIONS OFF)
   if( APPLE)
      add_compile_definitions( _DARWIN_C_SOURCE)
   elseif( NOT WIN32)
      add_compile_definitions( _XOPEN_SOURCE=700)
   endif()
endif()

### mulle-sde environment

set( MULLE_SDE_REFLECT_DIR "reflect")

if( NOT MULLE_SOURCETREE_CONFIG_NAME)
   set( MULLE_SOURCETREE_CONFIG_NAME "$ENV{MULLE_SOURCETREE_CONFIG_NAME}")
endif()
if( MULLE_SOURCETREE_CONFIG_NAME)
   set( MULLE_SDE_REFLECT_DIR "reflect.${MULLE_SOURCETREE_CONFIG_NAME}")
endif()

# add cmake module paths to search path
list( INSERT CMAKE_MODULE_PATH 0 "${CMAKE_CURRENT_SOURCE_DIR}/cmake/share")
list( INSERT CMAKE_MODULE_PATH 0 "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${MULLE_SDE_REFLECT_DIR}")
list( INSERT CMAKE_MODULE_PATH 0 "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include( Environment)

include( Files)

#
# IDE support only makes sense in a mulle-sde build, and USE_DEPENDENCY_DIR
# is currently the best indicator we have for that
#
if( USE_DEPENDENCY_DIR)
   include( IDESupport OPTIONAL)
endif()


if( "${MULLE_PROJECT_TYPE}" STREQUAL "Executable")

   ### Executable

   include( Dependencies)
   include( Executable)

   # use EXECUTABLE_COMPILE_TARGET for compiler options
   target_compile_definitions( "${EXECUTABLE_COMPILE_TARGET}" PUBLIC)
   target_compile_options( "${EXECUTABLE_COMPILE_TARGET}" PUBLIC)
   # use EXECUTABLE_LINK_TARGET for linker options
   if( LINK_PHASE)
      target_link_options( "${EXECUTABLE_LINK_TARGET}" PUBLIC)
   endif()

   include( InstallExecutable)

   # only for Executable
   include( Motd OPTIONAL)


elseif( "${MULLE_PROJECT_TYPE}" STREQUAL "Framework")

   ### Framework

   if( LINK_PHASE)
      include( Dependencies)
   endif()

   include( Framework)
   include( InstallFramework)

elseif( "${MULLE_PROJECT_TYPE}" STREQUAL "Library")

   ### Library

   if( HEADER_PHASE)
      install( FILES ${INSTALL_PUBLIC_HEADERS}  DESTINATION "include/${PROJECT_NAME}")
      install( FILES ${INSTALL_PRIVATE_HEADERS} DESTINATION "include/${PROJECT_NAME}")
      install( FILES ${INSTALL_CMAKE_INCLUDES}  DESTINATION "include/${PROJECT_NAME}/cmake")

      # short cut out
      if( NOT COMPILE_PHASE AND NOT LINK_PHASE)
         return()
      endif()
   endif()

   if( LINK_PHASE)
      include( Dependencies OPTIONAL)
   endif()

   include( Library)

   # use LIBRARY_COMPILE_TARGET for compiler options
   target_compile_definitions( "${LIBRARY_COMPILE_TARGET}" PUBLIC)
   target_compile_options( "${LIBRARY_COMPILE_TARGET}" PUBLIC)
   # use LIBRARY_LINK_TARGET for linker options
   if( LINK_PHASE)
      target_link_options( "${LIBRARY_LINK_TARGET}" PUBLIC)
   endif()

   include( InstallLibrary)

   if( LINK_PHASE)
      #
      # In a mulle-sde build the headers are installed into the SDK, so this
      # path is only for plain cmake consumers
      #
      if( NOT USE_DEPENDENCY_DIR)
         include( AddSubdirectorySupport)
      endif()
      include( FindPackageSupport)

      #
      # Force-link propagation for force-linkable initialization libraries.
      # Controlled by MULLE_LIBRARY_FORCE_LINK (default ON, see above) and a
      # silent no-op below CMake 3.24, so it is safe for every project.
      #
      include( AllLoadSupport)
   endif()

else()

   message( ERROR "Unknown project type \"${MULLE_PROJECT_TYPE}\"")

endif()



include( FinalOutput OPTIONAL)
