#include "test_helpers.hpp"
#include <sys/stat.h>
#include <fstream>
#include <string>

static bool file_exists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static std::string read_file(const std::string &path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

TEST(InitdbTest, CreatesExpectedFiles) {
    TempDataDir dir;
    ASSERT_FALSE(dir.path().empty());

    const char *share = get_pg_share_dir();
    ASSERT_NE(share, nullptr);

    embed_pg_options opts = {};
    opts.data_dir = dir.path().c_str();
    opts.pg_data_path = share;
    opts.superuser = "postgres";
    opts.encoding = "UTF8";

    ASSERT_EQ(embed_pg_initdb(&opts), 0);

    EXPECT_TRUE(file_exists(dir.path() + "/PG_VERSION"));
    EXPECT_TRUE(file_exists(dir.path() + "/postgresql.conf"));
    EXPECT_TRUE(file_exists(dir.path() + "/pg_hba.conf"));
}

TEST(InitdbTest, NullOptsFails) {
    EXPECT_NE(embed_pg_initdb(nullptr), 0);
}

TEST(InitdbTest, NullDatadirFails) {
    embed_pg_options opts = {};
    opts.pg_data_path = get_pg_share_dir();
    EXPECT_NE(embed_pg_initdb(&opts), 0);
}

TEST(InitdbTest, NullPgdataPathFails) {
    TempDataDir dir;
    embed_pg_options opts = {};
    opts.data_dir = dir.path().c_str();
    EXPECT_NE(embed_pg_initdb(&opts), 0);
}

TEST(InitdbTest, AppendsConfig) {
    TempDataDir dir;
    ASSERT_FALSE(dir.path().empty());

    const char *share = get_pg_share_dir();
    ASSERT_NE(share, nullptr);

    embed_pg_options opts = {};
    opts.data_dir = dir.path().c_str();
    opts.pg_data_path = share;
    opts.superuser = "postgres";
    opts.encoding = "UTF8";

    ASSERT_EQ(embed_pg_initdb(&opts), 0);

    std::string conf = read_file(dir.path() + "/postgresql.conf");
    EXPECT_FALSE(conf.empty());
    // embed_pg_initdb appends "fsync = off"
    EXPECT_NE(conf.find("fsync = off"), std::string::npos);
}

TEST(InitdbTest, FullLifecycleAfterInit) {
    TempDataDir dir;
    ASSERT_FALSE(dir.path().empty());

    const char *share = get_pg_share_dir();
    ASSERT_NE(share, nullptr);

    embed_pg_options opts = {};
    opts.data_dir = dir.path().c_str();
    opts.pg_data_path = share;
    opts.superuser = "postgres";
    opts.encoding = "UTF8";

    ASSERT_EQ(embed_pg_initdb(&opts), 0);

    embed_pg_server *srv = embed_pg_create();
    ASSERT_NE(srv, nullptr);
    ASSERT_EQ(embed_pg_start(srv, &opts), 0);
    ASSERT_EQ(embed_pg_wait_ready(srv, 30000), 0);

    PGconn *conn = embed_pg_connect(srv, "postgres", "postgres");
    ASSERT_NE(conn, nullptr);
    ASSERT_EQ(PQstatus(conn), CONNECTION_OK);

    PGresult *res = PQexec(conn, "SELECT 1");
    ASSERT_EQ(PQresultStatus(res), PGRES_TUPLES_OK);
    PQclear(res);
    PQfinish(conn);

    embed_pg_stop(srv);
    embed_pg_destroy(srv);
}
