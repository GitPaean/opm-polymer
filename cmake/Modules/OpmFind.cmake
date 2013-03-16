# - Generic inclusion of packages
#
# Synopsis:
#
#	find_and_append_package (name args)
#
# where
#
#	name          Name of the package, e.g. Boost
#   args          Other arguments, e.g. COMPONENTS, REQUIRED, QUIET etc.
#
# This macro will append the list of standard variables found by the
# package to this project's standard variables
#
########################################################################
#
# - Generic inclusion of a list of packages
#
# Synopsis:
#
#	find_and_append_package_list (args)
#
# where
#
#	args          List of package strings. Each string must be quoted if
#	              it contains more than one word.
#
# Example:
#
#	find_and_append_package_list (
#		"Boost COMPONENTS filesystem REQUIRED"
#		SUPERLU
#	)

include (Duplicates)

# list of suffixes for all the project variables
set (_opm_proj_vars
  LINKER_FLAGS
  LIBRARIES
  DEFINITIONS
  INCLUDE_DIRS
  LIBRARY_DIRS
  CONFIG_VARS
  )

# ensure that they are at least the empty list after we're done
foreach (name IN LISTS _opm_proj_vars)
  if (NOT DEFINED ${CMAKE_PROJECT_NAME}_${name})
	set (${CMAKE_PROJECT_NAME}_${name} "")
  endif (NOT DEFINED ${CMAKE_PROJECT_NAME}_${name})
endforeach (name)

# these dependencies must always be handled by the find module
set (_opm_proj_exemptions
  dune-common
  dune-istl
  dune-grid
  dune-geometry
  )

