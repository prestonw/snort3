set ( TIME_INTERNAL_SOURCES
    packet_time.cc
    packet_time.h
    periodic.cc
    periodic.h
    timersub.h
    tsc_clock.cc
    )

if ( ENABLE_UNIT_TESTS )
    list ( APPEND TIME_INTERNAL_SOURCES stopwatch_test.cc )
endif ( ENABLE_UNIT_TESTS )

set ( TIME_INCLUDES
    clock_defs.h
    stopwatch.h
    tsc_clock.h
    )

add_library ( time STATIC
    ${TIME_INTERNAL_SOURCES}
    ${TIME_INCLUDES}
    )

install (FILES ${TIME_INCLUDES}
    DESTINATION "${INCLUDE_INSTALL_PATH}/time"
)
