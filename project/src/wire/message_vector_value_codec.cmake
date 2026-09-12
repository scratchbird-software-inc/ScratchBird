# Pure value codec shared by parser, engine metadata hashing and server bridge.
# Also available in focused builds that do not add the whole wire directory.
if(NOT TARGET sb_wire_message_vector_value_codec)
  add_library(sb_wire_message_vector_value_codec STATIC
    ${CMAKE_CURRENT_LIST_DIR}/message_vector_value_codec.cpp)
  target_compile_features(sb_wire_message_vector_value_codec PUBLIC cxx_std_20)
  target_include_directories(sb_wire_message_vector_value_codec PUBLIC
    ${CMAKE_CURRENT_LIST_DIR})
endif()
