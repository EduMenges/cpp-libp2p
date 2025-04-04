#
# Copyright Quadrivium LLC
# All Rights Reserved
# SPDX-License-Identifier: Apache-2.0
#

if (NOT PACKAGE_MANAGER STREQUAL "hunter")
  function(hunter_add_package)
  endfunction()
endif ()

if (TESTING)
  # https://docs.hunter.sh/en/latest/packages/pkg/GTest.html
  hunter_add_package(GTest)
  find_package(GTest CONFIG REQUIRED)
endif()

if (PACKAGE_MANAGER STREQUAL "hunter")
  hunter_add_package(Boost COMPONENTS random filesystem program_options)
  find_package(Boost CONFIG REQUIRED filesystem random program_options)
else ()
  find_package(Boost CONFIG REQUIRED filesystem random beast program_options)
endif ()

# https://www.openssl.org/
hunter_add_package(BoringSSL)
find_package(OpenSSL CONFIG REQUIRED)

hunter_add_package(libsecp256k1)
find_package(libsecp256k1 CONFIG REQUIRED)

hunter_add_package(lsquic)
find_package(lsquic CONFIG REQUIRED)

# https://developers.google.com/protocol-buffers/
hunter_add_package(Protobuf)
find_package(Protobuf CONFIG REQUIRED)

hunter_add_package(qtils)
find_package(qtils CONFIG REQUIRED)

find_package(Threads)

hunter_add_package(c-ares)
find_package(c-ares CONFIG REQUIRED)

hunter_add_package(fmt)
find_package(fmt CONFIG REQUIRED)

hunter_add_package(yaml-cpp)
find_package(yaml-cpp CONFIG REQUIRED)

hunter_add_package(soralog)
find_package(soralog CONFIG REQUIRED)

# https://github.com/masterjedy/hat-trie
hunter_add_package(tsl_hat_trie)
find_package(tsl_hat_trie CONFIG REQUIRED)

# https://github.com/masterjedy/di
hunter_add_package(Boost.DI)
find_package(Boost.DI CONFIG REQUIRED)

if (SQLITE_ENABLED)
  # https://github.com/qdrvm/libp2p-sqlite-modern-cpp/tree/hunter
  hunter_add_package(SQLiteModernCpp)
  find_package(SQLiteModernCpp CONFIG REQUIRED)
endif ()

find_package(ZLIB REQUIRED)
