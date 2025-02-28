vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
vcpkg_from_github(
  OUT_SOURCE_PATH SOURCE_PATH
  REPO qdrvm/boost-di
  REF d5de6c9840c7fc2e44bf37134b4a14b88151ecc4
  SHA512 98e924d02bde23940ab59330730de6d4198d1e98cbb303ef713e3ee06f88e90707a952e6aa9a0372213b46d6c9d3478e86ed5cd64d74899861984e786e21c319
)
vcpkg_cmake_configure(
  SOURCE_PATH "${SOURCE_PATH}"
  OPTIONS
    -DBOOST_DI_OPT_BUILD_TESTS=OFF
    -DBOOST_DI_OPT_BUILD_EXAMPLES=OFF
)
vcpkg_cmake_install()
