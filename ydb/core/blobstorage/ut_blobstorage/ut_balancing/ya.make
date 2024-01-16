UNITTEST_FOR(ydb/core/blobstorage/ut_blobstorage)
    SIZE(MEDIUM)

    TIMEOUT(600)

    SRCS(
        balancing_2.cpp
    )

    PEERDIR(
        ydb/core/blobstorage/ut_blobstorage/lib
    )

END()
