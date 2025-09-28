#Note: this file was made primarily to support msvc and its project file incompatibilities nightmare.
#Its also useful for various other IDEs like QtCreator etc.
#It uses system libraries, so it will have dependancy issues with public releases where those dependancies are distro/version-specific.
#Public builds are still built using the (overcomplicated) traditional (g)makefile.

CMAKE_MINIMUM_REQUIRED(VERSION 3.0...3.10)
cmake_policy(SET CMP0063 NEW)

PROJECT(FTEQuake)

IF(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
        MESSAGE(STATUS "Configuring for Windows Store / UWP toolchain")
        SET(FTE_PLATFORM_UWP TRUE)
ENDIF()

SET(FTE_ENABLE_ZIP_PACKAGING TRUE)
FIND_PROGRAM(ZIP_EXECUTABLE zip)
IF(FTE_PLATFORM_UWP OR NOT ZIP_EXECUTABLE)
        SET(FTE_ENABLE_ZIP_PACKAGING FALSE)
ENDIF()
IF(NOT FTE_ENABLE_ZIP_PACKAGING)
        MESSAGE(STATUS "Disabling zip-based packaging targets (zip tool unavailable or not supported)")
ENDIF()

SET(FTE_ENABLE_I18N_TARGET TRUE)
FIND_PROGRAM(XGETTEXT_EXECUTABLE xgettext)
IF(CMAKE_CROSSCOMPILING OR NOT XGETTEXT_EXECUTABLE)
        SET(FTE_ENABLE_I18N_TARGET FALSE)
ENDIF()

IF(FTE_PLATFORM_UWP)
        ADD_DEFINITIONS(-DWINRT)
        ADD_DEFINITIONS(-DHAVE_WEBSOCKCL)
        SET(FTE_DEFINES ${FTE_DEFINES};WINRT)
        SET(FTESV_DEFINES ${FTESV_DEFINES};WINRT)
        SET(FTE_LIB_DEFINES ${FTE_LIB_DEFINES};WINRT)
        SET(FTE_DEFINES ${FTE_DEFINES};HAVE_WEBSOCKCL)
        SET(FTESV_DEFINES ${FTESV_DEFINES};HAVE_WEBSOCKCL)
        SET(FTE_LIB_DEFINES ${FTE_LIB_DEFINES};HAVE_WEBSOCKCL)
        SET(FTE_ENGINE_SERVER_ONLY false CACHE BOOL "Compile fteqw-sv (server only) engine binary." FORCE)
        SET(FTE_TOOL_IQM false CACHE BOOL "Compile IQM Tool." FORCE)
        SET(FTE_TOOL_IMAGE false CACHE BOOL "Compile Image Tool." FORCE)
        SET(FTE_TOOL_MASTER false CACHE BOOL "Compile master server." FORCE)
        SET(FTE_TOOL_QTV false CACHE BOOL "Compile qtv server." FORCE)
        SET(FTE_TOOL_HTTPSV false CACHE BOOL "Compile small http server." FORCE)
        SET(FTE_TOOL_QCVM false CACHE BOOL "Compile standalone qcvm." FORCE)
        SET(FTE_TOOL_QCC false CACHE BOOL "Compile commandline qc compiler." FORCE)
        SET(FTE_TOOL_QCCGUI false CACHE BOOL "Compile gui qc compiler." FORCE)
        SET(FTE_MENU_SYS false CACHE BOOL "Compile System Menu." FORCE)
        SET(FTE_CSADDON false CACHE BOOL "CS Addon." FORCE)
ENDIF()

INCLUDE_DIRECTORIES(
	engine/common
	engine/client
	engine/qclib
	engine/gl
	engine/server
	engine
)

IF (EXISTS ${CMAKE_SOURCE_DIR}/.svn AND "${FTE_REVISON}" STREQUAL "")
	EXECUTE_PROCESS(COMMAND
		"svnversion"
		WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		OUTPUT_VARIABLE FTE_REVISON
		ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE
	)

	IF (NOT "${FTE_REVISON}" STREQUAL "")
		MESSAGE(STATUS "FTE SVN Revision ${FTE_REVISON}")
		IF(FTE_REVISON MATCHES "M")
			MESSAGE(STATUS "--- PRIVATE CHANGES DETECTED ---")
			SET(FTE_REVISON SVNREVISION=${FTE_REVISON})
		ELSE()
			MESSAGE(STATUS "No local changes")
ENDIF()

# libepoll-shim needs to be installed on the BSDs and Mac OSX to get 
# some of the server code to compile and work correctly on those platforms - Brad
IF(CMAKE_SYSTEM_NAME MATCHES "BSD" OR CMAKE_SYSTEM_NAME MATCHES "Darwin")
	INCLUDE(FetchContent)
	FetchContent_Declare(
		epoll-shim
		GIT_REPOSITORY https://github.com/jiixyj/epoll-shim.git
		GIT_TAG master
	)
	SET(epoll-shim BUILD_TESTS OFF CACHE INTERNAL "")
	SET(epoll-shim BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
	FetchContent_MakeAvailable(epoll-shim)

	SET(EPOLL_INC_DIR "${epoll-shim_SOURCE_DIR}/include")
ENDIF()

SET(FTE_BUILD_CONFIG ${PROJECT_SOURCE_DIR}/engine/common/config_fteqw.h CACHE FILEPATH "Which build config file to use to control supported features.")
SET(FTE_LIB_DEFINES ${FTE_LIB_DEFINES};CONFIG_FILE_NAME=${FTE_BUILD_CONFIG})
SET(FTE_USE_SDL false CACHE BOOL "Force the use of SDL2 instead of using native builds.")

INCLUDE(GNUInstallDirs)
SET(FTE_INSTALL_BINDIR games CACHE STRING "Binary dir to install to.")
SET(FTE_INSTALL_LIBDIR fteqw CACHE STRING "Binary dir to install to.")

IF(NOT WIN32)
        SET(SYS_LIBS ${SYS_LIBS} m)
ELSE()
        IF(FTE_PLATFORM_UWP)
                SET(SYS_LIBS ${SYS_LIBS} windowsapp bcrypt)
        ELSE()
                SET(SYS_LIBS ${SYS_LIBS})
        ENDIF()
ENDIF()

SET(FTE_DEP_ZLIB true CACHE BOOL "Link against zlib.")
IF(FTE_DEP_ZLIB)
	FIND_PACKAGE(ZLIB)
ENDIF()
IF(ZLIB_FOUND)
	SET(FTE_LIB_DEFINES ${FTE_LIB_DEFINES};ZLIB_STATIC)
	SET(FTE_LIBS ${FTE_LIBS} ${ZLIB_LIBRARIES})
	SET(FTESV_LIBS ${FTESV_LIBS} ${ZLIB_LIBRARIES})
	SET(FTEQCC_LIBS ${FTEQCC_LIBS} ${ZLIB_LIBRARIES})
	SET(FTEQTV_LIBS ${FTEQTV_LIBS} ${ZLIB_LIBRARIES})
ELSE()
	MESSAGE(WARNING "libz library NOT available. compressed pk3, ICE, Q2E, etc etc, yada yada, blah blah will not be available.")
	SET(FTE_LIB_DEFINES ${FTE_LIB_DEFINES};NO_ZLIB)
ENDIF()

SET(FTE_DEP_BZIP2 true CACHE BOOL "Link against libbzip2.")
IF(FTE_DEP_BZIP2)
	FIND_PACKAGE(BZip2)
ENDIF()
IF(BZIP2_FOUND)
	SET(FTE_LIB_DEFINES ${FTE_LIB_DEFINES};AVAIL_BZLIB;BZLIB_STATIC)
	SET(FTE_LIBS ${FTE_LIBS} bz2)
	SET(FTESV_LIBS ${FTESV_LIBS} bz2)
#	MESSAGE(STATUS "bzip2 library found. bz2-compressed pk3s will work for the price of extra bloat! yay!")
IF(CMAKE_C_COMPILER_ID MATCHES "GNU")
		set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall ${FTE_WERROR_} -Wno-pointer-sign -Wno-unknown-pragmas -Wno-format-zero-length -Wno-strict-aliasing -Wno-error=cpp")
	ELSE()
		set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 ${FTE_WERROR_}")
	ENDIF()
	IF (NOT FTE_USE_SDL)
		set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,--warn-common")
	ENDIF()
	#SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wc++-compat")	#lul, thousands of errors!
ENDIF()
IF(CMAKE_BUILD_TYPE MATCHES "Debug")
	IF(NOT ${WIN32})
		set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fstack-protector-strong")
		set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu89")
	ENDIF()
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_DEBUG")
ENDIF()
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_FILE_OFFSET_BITS=64")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DFTE_LIBRARY_PATH=${CMAKE_INSTALL_FULL_LIBDIR}/${FTE_INSTALL_LIBDIR} -DFTE_DATA_DIR=${CMAKE_INSTALL_FULL_DATAROOTDIR}")

FUNCTION(EMBED_PLUGIN_META PLUGNAME PLUGTITLE PLUGDESC)
	SET_TARGET_PROPERTIES(plug_${PLUGNAME} PROPERTIES OUTPUT_NAME "${PLUGNAME}")
	SET_TARGET_PROPERTIES(plug_${PLUGNAME} PROPERTIES PREFIX "fteplug_")
	SET_TARGET_PROPERTIES(plug_${PLUGNAME} PROPERTIES LINK_FLAGS "-Wl,--no-undefined")
	SET(INSTALLTARGS ${INSTALLTARGS} "plug_${PLUGNAME}" PARENT_SCOPE)
        #sadly we need to use a temp zip file, because otherwise zip insists on using zip64 extensions which breaks zip -A (as well as any attempts to read any files).
        IF(FTE_ENABLE_ZIP_PACKAGING)
        ADD_CUSTOM_COMMAND(
		TARGET plug_${PLUGNAME} POST_BUILD
		COMMAND /bin/echo -e "{\\n	package fteplug_${PLUGNAME}\\n	ver \"${SVNREVISION}\"\\n	category Plugins\\n	title \"${PLUGTITLE}\"\\n	gamedir \"\"\\n	desc \"${PLUGDESC}\"\\n}" | ${ZIP_EXECUTABLE} -q -9 -fz- $<TARGET_FILE:plug_${PLUGNAME}>.zip -
		COMMAND cmake -E cat $<TARGET_FILE:plug_${PLUGNAME}>.zip >> "$<TARGET_FILE:plug_${PLUGNAME}>"
		COMMAND ${ZIP_EXECUTABLE} -A "$<TARGET_FILE:plug_${PLUGNAME}>"
                COMMAND cmake -E rm $<TARGET_FILE:plug_${PLUGNAME}>.zip
                VERBATIM)
        ENDIF()
ENDFUNCTION()

SET(FTE_DEP_GNUTLS true CACHE BOOL "Link against gnutls")
IF(FTE_DEP_GNUTLS)
	FIND_PACKAGE(GnuTLS)
	IF(NOT GNUTLS_FOUND)
		MESSAGE(WARNING "gnutls library NOT available. HTTPS/DTLS will not be available.")
		SET(FTE_LIB_DEFINES ${FTE_LIB_DEFINES};NO_GNUTLS)
	ELSE()
		IF(WIN32)
			SET(GNUTLS_STATIC true CACHE BOOL "Link gnutls statically.") #usually as an .so though. :/
		ELSE()
			SET(GNUTLS_STATIC false CACHE BOOL "Link gnutls statically.") #usually as an .so though. :/
		ENDIF()
		IF(GNUTLS_STATIC)
			SET(FTE_LIB_DEFINES ${FTE_LIB_DEFINES};GNUTLS_STATIC)
			SET(FTE_LIBS ${FTE_LIBS} ${GNUTLS_LIBRARY})
			SET(FTESV_LIBS ${FTESV_LIBS} ${GNUTLS_LIBRARY})
		ENDIF()
	ENDIF()
ENDIF()
IF(WIN32)
	SET(FTE_DEP_WINSSPI true CACHE BOOL "Link against winsspi(schannel)")
	IF(NOT FTE_DEP_WINSSPI)
		SET(FTE_DEFINES ${FTE_DEFINES};NO_WINSSPI)
	ENDIF()
IF(FTE_DEP_SDL3)
ENDIF()
IF(SDL3_FOUND)	#SDL3
	MESSAGE(STATUS "SDL3 library found, woo, modern, for now.")

#	FIND_PACKAGE(PkgConfig REQUIRED)
#	PKG_SEARCH_MODULE(sdl3 REQUIRED sdl3)

	FIND_PACKAGE(SDL3 REQUIRED)

	INCLUDE_DIRECTORIES(${FREETYPE_INCLUDE_DIRS} ${SDL3_INCLUDE_DIRS})

	SET(FTE_DEFINES ${FTE_DEFINES};FTE_SDL3;MULTITHREAD)
	SET(FTE_LIBS ${FTE_LIBS} ${SYS_LIBS} ${CMAKE_DL_LIBS} ${SDL3_LIBRARIES})
	SET(FTE_ARCH_FILES
		engine/client/sys_sdl.c
		engine/client/snd_al.c
		engine/client/snd_sdl.c
		engine/client/in_sdl.c
		engine/client/cd_sdl.c
		engine/gl/gl_vidsdl.c
	)

	SET(FTESV_DEFINES ${FTESV_DEFINES};MULTITHREAD)
	SET(FTESV_LIBS ${FTESV_LIBS} ${SYS_LIBS} ${CMAKE_DL_LIBS} ${SDL3_LIBRARIES})

        IF(WIN32)
                IF(FTE_PLATFORM_UWP)
                        SET(FTE_LIBS ${FTE_LIBS} ${SYS_LIBS})
                ELSE()
                        SET(FTE_LIBS ${FTE_LIBS} wsock32 gdi32 ole32)
                ENDIF()
                SET(FTE_DEFINES ${FTE_DEFINES})
                SET(FTE_ARCH_FILES ${FTE_ARCH_FILES}
                        engine/common/net_ssl_winsspi.c
                        engine/common/fs_win32.c
                )
                SET(FTESV_ARCH_FILES ${FTESV_ARCH_FILES}
                        engine/common/net_ssl_winsspi.c
                        engine/common/fs_win32.c
                        engine/server/sv_sys_win.c
                )
                IF(NOT FTE_PLATFORM_UWP)
                        SET(FTE_ARCH_FILES ${FTE_ARCH_FILES}
                                engine/client/winquake.rc
                        )
                        SET(FTESV_ARCH_FILES ${FTESV_ARCH_FILES}
                                engine/client/winquake.rc
                        )
                ENDIF()
        ELSE()
                SET(FTE_ARCH_FILES ${FTE_ARCH_FILES}
                        engine/common/net_ssl_gnutls.c
                )
                SET(FTESV_ARCH_FILES ${FTESV_ARCH_FILES}
			engine/common/net_ssl_gnutls.c
			engine/common/sys_linux_threads.c
			engine/server/sv_sys_unix.c
		)
		SET(FTESV_LIBS ${FTESV_LIBS} pthread)
	ENDIF()
ELSEIF(${ANDROID})
#	FIND_PACKAGE(Freetype REQUIRED)

#	INCLUDE_DIRECTORIES(	${FREETYPE_INCLUDE_DIRS} )

	SET(FTE_DEFINES ${FTE_DEFINES};ANDROID;VKQUAKE;MULTITHREAD)
	SET(FTE_LIBS ${FTE_LIBS} android log EGL ${SYS_LIBS} ${CMAKE_DL_LIBS})
	SET(FTE_ARCH_FILES
		engine/client/sys_droid.c
		engine/common/sys_linux_threads.c
		engine/client/snd_droid.c
		engine/client/cd_null.c
		engine/gl/gl_viddroid.c
	)
ELSEIF(WIN32 AND NOT FTE_USE_SDL)
	INCLUDE_DIRECTORIES(engine/libs engine/libs/freetype2/include)
#	LINK_DIRECTORIES(engine/libs/mingw64-libs)
	
	#	engine/server/sv_sys_win.c
	
        SET(FTE_DEFINES ${FTE_DEFINES};D3D9QUAKE;D3D11QUAKE)
        IF(FTE_PLATFORM_UWP)
                SET(FTE_LIBS ${FTE_LIBS} ${SYS_LIBS} d3d11 dxgi)
                SET(FTE_ARCH_FILES
                        engine/common/sys_winrt_threads.cpp
                        engine/common/net_ssl_winsspi.c
                        engine/common/net_ssl_gnutls.c
                        engine/common/fs_win32.c
                )
                LIST(APPEND FTE_ARCH_FILES
                        engine/client/cd_win.c
                        engine/client/snd_xaudio.c
                        engine/client/sys_winrt.cpp
                )
        ELSE()
                SET(FTE_LIBS ${FTE_LIBS} ole32 gdi32 wsock32 winmm dxguid)
                SET(FTE_ARCH_FILES
                        engine/common/sys_win_threads.c
                        engine/common/net_ssl_winsspi.c
                        engine/common/net_ssl_gnutls.c
                        engine/common/fs_win32.c
                )
                LIST(APPEND FTE_ARCH_FILES
                        engine/client/cd_win.c
                        engine/client/in_win.c
                        engine/client/snd_al.c
                        engine/client/snd_directx.c
                        engine/client/snd_wasapi.c
                        engine/client/snd_win.c
                        engine/client/snd_xaudio.c
                        engine/client/sys_win.c
                )
        ENDIF()
        SET(FTE_ARCH_FILES ${FTE_ARCH_FILES}
                engine/gl/gl_vidnt.c

                engine/d3d/d3d_backend.c
                engine/d3d/d3d_image.c
                engine/d3d/d3d_shader.c
                engine/d3d/d3d11_backend.c
                engine/d3d/d3d11_image.c
                engine/d3d/d3d11_shader.c
                engine/d3d/d3d8_backend.c
                engine/d3d/d3d8_image.c
                engine/d3d/vid_d3d.c
                engine/d3d/vid_d3d11.c
                engine/d3d/vid_d3d8.c
        )

        IF(NOT FTE_PLATFORM_UWP)
                SET(FTE_ARCH_FILES ${FTE_ARCH_FILES}
                        engine/client/winquake.rc
                )
        ENDIF()

        IF(FTE_PLATFORM_UWP)
                SET(FTESV_LIBS ${FTESV_LIBS} ${SYS_LIBS})
        ELSE()
                SET(FTESV_LIBS ${FTESV_LIBS} wsock32 winmm)
        ENDIF()
        SET(FTESV_ARCH_FILES
                engine/common/sys_win_threads.c
                engine/common/net_ssl_winsspi.c
                engine/common/net_ssl_gnutls.c
                engine/common/fs_win32.c
                engine/server/sv_sys_win.c
        )
        IF(NOT FTE_PLATFORM_UWP)
                SET(FTESV_ARCH_FILES ${FTESV_ARCH_FILES}
                        engine/client/winquake.rc
                )
        ENDIF()
ELSEIF(UNIX AND NOT FTE_USE_SDL)	#linux(ish)
				#openbsd will have issues with snd_linux.c

	#linux-only packages
	FIND_PACKAGE(ALSA)
	IF(ALSA_FOUND)
		SET(FTE_LIB_DEFINES ${FTE_LIB_DEFINES};AUDIO_ALSA;AUDIO_PULSE)
	ELSE()
		MESSAGE(WARNING "asound (alsa) library NOT available.")
	ENDIF()

	FIND_PACKAGE(X11)
	IF(X11_FOUND)
		IF (NOT X11_Xcursor_FOUND)
			SET(FTE_LIB_DEFINES ${FTE_LIB_DEFINES};NO_X11_CURSOR)
			MESSAGE(WARNING "Xcursor library NOT available.")
		ENDIF()
		IF (NOT X11_Xrandr_FOUND)
			SET(FTE_LIB_DEFINES ${FTE_LIB_DEFINES};NO_X11_RANDR)
			MESSAGE(WARNING "Xrandr library NOT available.")
		ENDIF()
		IF (NOT X11_Xscreensaver_FOUND)
			SET(FTE_LIB_DEFINES ${FTE_LIB_DEFINES};NO_X11_XSS)
			MESSAGE(WARNING "Xss library NOT available.")
		ENDIF()
SET(FTE_QCVM_FILES
	engine/qclib/qccmain.c
	engine/qclib/qcd_main.c
	engine/qclib/qcdecomp.c
)
SET(FTE_COMMON_FILES
	#these files are common to both server-only and client+server builds.
	engine/common/cmd.c
	engine/common/com_mesh.c
	engine/common/com_bih.c
	engine/common/common.c
	engine/common/json.c
	engine/common/crc.c
	engine/common/cvar.c
	engine/common/fs.c
	engine/common/fs_dzip.c
	engine/common/fs_pak.c
	engine/common/fs_stdio.c
	engine/common/fs_xz.c
	engine/common/fs_zip.c
	engine/common/gl_q2bsp.c
	engine/common/huff.c
	engine/common/log.c
	engine/common/mathlib.c
	engine/common/md4.c
	engine/common/md5.c
        engine/common/net_chan.c
        engine/common/net_ice.c
	engine/common/plugin.c
	engine/common/pmove.c
	engine/common/pmovetst.c
	engine/common/pr_bgcmd.c
	engine/common/q1bsp.c
	engine/common/q2pmove.c
	engine/common/qvm.c
	engine/common/sha1.c
	engine/common/sha2.c
	engine/common/translate.c
	engine/common/zone.c

	#important headers
	engine/common/bothdefs.h
	engine/common/config_fteqw.h
	engine/common/config_minimal.h
	engine/common/config_nocompat.h
	engine/common/config_wastes.h
	engine/common/config_freecs.h
	engine/common/config_fteqw_noweb.h

	#useless headers that I'll never search for
	engine/client/api_menu.h
	engine/client/cdaudio.h
	engine/client/client.h
SET(FTE_COMMON_FILES
	engine/server/sv_sql.h
	#engine/sw/sw.h
	#engine/sw/sw_spans.h
	engine/vk/vkrenderer.h
	engine/web/ftejslib.h


	#sigh
	engine/client/pr_skelobj.c
	engine/client/m_download.c
	engine/client/net_master.c
	engine/client/r_d3.c
	
	#these are here because of hitmodel etc
	engine/gl/gl_heightmap.c
	engine/gl/gl_hlmdl.c
	engine/gl/gl_model.c
	
	engine/server/sv_move.c
	engine/server/sv_phys.c
	engine/server/world.c
	
	${FTE_QCVM_FILES}
	engine/qclib/hash.c
	
        engine/http/httpclient.c
)

IF(FTE_PLATFORM_UWP)
        LIST(APPEND FTE_COMMON_FILES engine/common/net_winrt.cpp)
ELSE()
        LIST(APPEND FTE_COMMON_FILES engine/common/net_wins.c)
ENDIF()

SET(FTE_SERVER_FILES
	engine/server/net_preparse.c
	engine/server/pr_cmds.c
	engine/server/pr_lua.c
	engine/server/pr_q1qvm.c
	engine/server/savegame.c
	engine/server/sv_ccmds.c
	engine/server/sv_chat.c
	engine/server/sv_cluster.c
	engine/server/sv_demo.c
	engine/server/sv_ents.c
	engine/server/sv_init.c
	engine/server/sv_main.c
	engine/server/sv_master.c
	engine/server/sv_mvd.c
	engine/server/sv_nchan.c
	engine/server/sv_rankin.c
	engine/server/sv_send.c
	engine/server/sv_sql.c
	engine/server/sv_user.c
#	engine/server/svhl_game.c
#	engine/server/svhl_phys.c
#	engine/server/svhl_world.c
	engine/server/svq2_ents.c
	engine/server/svq2_game.c
)
IF(FTE_PLUG_XMPP)
	)
	SET_TARGET_PROPERTIES(plug_xmpp PROPERTIES COMPILE_DEFINITIONS "FTEPLUGIN;${FTE_LIB_DEFINES}")

	#for SRV lookups, so we actually get the right server from account names/etc.
	IF(ANDROID)	#libresolv issues.
	ELSEIF(${WIN32}) #softlinks a dll
	ELSE()
		TARGET_LINK_LIBRARIES(plug_xmpp ${SYS_LIBS} resolv)
	ENDIF()

	EMBED_PLUGIN_META(xmpp "XMPP Plugin" "XMPP/Jabber instant messenger plugin for chatting without tabbing out.")
ENDIF()

INSTALL(TARGETS ${INSTALLTARGS}
	RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}/${FTE_INSTALL_BINDIR}"
	LIBRARY DESTINATION "${CMAKE_INSTALL_FULL_LIBDIR}/${FTE_INSTALL_LIBDIR}"
)

