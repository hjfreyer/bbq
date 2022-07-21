idf_component_register(SRCS "state.c" "main.c"
                       INCLUDE_DIRS ".")
target_add_binary_data(${COMPONENT_TARGET} "certs/private_key.pem" TEXT)
