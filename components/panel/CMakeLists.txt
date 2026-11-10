idf_component_register(
    SRCS "src/Schema.cpp"
    INCLUDE_DIRS "include"
    REQUIRES sound
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
