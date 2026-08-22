cmake_minimum_required(VERSION 3.25)

cmake_policy(SET CMP0091 NEW)

project(
    GW2MannyUploader
    VERSION 0.1.0
    DESCRIPTION "A streamlined arcdps log uploader for Raidcore Nexus"
    LANGUAGES C CXX
)

# Set the MSVC runtime before creating any targets, including bundled dependencies.
# Nexus addons are self-contained and must not mix the dynamic CRT with /MT or /MTd.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(MANNY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(MANNY_TWITCH_CLIENT_ID "" CACHE STRING "Public Twitch application client ID")
if(MANNY_TWITCH_CLIENT_ID AND NOT MANNY_TWITCH_CLIENT_ID MATCHES "^[a-z0-9]+$")
    message(FATAL_ERROR "MANNY_TWITCH_CLIENT_ID must contain only lowercase ASCII letters and digits")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(CTest)
include(Dependencies)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(manny_uploader_core STATIC)
target_sources(
    manny_uploader_core
    PRIVATE
        src/addon/addon_lifecycle.cpp
        src/application/application_pump.cpp
        src/application/configuration_service.cpp
        src/application/donbot_configuration_controller.cpp
        src/application/log_ingestion_coordinator.cpp
        src/application/nexus_options_controller.cpp
        src/application/recent_log_actions_controller.cpp
        src/application/twitch_authentication_controller.cpp
        src/application/twitch_message_template.cpp
        src/application/twitch_session.cpp
        src/application/twitch_session_owner.cpp
        src/application/upload_coordinator.cpp
        src/config/dpapi_secret_protector.cpp
        src/config/protected_file_secret_store.cpp
        src/config/settings.cpp
        src/config/settings_store.cpp
        src/domain/upload_job.cpp
        src/evtc/metadata_decoder.cpp
        src/evtc/metadata_parser_worker.cpp
        src/evtc/zevtc_archive.cpp
        src/filesystem/change_notifying_log_candidate_source.cpp
        src/filesystem/log_discovery.cpp
        src/filesystem/polling_log_candidate_source.cpp
        src/http/body_sources.cpp
        src/http/curl_http_client.cpp
        src/http/multipart_form_data.cpp
        src/ports/http_client.cpp
        src/ports/secret_store.cpp
        src/ports/twitch_authenticator.cpp
        src/project_info.cpp
        src/providers/async_upload_worker.cpp
        src/providers/dps_report_client.cpp
        src/providers/dps_report_provider_worker.cpp
        src/providers/donbot_client.cpp
        src/providers/donbot_provider_worker.cpp
        src/providers/donbot_verification_worker.cpp
        src/providers/twitch_client.cpp
        src/providers/twitch_chat_delivery.cpp
        src/providers/twitch_authentication_worker.cpp
        src/providers/twitch_provider_worker.cpp
        src/providers/twitch_test_message_worker.cpp
        src/providers/wingman_client.cpp
        src/providers/wingman_provider_worker.cpp
        src/support/atomic_file.cpp
        src/support/secret_value.cpp
        src/support/utf8.cpp
        src/ui/nexus_options_model.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/manny_uploader/addon/addon_lifecycle.hpp
            include/manny_uploader/application/application_pump.hpp
            include/manny_uploader/application/configuration_service.hpp
            include/manny_uploader/application/donbot_configuration_controller.hpp
            include/manny_uploader/application/log_ingestion_coordinator.hpp
            include/manny_uploader/application/nexus_options_controller.hpp
            include/manny_uploader/application/recent_log_actions_controller.hpp
            include/manny_uploader/application/twitch_authentication_controller.hpp
            include/manny_uploader/application/twitch_message_template.hpp
            include/manny_uploader/application/twitch_session.hpp
            include/manny_uploader/application/twitch_session_owner.hpp
            include/manny_uploader/application/upload_coordinator.hpp
            include/manny_uploader/config/protected_file_secret_store.hpp
            include/manny_uploader/config/secret_protector.hpp
            include/manny_uploader/config/settings.hpp
            include/manny_uploader/config/settings_store.hpp
            include/manny_uploader/domain/upload_job.hpp
            include/manny_uploader/evtc/metadata_decoder.hpp
            include/manny_uploader/evtc/metadata_parser_worker.hpp
            include/manny_uploader/evtc/zevtc_archive.hpp
            include/manny_uploader/filesystem/change_notifying_log_candidate_source.hpp
            include/manny_uploader/filesystem/log_discovery.hpp
            include/manny_uploader/filesystem/polling_log_candidate_source.hpp
            include/manny_uploader/http/body_sources.hpp
            include/manny_uploader/http/curl_http_client.hpp
            include/manny_uploader/http/multipart_form_data.hpp
            include/manny_uploader/ports/clock.hpp
            include/manny_uploader/ports/donbot_verifier.hpp
            include/manny_uploader/ports/external_action_launcher.hpp
            include/manny_uploader/ports/http_client.hpp
            include/manny_uploader/ports/log_candidate_source.hpp
            include/manny_uploader/ports/log_metadata_parser.hpp
            include/manny_uploader/ports/secret_store.hpp
            include/manny_uploader/ports/settings_store.hpp
            include/manny_uploader/ports/twitch_authenticator.hpp
            include/manny_uploader/ports/twitch_delivery_session.hpp
            include/manny_uploader/ports/twitch_test_messenger.hpp
            include/manny_uploader/ports/upload_provider.hpp
            include/manny_uploader/project_info.hpp
            include/manny_uploader/providers/async_upload_worker.hpp
            include/manny_uploader/providers/dps_report_client.hpp
            include/manny_uploader/providers/dps_report_provider_worker.hpp
            include/manny_uploader/providers/donbot_client.hpp
            include/manny_uploader/providers/donbot_provider_worker.hpp
            include/manny_uploader/providers/donbot_verification_worker.hpp
            include/manny_uploader/providers/twitch_client.hpp
            include/manny_uploader/providers/twitch_chat_delivery.hpp
            include/manny_uploader/providers/twitch_authentication_worker.hpp
            include/manny_uploader/providers/twitch_provider_worker.hpp
            include/manny_uploader/providers/twitch_test_message_worker.hpp
            include/manny_uploader/providers/wingman_client.hpp
            include/manny_uploader/providers/wingman_provider_worker.hpp
            include/manny_uploader/support/atomic_file.hpp
            include/manny_uploader/support/secret_value.hpp
            include/manny_uploader/support/utf8.hpp
            include/manny_uploader/ui/nexus_options_model.hpp
)
target_compile_features(manny_uploader_core PUBLIC cxx_std_23)
target_link_libraries(manny_uploader_core PRIVATE glaze::glaze manny_miniz)
if(WIN32)
    target_sources(
        manny_uploader_core
        PRIVATE src/filesystem/windows_directory_change_monitor.cpp
    )
    target_compile_definitions(manny_uploader_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
set_target_properties(manny_uploader_core PROPERTIES CXX_EXTENSIONS OFF)
manny_apply_compiler_warnings(manny_uploader_core)

if(WIN32)
    # CMake defines linker-PDB output properties for SHARED libraries, while Nexus still loads the
    # resulting DLL dynamically as a plugin. Non-Windows development builds remain MODULE targets.
    add_library(manny_uploader SHARED)
    target_sources(
        manny_uploader
        PRIVATE
            src/addon/nexus_entry.cpp
            src/addon/production_runtime.cpp
            src/addon/production_runtime.hpp
            src/addon/quick_access_icon.hpp
    )
else()
    add_library(manny_uploader MODULE)
    target_sources(manny_uploader PRIVATE src/addon.cpp)
endif()
target_link_libraries(manny_uploader PRIVATE manny_uploader_core)
target_compile_features(manny_uploader PRIVATE cxx_std_23)
set_target_properties(
    manny_uploader
    PROPERTIES
        CXX_EXTENSIONS OFF
        OUTPUT_NAME "manny_uploader"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
manny_apply_compiler_warnings(manny_uploader)

if(WIN32)
    target_link_libraries(manny_uploader_core PRIVATE crypt32 CURL::libcurl)
    target_link_libraries(manny_uploader PRIVATE manny_nexus_api manny_imgui shell32)
    target_compile_definitions(manny_uploader PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MANNY_TWITCH_CLIENT_ID)
        target_compile_definitions(
            manny_uploader
            PRIVATE MANNY_TWITCH_CLIENT_ID="${MANNY_TWITCH_CLIENT_ID}"
        )
    endif()
endif()

if(MINGW)
    target_link_options(manny_uploader PRIVATE -static -static-libgcc -static-libstdc++)
endif()

if(MSVC)
    # Keep private release symbols out of the install ZIP while retaining a separately publishable
    # linker PDB. /DEBUG changes the linker's optimization defaults, so preserve REF/ICF explicitly.
    target_compile_options(manny_uploader_core PRIVATE "$<$<CONFIG:Release>:/Z7>")
    target_compile_options(manny_uploader PRIVATE "$<$<CONFIG:Release>:/Z7>")
    target_link_options(
        manny_uploader
        PRIVATE
            "$<$<CONFIG:Release>:/DEBUG:FULL>"
            "$<$<CONFIG:Release>:/OPT:REF>"
            "$<$<CONFIG:Release>:/OPT:ICF>"
    )
    set_target_properties(
        manny_uploader
        PROPERTIES
            PDB_NAME_RELEASE "manny_uploader"
            PDB_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/symbols/Release"
    )
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(
    TARGETS manny_uploader
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .
)

if(WIN32)
    set(CPACK_GENERATOR ZIP)
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
    set(CPACK_MONOLITHIC_INSTALL ON)
    set(CPACK_PACKAGE_CHECKSUM SHA256)
    set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package")
    set(CPACK_PACKAGE_FILE_NAME "GW2-Manny-Uploader-${PROJECT_VERSION}-windows-x64")
    set(CPACK_PACKAGE_NAME "GW2 Manny Uploader")
    set(CPACK_PACKAGE_VENDOR "LoganWal")
    include(CPack)
endif()
