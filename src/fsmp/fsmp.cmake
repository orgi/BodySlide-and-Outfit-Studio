# Builds FSMP (lib/hdtSMP64) from its unmodified sources for the leveled list
# previewer, together with the Bullet version FSMP ships with (lib/bullet3 plus
# the patch from FSMP's own vcpkg port). See src/components/agents.md.

set(FSMP_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../lib/hdtSMP64)
set(FSMP_STANDALONE_DIR ${CMAKE_CURRENT_LIST_DIR})
set(FSMP_BULLET_SOURCE ${CMAKE_CURRENT_LIST_DIR}/../../lib/bullet3/src)
set(FSMP_BULLET_DIR ${CMAKE_BINARY_DIR}/fsmp-bullet3)
set(FSMP_PATCHED_FILE src/BulletDynamics/ConstraintSolver/btGeneric6DofSpring2Constraint)

# ---------------------------------------------------------------------------
# Bullet sources with FSMP's patch applied
# ---------------------------------------------------------------------------

foreach(dir LinearMath BulletCollision BulletDynamics)
	file(COPY ${FSMP_BULLET_SOURCE}/${dir} DESTINATION ${FSMP_BULLET_DIR}/src
		PATTERN btGeneric6DofSpring2Constraint.* EXCLUDE)
endforeach()
file(COPY ${FSMP_BULLET_SOURCE}/btBulletCollisionCommon.h ${FSMP_BULLET_SOURCE}/btBulletDynamicsCommon.h
	DESTINATION ${FSMP_BULLET_DIR}/src)

# The port applies its source changes inside portfile.cmake, between these two markers.
set(_fsmp_portfile ${FSMP_ROOT}/cmake/ports/bullet3/portfile.cmake)
file(READ ${_fsmp_portfile} _fsmp_port)
string(FIND "${_fsmp_port}" "# Apply non-Hookean spring modifications" _fsmp_patch_begin)
string(FIND "${_fsmp_port}" "file(REMOVE_RECURSE \"\${SOURCE_PATH}/examples/ThirdPartyLibs\")" _fsmp_patch_end)
if(_fsmp_patch_begin EQUAL -1 OR _fsmp_patch_end EQUAL -1 OR _fsmp_patch_end LESS _fsmp_patch_begin)
	message(FATAL_ERROR "FSMP: the Bullet patch block was not found in ${_fsmp_portfile}")
endif()
math(EXPR _fsmp_patch_length "${_fsmp_patch_end} - ${_fsmp_patch_begin}")
string(SUBSTRING "${_fsmp_port}" ${_fsmp_patch_begin} ${_fsmp_patch_length} _fsmp_patch)

set(_fsmp_staging ${CMAKE_BINARY_DIR}/fsmp-bullet3-staging)
file(REMOVE_RECURSE ${_fsmp_staging})
foreach(ext h cpp)
	configure_file(${FSMP_BULLET_SOURCE}/BulletDynamics/ConstraintSolver/btGeneric6DofSpring2Constraint.${ext}
		${_fsmp_staging}/${FSMP_PATCHED_FILE}.${ext} COPYONLY)
endforeach()
file(WRITE ${_fsmp_staging}/patch.cmake "${_fsmp_patch}")
block(SCOPE_FOR VARIABLES)
	set(SOURCE_PATH ${_fsmp_staging})
	include(${_fsmp_staging}/patch.cmake)
endblock()

file(READ ${_fsmp_staging}/${FSMP_PATCHED_FILE}.h _fsmp_patched_h)
file(READ ${_fsmp_staging}/${FSMP_PATCHED_FILE}.cpp _fsmp_patched_cpp)
foreach(token setNonHookeanDamping setNonHookeanStiffness m_nonHookeanDamping m_nonHookeanStiffness)
	string(FIND "${_fsmp_patched_h}" "${token}" _fsmp_found)
	if(_fsmp_found EQUAL -1)
		message(FATAL_ERROR "FSMP: the Bullet patch from ${_fsmp_portfile} did not apply to btGeneric6DofSpring2Constraint.h (${token} missing)")
	endif()
endforeach()
string(FIND "${_fsmp_patched_cpp}" "setNonHookeanDamping" _fsmp_found)
if(_fsmp_found EQUAL -1)
	message(FATAL_ERROR "FSMP: the Bullet patch from ${_fsmp_portfile} did not apply to btGeneric6DofSpring2Constraint.cpp")
