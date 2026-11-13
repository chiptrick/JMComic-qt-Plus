# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Release")
  file(REMOVE_RECURSE
  "CMakeFiles\\host_test_jm_api_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\host_test_jm_api_autogen.dir\\ParseCache.txt"
  "CMakeFiles\\host_test_jm_crypto_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\host_test_jm_crypto_autogen.dir\\ParseCache.txt"
  "CMakeFiles\\host_test_jm_webp_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\host_test_jm_webp_autogen.dir\\ParseCache.txt"
  "CMakeFiles\\host_test_net_client_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\host_test_net_client_autogen.dir\\ParseCache.txt"
  "host_test_jm_api_autogen"
  "host_test_jm_crypto_autogen"
  "host_test_jm_webp_autogen"
  "host_test_net_client_autogen"
  )
endif()