# insert this boilerplate whenever we are going to find a new package
macro (find_and_append_package_to prefix name)
  # if we have specified a directory, don't revert to searching the
  # system default paths afterwards
  string (TOUPPER "${name}" NAME)
  string (REPLACE "-" "_" NAME "${NAME}")
  # the documentation says that if *-config.cmake files are not found,
  # find_package will revert to doing a full search, but that is not
  # true, so unconditionally setting ${name}_DIR is not safe. however,
  # if the directory given to us contains a config file, then copy the
  # value over to this variable to switch to config mode (CMake will
  # always use config mode if *_DIR is defined)
  if (NOT DEFINED ${name}_DIR AND (DEFINED ${name}_ROOT OR DEFINED ${NAME}_ROOT))
	if (EXISTS ${${name}_ROOT}/${name}-config.cmake OR EXISTS ${${name}_ROOT}/${name}Config.cmake)
	  set (${name}_DIR "${${name}_ROOT}")
	endif (EXISTS ${${name}_ROOT}/${name}-config.cmake OR EXISTS ${${name}_ROOT}/${name}Config.cmake)
	if (EXISTS ${${NAME}_ROOT}/${name}-config.cmake OR EXISTS ${${NAME}_ROOT}/${name}Config.cmake)
	  set (${name}_DIR "${${NAME}_ROOT}")
	endif (EXISTS ${${NAME}_ROOT}/${name}-config.cmake OR EXISTS ${${NAME}_ROOT}/${name}Config.cmake)
  endif (NOT DEFINED ${name}_DIR AND (DEFINED ${name}_ROOT OR DEFINED ${NAME}_ROOT))

  # these libraries need special handling which is not provided in
  # the -config.cmake file, but which must be provided by this project,
  # something which is done in our find module
  list (FIND _opm_proj_exemptions "${name}" _${name}_exempted)
  if ((NOT (_${name}_exempted EQUAL -1)) AND (DEFINED ${name}_DIR))
	# most often we are given the name to the build directory and this
	# is a sub-directory of the source tree
	if (${name}_DIR MATCHES "build")
	  get_filename_component (${name}_ROOT "${${name}_DIR}" PATH)
	else (${name}_DIR MATCHES "build")
	  set (${name}_ROOT "${${name}_DIR}")
	endif (${name}_DIR MATCHES "build")
	# store this for later, in case we reconfigure
	set (${name}_ROOT "${${name}_ROOT}" CACHE LOCATION "Path to ${name}")
	# clear this to not use config mode
	unset (${name}_DIR)
	# variables that are given on the command-line is also put in the cache
	# removing the local copy only "unshadows" this one
	unset (${name}_DIR CACHE)
  endif ((NOT (_${name}_exempted EQUAL -1)) AND (DEFINED ${name}_DIR))

  # using config mode is better than using module (aka. find) mode
  # because then the package has already done all its probes and
  # stored them in the config file for us
  if (${name}_DIR)
	message (STATUS "Finding package ${name} using config mode")
	find_package (${name} ${ARGN} NO_MODULE PATHS ${${name}_DIR} NO_DEFAULT_PATH)
  else (${name}_DIR)
	message (STATUS "Finding package ${name} using module mode")
	find_package (${name} ${ARGN})
  endif (${name}_DIR)

  # the variable "NAME" may be replaced during find_package (as this is
  # now a macro, and not a function anymore), so we must reinitialize
  string (TOUPPER "${name}" NAME)
  string (REPLACE "-" "_" NAME "${NAME}")

  if (${name}_FOUND OR ${NAME}_FOUND)
	foreach (var IN LISTS _opm_proj_vars)
	  if (DEFINED ${name}_${var})
		list (APPEND ${prefix}_${var} ${${name}_${var}})
	  # some packages define an uppercase version of their own name
	  elseif (DEFINED ${NAME}_${var})
		list (APPEND ${prefix}_${var} ${${NAME}_${var}})
	  endif (DEFINED ${name}_${var})
	  # some packages define _PATH instead of _DIRS (Hi, MPI!)
	  if ("${var}" STREQUAL "INCLUDE_DIRS")
		if (DEFINED ${name}_INCLUDE_PATH)
		  list (APPEND ${prefix}_INCLUDE_DIRS ${${name}_INCLUDE_PATH})
		elseif (DEFINED ${NAME}_INCLUDE_PATH)
		  list (APPEND ${prefix}_INCLUDE_DIRS ${${NAME}_INCLUDE_PATH})
		endif (DEFINED ${name}_INCLUDE_PATH)
	  endif ("${var}" STREQUAL "INCLUDE_DIRS")
	  # cleanup lists
	  if ("${var}" STREQUAL "LIBRARIES")
		remove_duplicate_libraries (${prefix})
	  else ("${var}" STREQUAL "LIBRARIES")
		remove_duplicate_var (${prefix} ${var})
	  endif ("${var}" STREQUAL "LIBRARIES")
	endforeach (var)
	# some libraries only define xxx_FOUND and not a corresponding HAVE_xxx
	if (NOT DEFINED HAVE_${NAME})
	  set (HAVE_${NAME} 1)
	endif (NOT DEFINED HAVE_${NAME})
  endif (${name}_FOUND OR ${NAME}_FOUND)
endmacro (find_and_append_package_to prefix name)

# append to the list of variables associated with the project
macro (find_and_append_package name)
  find_and_append_package_to (${CMAKE_PROJECT_NAME} ${name} ${ARGN})
endmacro (find_and_append_package name)

# find a list of dependencies, adding each one of them
macro (find_and_append_package_list_to prefix)
  # setting and separating is necessary to work around apparent bugs
  # in CMake's parser (sic)
  set (_deps ${ARGN})
  foreach (_dep IN LISTS _deps)
	separate_arguments (_args UNIX_COMMAND ${_dep})
	find_and_append_package_to (${prefix} ${_args})
  endforeach (_dep)
endmacro (find_and_append_package_list_to prefix)

# convenience method to supply the project name as prefix
macro (find_and_append_package_list)
  find_and_append_package_list_to (${CMAKE_PROJECT_NAME} ${ARGN})
endmacro (find_and_append_package_list)
