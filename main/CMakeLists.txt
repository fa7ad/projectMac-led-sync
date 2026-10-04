idf_component_register(SRCS "main.c" "lamp.c"
                    INCLUDE_DIRS "."
                    REQUIRES bt nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer lwip)

# The panel ships gzipped and is served with Content-Encoding: gzip, so the
# browser inflates it -- no decompressor on the board. mtime=0 keeps the
# output reproducible.
idf_build_get_property(python PYTHON)
set(index_gz "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_custom_command(OUTPUT ${index_gz}
    COMMAND ${python} -c "import gzip, sys; open(sys.argv[2], 'wb').write(gzip.compress(open(sys.argv[1], 'rb').read(), 9, mtime=0))"
            ${CMAKE_CURRENT_SOURCE_DIR}/index.html ${index_gz}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/index.html
    VERBATIM)
add_custom_target(index_html_gz DEPENDS ${index_gz})
target_add_binary_data(${COMPONENT_LIB} ${index_gz} BINARY DEPENDS index_html_gz)