endif()
# The port's replacements are plain text searches. One that finds nothing leaves
# the source as it is, in FSMP's own build as well as here.
string(FIND "${_fsmp_patched_cpp}" "limot->m_nonHookeanDamping" _fsmp_found)
if(_fsmp_found EQUAL -1)
	message(STATUS "FSMP: the non-Hookean spring calculation of FSMP's Bullet patch does not match this Bullet version; as in FSMP's own build, the springs keep Bullet's calculation")
endif()
foreach(ext h cpp)
	configure_file(${_fsmp_staging}/${FSMP_PATCHED_FILE}.${ext} ${FSMP_BULLET_DIR}/${FSMP_PATCHED_FILE}.${ext} COPYONLY)
endforeach()

file(GLOB_RECURSE FSMP_BULLET_SOURCES
	${FSMP_BULLET_DIR}/src/LinearMath/*.cpp
	${FSMP_BULLET_DIR}/src/BulletCollision/*.cpp
	${FSMP_BULLET_DIR}/src/BulletDynamics/*.cpp)

# FSMP is built with BT_USE_SSE_IN_API and at least AVX. MSVC enables Bullet's
# SSE code paths by itself; other compilers get the same configuration here.
set(FSMP_BULLET_DEFINITIONS BT_USE_SSE_IN_API)
if(MSVC)
	set(FSMP_SIMD_OPTIONS /arch:AVX)
else()
	list(APPEND FSMP_BULLET_DEFINITIONS BT_USE_SSE BT_USE_SIMD_VECTOR3 BT_ALLOW_SSE4)
	set(FSMP_SIMD_OPTIONS -mavx "SHELL:-include immintrin.h")
endif()

# FSMP's port builds Bullet with its multithreading feature, and FSMP runs
# Bullet on its PPL task scheduler (include-noppl/ppl.h where PPL is missing).
list(APPEND FSMP_BULLET_DEFINITIONS BT_THREADSAFE=1 BT_USE_PPL=1)

# Only the FSMP targets see Bullet and the SIMD options; FsmpScene.h is free of both.
add_library(fsmp_bullet STATIC ${FSMP_BULLET_SOURCES})
target_include_directories(fsmp_bullet PRIVATE ${FSMP_BULLET_DIR}/src)
target_compile_definitions(fsmp_bullet PRIVATE ${FSMP_BULLET_DEFINITIONS})
target_compile_options(fsmp_bullet PRIVATE ${FSMP_SIMD_OPTIONS})
if(NOT MSVC)
	target_include_directories(fsmp_bullet PRIVATE ${FSMP_STANDALONE_DIR}/include-noppl)
endif()
if(NOT WIN32)
	target_include_directories(fsmp_bullet PRIVATE ${FSMP_STANDALONE_DIR}/include-nowin)
endif()
if(NOT MSVC)
	# The solver chooses its SSE4.1/FMA row solvers at runtime from the CPU features,
	# like MSVC builds do. Only the explicit intrinsics may use FMA instructions.
	set_source_files_properties(${FSMP_BULLET_DIR}/src/BulletDynamics/ConstraintSolver/btSequentialImpulseConstraintSolver.cpp
		PROPERTIES COMPILE_OPTIONS "-mfma;-ffp-contract=off")
endif()

# ---------------------------------------------------------------------------
# FSMP
# ---------------------------------------------------------------------------

set(FSMP_SOURCES
	${FSMP_ROOT}/src/hdtSkinnedMesh/hdtBoneScaleConstraint.cpp
	${FSMP_ROOT}/src/hdtSkinnedMesh/hdtCollider.cpp
	${FSMP_ROOT}/src/hdtSkinnedMesh/hdtCollisionAlgorithm.cpp
	${FSMP_ROOT}/src/hdtSkinnedMesh/hdtConeTwistConstraint.cpp
	${FSMP_ROOT}/src/hdtSkinnedMesh/hdtDispatcher.cpp
	${FSMP_ROOT}/src/hdtSkinnedMesh/hdtGeneric6DofConstraint.cpp
	${FSMP_ROOT}/src/hdtSkinnedMesh/hdtSkinnedMeshAlgorithm.cpp
	${FSMP_ROOT}/src/hdtSkinnedMesh/hdtSkinnedMeshBody.cpp
	${FSMP_ROOT}/src/hdtSkinnedMesh/hdtSkinnedMeshBone.cpp
	${FSMP_ROOT}/src/hdtSkinnedMesh/hdtSkinnedMeshShape.cpp
	${FSMP_ROOT}/src/hdtSkinnedMesh/hdtSkinnedMeshSystem.cpp
	${FSMP_ROOT}/src/hdtSkinnedMesh/hdtSkinnedMeshWorld.cpp
	${FSMP_ROOT}/src/hdtSkinnedMesh/hdtStiffSpringConstraint.cpp
	${FSMP_ROOT}/src/hdtSkinnedMesh/hdtVertex.cpp
	${FSMP_ROOT}/src/ActorManager.cpp
	${FSMP_ROOT}/src/config.cpp
	${FSMP_ROOT}/src/Events.cpp
	${FSMP_ROOT}/src/hdtConvertNi.cpp
	${FSMP_ROOT}/src/hdtDefaultBBP.cpp
	${FSMP_ROOT}/src/hdtForceUpdateList.cpp
	${FSMP_ROOT}/src/hdtSerialization.cpp
	${FSMP_ROOT}/src/hdtSkyrimBody.cpp
	${FSMP_ROOT}/src/hdtSkyrimBone.cpp
	${FSMP_ROOT}/src/hdtSkyrimPhysicsWorld.cpp
	${FSMP_ROOT}/src/hdtSkyrimSystem.cpp
	${FSMP_ROOT}/src/PluginInterfaceImpl.cpp
	${FSMP_ROOT}/src/WeatherManager.cpp
	${FSMP_ROOT}/src/XmlReader.cpp
	)

set(FSMP_STANDALONE_SOURCES
	${FSMP_STANDALONE_DIR}/FsmpScene.cpp
	${FSMP_STANDALONE_DIR}/StandIns.cpp
	)

# FSMP's Plugin.h is generated from its project version.
file(READ ${FSMP_ROOT}/CMakeLists.txt _fsmp_project)
string(REGEX MATCH "project\\([^)]*VERSION[ \t\r\n]+([0-9]+)\\.([0-9]+)\\.([0-9]+)" _fsmp_version "${_fsmp_project}")
if(NOT _fsmp_version)
	message(FATAL_ERROR "FSMP: project version not found in ${FSMP_ROOT}/CMakeLists.txt")
endif()
block(SCOPE_FOR VARIABLES)
	set(PROJECT_VERSION_MAJOR ${CMAKE_MATCH_1})
	set(PROJECT_VERSION_MINOR ${CMAKE_MATCH_2})
	set(PROJECT_VERSION_PATCH ${CMAKE_MATCH_3})
	set(PROJECT_NAME hdtsmp64)
	set(PROJECT_BUILD_INFO "")
	set(AVX_VARIANT AVX)
	configure_file(${FSMP_ROOT}/cmake/Plugin.h.in ${CMAKE_BINARY_DIR}/fsmp-generated/Plugin.h @ONLY)
endblock()

add_library(fsmp STATIC ${FSMP_SOURCES} ${FSMP_STANDALONE_SOURCES})
target_compile_features(fsmp PRIVATE cxx_std_23)
set_target_properties(fsmp PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(fsmp PRIVATE fsmp_bullet)
target_compile_definitions(fsmp PRIVATE ${FSMP_BULLET_DEFINITIONS})
target_compile_options(fsmp PRIVATE ${FSMP_SIMD_OPTIONS})
target_include_directories(fsmp PRIVATE
	${FSMP_STANDALONE_DIR}/include
	${FSMP_BULLET_DIR}/src
	${FSMP_ROOT}/src
	${CMAKE_BINARY_DIR}/fsmp-generated
	${CMAKE_CURRENT_LIST_DIR}/../../lib/nifly/include
	${CMAKE_CURRENT_LIST_DIR}/../../lib/nifly/external
	)
if(NOT MSVC)
	target_include_directories(fsmp PRIVATE ${FSMP_STANDALONE_DIR}/include-noppl)
endif()
if(NOT WIN32)
	target_include_directories(fsmp PRIVATE ${FSMP_STANDALONE_DIR}/include-nowin)
endif()
target_include_directories(fsmp INTERFACE ${FSMP_STANDALONE_DIR})

# FSMP's sources rely on its precompiled header being included first.
if(MSVC)
	target_compile_options(fsmp PRIVATE "/FI${FSMP_ROOT}/src/PCH.h" /permissive- /Zc:preprocessor /bigobj)
else()
	target_compile_options(fsmp PRIVATE "SHELL:-include ${FSMP_ROOT}/src/PCH.h")
endif()
