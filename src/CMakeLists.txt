# Main component of the BTWifiModule firmware.
#
# Sources are listed explicitly (rather than GLOB) so that new additions to the
# tree are intentional and reproducible.

idf_component_register(
    SRCS
        main.c
        at_command.c
        bt.c
        bt_addr.c
        bt_client.c
        bt_server.c
        cb.c
        settings.c
        terminal.c
    INCLUDE_DIRS
        .
)
