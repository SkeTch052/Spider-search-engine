﻿#include "ini_parser.h"

Config load_config(const std::string& filename) {
    INIReader reader(filename);
    Config config;

    if (reader.ParseError() < 0) {
        throw std::runtime_error("Error loading configuration file: " + filename);
    }

    config.db_host = reader.Get("database", "host", "localhost");
    config.db_port = reader.GetInteger("database", "port", 5432);
    config.db_name = reader.Get("database", "dbname", "your_database");
    config.db_user = reader.Get("database", "user", "user");
    config.db_password = reader.Get("database", "password", "password");

    config.depth = reader.GetInteger("spider", "depth", 2);
    config.start_url = reader.Get("spider", "start_url", "https://en.wikipedia.org/wiki/Main_Page");

    config.search_port = reader.GetInteger("search", "port", 8081);

    return config;
}