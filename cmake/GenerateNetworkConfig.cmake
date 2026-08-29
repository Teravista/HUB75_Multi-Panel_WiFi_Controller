foreach(required_argument IN ITEMS ENV_FILE TEMPLATE_FILE OUTPUT_FILE)
    if(NOT DEFINED ${required_argument})
        message(FATAL_ERROR "Missing required argument: ${required_argument}")
    endif()
endforeach()

if(NOT EXISTS "${ENV_FILE}")
    message(FATAL_ERROR "Missing .env file. Copy .env.example to .env and fill in its values.")
endif()

file(READ "${ENV_FILE}" ENV_CONTENT)

function(read_env_value key output_variable)
    string(REGEX MATCH "(^|\n)[ \t]*${key}=([^\r\n]*)" env_match "${ENV_CONTENT}")
    if(NOT env_match)
        message(FATAL_ERROR "Missing ${key} in .env")
    endif()

    set(value "${CMAKE_MATCH_2}")
    if(value STREQUAL "")
        message(FATAL_ERROR "${key} must not be empty in .env")
    endif()

    set(${output_variable} "${value}" PARENT_SCOPE)
endfunction()

read_env_value("WIFI_SSID" WIFI_SSID)
read_env_value("WIFI_PASSWORD" WIFI_PASSWORD)
read_env_value("PICO_PORT" IMAGE_UPLOAD_PORT)
read_env_value("WIFI_GATEWAY_PROBE_PORT" WIFI_GATEWAY_PROBE_PORT)

foreach(port_name IN ITEMS IMAGE_UPLOAD_PORT WIFI_GATEWAY_PROBE_PORT)
    if(NOT "${${port_name}}" MATCHES "^[0-9]+$" OR
       ${${port_name}} LESS 1 OR ${${port_name}} GREATER 65535)
        message(FATAL_ERROR "${port_name} must be an integer from 1 to 65535")
    endif()
endforeach()

string(REPLACE "\\" "\\\\" WIFI_SSID_ESCAPED "${WIFI_SSID}")
string(REPLACE "\"" "\\\"" WIFI_SSID_ESCAPED "${WIFI_SSID_ESCAPED}")
string(REPLACE "\\" "\\\\" WIFI_PASSWORD_ESCAPED "${WIFI_PASSWORD}")
string(REPLACE "\"" "\\\"" WIFI_PASSWORD_ESCAPED "${WIFI_PASSWORD_ESCAPED}")

get_filename_component(output_directory "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
configure_file("${TEMPLATE_FILE}" "${OUTPUT_FILE}" @ONLY)