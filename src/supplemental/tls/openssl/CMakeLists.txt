include(FindThreads)

if (NNG_TLS_ENGINE STREQUAL "open")
    set(NNG_TLS_OPENSSL ON)
    set(NNG_TLS_OPENSSL_HAVE_GM ON)
    set(NNG_TLS_OPENSSL_HAVE_GM_NW ON)
    message("Enable TASSL !!")
    message(NOTICE "
        ************************************************************
        Linking against OpenSSL may change license terms.
        Consult a lawyer and the license files for details.
        ************************************************************")
    nng_sources(openssl.c)

    # If OpenSSL was added by a consuming project, then we should use that
    # instance of it, instead of configuring our own.
    if (TARGET openssl)
        nng_link_libraries(openssl)
    else()
        # We want to prefer config mode over our local find package.
        if (NOT (DEFINED CMAKE_FIND_PACKAGE_PREFER_CONFIG))
            set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
            find_package(OpenSSL REQUIRED)
            unset(CMAKE_FIND_PACKAGE_PREFER_CONFIG)
        else()
            find_package(OpenSSL REQUIRED)
        endif()
		if(NNG_TLS_OPENSSL_HAVE_GM_NW)
			message("==OPENSSL CONF WITH GM FOR NW: " ${NNG_TLS_OPENSSL_NW_CONF})
			nng_include_directories(${CMAKE_SOURCE_DIR}/extern/nw/include)
			nng_link_libraries_public(
				${CMAKE_SOURCE_DIR}/extern/nw/lib/libcn_x509_common.so
				${CMAKE_SOURCE_DIR}/extern/nw/lib/libcn_x509_crypto.so
				${CMAKE_SOURCE_DIR}/extern/nw/lib/libcn_x509_engine.so
				${CMAKE_SOURCE_DIR}/extern/nw/lib/libcn_x509_proxy.so
				${CMAKE_SOURCE_DIR}/extern/nw/lib/libssl.so
				${CMAKE_SOURCE_DIR}/extern/nw/lib/libcrypto.so)

			#nng_link_libraries_public(
			#	${CMAKE_SOURCE_DIR}/extern/nw/libx86/libcn_x509_proxy.so
			#	/root/TASSL/build/lib/libssl.so
			#	/root/TASSL/build/lib/libcrypto.so)
		else()
			nng_link_libraries_public(OpenSSL::SSL OpenSSL::Crypto)
		endif()
    endif()

    nng_defines(NNG_TLS_ENGINE_INIT=nng_tls_engine_init_open)
    nng_defines(NNG_TLS_ENGINE_FINI=nng_tls_engine_fini_open)
    nng_defines(NNG_SUPP_TLS)
    nng_defines(NNG_TLS_ENGINE_OPENSSL)
endif ()