INSTALL(FILES
	fteqw.desktop
	DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications/")


SET(FTE_MENU_SYS true CACHE BOOL "Compile System Menu.")
IF(FTE_MENU_SYS)
	IF(FTE_ENABLE_ZIP_PACKAGING)
		ADD_CUSTOM_TARGET(menusys ALL
		VERBATIM
		DEPENDS fteqcc
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/quakec/menusys/"
		COMMAND fteqcc -srcfile "menu.src" -o "${CMAKE_CURRENT_BINARY_DIR}/menu.dat" -DREVISION="${SVNREVISION}" -DDATE="${FTE_DATE}" -DBRANCH="${FTE_BRANCH}"
		COMMAND /bin/echo -e "{\\n	package fte_menusys\\n	ver \"${SVNREVISION}\"\\n	category Plugins\\n	title \"Replacement Menus\"\\n	gamedir \"id1\"\\n	desc \"Modern menus to replace the ancient quake ones\"\\n}" | ${ZIP_EXECUTABLE} -j -q -9 -fz- "${CMAKE_CURRENT_BINARY_DIR}/menusys.pk3" - "${CMAKE_CURRENT_BINARY_DIR}/menu.dat"
		BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/menu.dat" "${CMAKE_CURRENT_BINARY_DIR}/menu.lno" "${CMAKE_CURRENT_BINARY_DIR}/menusys.pk3"
		SOURCES
			quakec/menusys/menu.src
			quakec/menusys/fteextensions.qc
			quakec/menusys/menusys/mitems.qc
			quakec/menusys/menusys/mitems_common.qc
			quakec/menusys/menusys/mitem_frame.qc
			quakec/menusys/menusys/mitem_desktop.qc
			quakec/menusys/menusys/mitem_exmenu.qc
			quakec/menusys/menusys/mitem_edittext.qc
			quakec/menusys/menusys/mitem_tabs.qc
			quakec/menusys/menusys/mitem_colours.qc
			quakec/menusys/menusys/mitem_checkbox.qc
			quakec/menusys/menusys/mitem_slider.qc
			quakec/menusys/menusys/mitem_combo.qc
			quakec/menusys/menusys/mitem_bind.qc
			quakec/menusys/menusys/mitem_spinnymodel.qc
			quakec/menusys/menu/loadsave.qc
			quakec/menusys/menu/newgame.qc
			quakec/menusys/menu/options_basic.qc
			quakec/menusys/menu/options_effects.qc
			quakec/menusys/menu/options_keys.qc
			quakec/menusys/menu/options.qc
			quakec/menusys/menu/presets.qc
			quakec/menusys/menu/servers.qc
			quakec/menusys/menu/main.qc
			quakec/menusys/menu/mods.qc
			quakec/menusys/menu/cvars.qc
			quakec/menusys/menu/updates.qc
			quakec/menusys/menu/options_audio.qc
			quakec/menusys/menu/options_configs.qc
			quakec/menusys/menu/options_hud.qc
			quakec/menusys/menu/options_particles.qc
			quakec/menusys/menu/options_video.qc
			quakec/menusys/menu/quit.qc
	)

		INSTALL(FILES
			${CMAKE_CURRENT_BINARY_DIR}/menusys.pk3
			DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/games/quake/id1/")
	ELSE()
		MESSAGE(STATUS "Skipping menusys target because zip packaging support is disabled.")
	ENDIF()
ENDIF()

SET(FTE_CSADDON true CACHE BOOL "CS Addon.")
IF(FTE_CSADDON)
	IF(FTE_ENABLE_ZIP_PACKAGING)
		ADD_CUSTOM_TARGET(csaddon ALL
		VERBATIM
		DEPENDS fteqcc
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/quakec/csaddon/src/"
		COMMAND fteqcc -srcfile "csaddon.src" -o "${CMAKE_CURRENT_BINARY_DIR}/csaddon.dat"
		COMMAND /bin/echo -e "{\\n	package fte_csaddon\\n	ver \"${SVNREVISION}\"\\n	category Plugins\\n	title \"${PLUGTITLE}\"\\n	gamedir \"id1\"\\n	desc \"${PLUGDESC}\"\\n}" | ${ZIP_EXECUTABLE} -j -q -9 -fz- "${CMAKE_CURRENT_BINARY_DIR}/csaddon.pk3" - "${CMAKE_CURRENT_BINARY_DIR}/csaddon.dat"
		BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/csaddon.dat" "${CMAKE_CURRENT_BINARY_DIR}/csaddon.lno" "${CMAKE_CURRENT_BINARY_DIR}/csaddon.pk3"
		SOURCES
			quakec/csaddon/src/csaddon.src

			quakec/csaddon/src/csplat.qc
			quakec/csaddon/src/csfixups.qc

			quakec/csaddon/src/editor_lights.qc
			quakec/csaddon/src/editor_terrain.qc
			quakec/csaddon/src/brush_selection.qc
			quakec/csaddon/src/brush_history.qc
			quakec/csaddon/src/brush_manip.qc
			quakec/csaddon/src/brush_draw.qc
			quakec/csaddon/src/brush_vertedit.qc
			quakec/csaddon/src/editor_brushes.qc
			quakec/csaddon/src/editor_ents.qc
			quakec/csaddon/src/textfield.qc
			quakec/csaddon/src/editor_particles.qc
			quakec/csaddon/src/menu.qc
			quakec/csaddon/src/cam.qc
			quakec/csaddon/src/csaddon.qc
	)

		INSTALL(FILES
			${CMAKE_CURRENT_BINARY_DIR}/csaddon.pk3
			DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/games/quake/id1/")
	ELSE()
		MESSAGE(STATUS "Skipping csaddon target because zip packaging support is disabled.")
	ENDIF()
ENDIF()