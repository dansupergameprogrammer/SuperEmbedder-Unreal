# U3 (te266 sec 4.2, "Shaders"): stage the engine's compiled GPU shaders beside
# the executables that load them. The engine reads <directory of the host
# executable>/shaders (U3 passes GpuContextConfig{}).
#
# semb_stage_gpu_shaders(<shader dir> <target>...) stages ONCE PER OUTPUT
# DIRECTORY (review S2): targets whose executables land in the same directory
# share one stamped custom command behind one custom target, and each such
# executable depends on it. A per-target POST_BUILD copy (the engine's own
# superslm_deploy_gpu_shaders) gives every executable in a shared directory its
# own copy_directory into the same shaders/ folder, and parallel links race
# there. The stamp reruns the copy only when a source shader changes, so a
# staged file a test renames away stays renamed until the shaders change.
#
# A target's directory is read at configure time: its RUNTIME_OUTPUT_DIRECTORY,
# else CMAKE_RUNTIME_OUTPUT_DIRECTORY, else its BINARY_DIR; a multi-config
# generator appends the configuration, as it does for the executable itself.
# The staging action names no target file, so it depends on no executable and
# no executable waits on another's link.
function(semb_stage_gpu_shaders shader_dir)
	get_property(_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
	file(GLOB _shaders CONFIGURE_DEPENDS "${shader_dir}/*")
	set(_dirs "")
	set(_n 0)
	foreach(_t IN LISTS ARGN)
		if(NOT TARGET ${_t})
			continue()
		endif()
		get_target_property(_dir ${_t} RUNTIME_OUTPUT_DIRECTORY)
		if(NOT _dir)
			if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
				set(_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
			else()
				get_target_property(_dir ${_t} BINARY_DIR)
			endif()
		endif()
		if(_multi AND NOT _dir MATCHES "\\$<")
			string(APPEND _dir "/$<CONFIG>")
		endif()
		list(FIND _dirs "${_dir}" _i)
		if(_i EQUAL -1)
			list(LENGTH _dirs _i)
			list(APPEND _dirs "${_dir}")
			if(_multi)
				set(_stamp "${CMAKE_BINARY_DIR}/semb_gpu_shaders_${_i}_$<CONFIG>.stamp")
			else()
				set(_stamp "${CMAKE_BINARY_DIR}/semb_gpu_shaders_${_i}.stamp")
			endif()
			add_custom_command(OUTPUT "${_stamp}"
				COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dir}/shaders"
				COMMAND "${CMAKE_COMMAND}" -E copy_directory "${shader_dir}" "${_dir}/shaders"
				COMMAND "${CMAKE_COMMAND}" -E touch "${_stamp}"
				DEPENDS ${_shaders}
				COMMENT "Staging SuperSLM GPU shaders in ${_dir}/shaders"
				VERBATIM)
			add_custom_target(semb_gpu_shaders_${_i} DEPENDS "${_stamp}")
		endif()
		add_dependencies(${_t} semb_gpu_shaders_${_i})
	endforeach()
endfunction()
