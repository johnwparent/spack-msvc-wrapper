/**
 * Copyright Spack Project Developers. See COPYRIGHT file for details.
 *
 * SPDX-License-Identifier: (Apache-2.0 OR MIT)
 */
#pragma once

#define MSVC_WRAPPER_MAJOR 0
#define MSVC_WRAPPER_MINOR 1
#define MSVC_WRAPPER_PATCH 0

#define MSVC_WRAP_STR_(x) #x
#define MSVC_WRAP_STR(x) MSVC_WRAP_STR_(x)
#define MSVC_WRAPPER_VERSION \
    MSVC_WRAP_STR(MSVC_WRAPPER_MAJOR) "." \
    MSVC_WRAP_STR(MSVC_WRAPPER_MINOR) "." \
    MSVC_WRAP_STR(MSVC_WRAPPER_PATCH)
