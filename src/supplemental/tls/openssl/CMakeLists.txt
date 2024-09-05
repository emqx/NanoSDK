include(FindThreads)

if (NNG_TLS_ENGINE STREQUAL "open")
    set(NNG_TLS_OPENSSL ON)
    set(NNG_TLS_OPENSSL_HAVE_GM ON)
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
		nng_link_libraries_public(OpenSSL::SSL OpenSSL::Crypto)
    endif()

	#check_library_exists(wolfssl::wolfssl wolfSSL_CTX_LoadCRLBuffer "" NNG_WOLFSSL_HAVE_CRL)
    #check_library_exists(wolfssl::wolfssl wolfSSL_get_verify_result "" NNG_WOLFSSL_HAVE_VERIFY)
    #check_library_exists(wolfssl::wolfssl wolfSSL_CTX_set_default_passwd_cb "" NNG_WOLFSSL_HAVE_PASSWORD)
    #check_library_exists(wolfssl::wolfssl wolfSSL_get_peer_certificate "" NNG_WOLFSSL_HAVE_PEER_CERT)
    #check_library_exists(wolfssl::wolfssl wolfSSL_CTX_SetTmpDH "" NNG_WOLFSSL_HAVE_DH)
    #check_library_exists(wolfssl::wolfssl wolfSSL_CTX_set_psk_client_callback "" NNG_WOLFSSL_HAVE_PSK)

    #if (NNG_WOLFSSL_HAVE_DH)
    #    nng_defines(NNG_WOLFSSL_HAVE_DH)
    #else ()
    #    message(STATUS "wolfSSL configured without DH support.")
    #endif ()

    #if (NNG_WOLFSSL_HAVE_CRL)
    #    nng_defines(NNG_WOLFSSL_HAVE_CRL)
    #else ()
    #    message(STATUS "wolfSSL configured without CRL support.")
    #endif ()

    #if (NNG_WOLFSSL_HAVE_PASSWORD)
    #    nng_defines(NNG_WOLFSSL_HAVE_PASSWORD)
    #else ()
    #    message(STATUS "wolfSSL configured without password support.")
    #endif ()

    #if (NNG_WOLFSSL_HAVE_PEER_CERT)
    #    nng_defines(NNG_WOLFSSL_HAVE_PEER_CERT)
    #else ()
    #    message(STATUS "wolfSSL configured without peer cert chain support.")
    #endif ()

    #if (NNG_WOLFSSL_HAVE_PSK)
    #    nng_defines(NNG_SUPP_TLS_PSK)
    #else ()
    #    message(STATUS "wolfSSL configured without pre-shared key (PSK) support.")
    #endif()

    nng_defines(NNG_TLS_ENGINE_INIT=nng_tls_engine_init_open)
    nng_defines(NNG_TLS_ENGINE_FINI=nng_tls_engine_fini_open)
    nng_defines(NNG_SUPP_TLS)
	nng_defines(NNG_TLS_ENGINE_OPENSSL)
endif ()
