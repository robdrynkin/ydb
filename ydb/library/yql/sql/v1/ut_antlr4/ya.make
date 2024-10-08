UNITTEST_FOR(ydb/library/yql/sql/v1)

SRCS(
    sql_ut_antlr4.cpp
    sql_match_recognize_ut.cpp
)

PEERDIR(
    library/cpp/regex/pcre
    ydb/library/yql/public/udf/service/exception_policy
    ydb/library/yql/core/sql_types
    ydb/library/yql/sql
    ydb/library/yql/sql/pg_dummy
    ydb/library/yql/sql/v1/format
)

TIMEOUT(300)

SIZE(MEDIUM)

END()
