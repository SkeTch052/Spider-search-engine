﻿#include "http_utils.h"
#include "text_processor.h"
#include "../ini_parser.h"
#include "table_manager.h"
#include "db_buffer.h"
#include "extract_urls.h"
#include "parse_urls.h"

#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <Windows.h>
#include <functional>
#include <stdexcept>
#include <regex>
#include <chrono>
#include <pqxx/pqxx>
#include <myhtml/api.h>

#pragma execution_character_set(utf-8)


std::mutex mtx;                              // Мьютекс для очереди задач
std::condition_variable cv;                  // Условная переменная для уведомления потоков
std::queue<std::function<void()>> tasks;     // Очередь задач для пула потоков
bool exitThreadPool = false;                 // Флаг завершения пула потоков
bool tasksCompleted = false;                 // Флаг завершения всех задач

std::unordered_set<std::string> visitedUrls; // Множество посещённых URL
std::mutex visitedUrlsMutex;                 // Мьютекс для доступа к visitedUrls
const size_t MAX_URLS = 10000;               // Максимальное количество обрабатываемых URL

// Функция сохранения данных документа в буфер
void saveDocumentAndFrequency(const std::string& url, const std::string& clean, const std::map<std::string, int>& frequency, pqxx::connection& dbConn) {
    {
        std::lock_guard<std::mutex> lock(bufferMutex);
        buffer.push_back({ url, clean, frequency });
        if (buffer.size() < 10) return; // Ждём 10 записей
    }
    flushBuffer(dbConn); // Сбрасываем буфер в базу данных
}

void threadPoolWorker(pqxx::connection dbConn) {
    std::unique_lock<std::mutex> lock(mtx);
    while (!exitThreadPool || !tasks.empty()) {
        if (tasks.empty()) {
            cv.wait(lock);
        }
        else {
            auto task = tasks.front();
            tasks.pop();
            lock.unlock();
            task();
            lock.lock();
            
            if (tasks.empty() || visitedUrls.size() >= MAX_URLS) {
                tasksCompleted = true;
                cv.notify_all();
            }
        }
    }
    flushBuffer(dbConn); // Очищаем остатки буфера перед завершением
}

// Подготовка SQL-запросов для оптимизации работы с базой
void prepareStatements(pqxx::connection& c) {
    pqxx::work tx(c);
    tx.exec("PREPARE insert_word (text) AS INSERT INTO words (word) VALUES ($1) ON CONFLICT DO NOTHING;");
    tx.exec("PREPARE select_word_ids (text[]) AS SELECT id, word FROM words WHERE word = ANY($1);");
    tx.exec("PREPARE insert_frequency (int, int, int) AS INSERT INTO frequency (document_id, word_id, frequency) "
            "VALUES ($1, $2, $3) ON CONFLICT (document_id, word_id) DO UPDATE SET frequency = frequency.frequency + $3;");
    tx.commit();
}

// Проверка, является ли URL ссылкой на изображение
bool isImageUrl(const std::string& url) {
    const std::vector<std::string> imageExtensions = { ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp" };

    std::string lowerUrl = url;
    for (char& c : lowerUrl) {
        c = std::tolower(c);
    }

    for (const auto& ext : imageExtensions) {
        if (lowerUrl.length() >= ext.length() &&
            lowerUrl.substr(lowerUrl.length() - ext.length()) == ext) {
            return true;
        }
    }
    return false;
}

void parseLink(const Link& link, int depth, pqxx::connection& dbConn) {
    std::string baseUrl = (link.protocol == ProtocolType::HTTPS ? "https://" : "http://") + link.hostName;
    UrlComponents initialComponents = parseUrl(link.query, baseUrl);
    std::string fullUrl = (initialComponents.protocol == "https" ? "https://" : "http://") + initialComponents.host + initialComponents.query;

    {
        std::lock_guard<std::mutex> lock(visitedUrlsMutex);
        if (visitedUrls.count(fullUrl) > 0 || visitedUrls.size() >= MAX_URLS) {
            return; // Пропускаем уже посещённые URL или при превышении лимита
        }
        visitedUrls.insert(fullUrl);
    }

    try {
        std::string html = getHtmlContent({ link.protocol, initialComponents.host, initialComponents.query });
        if (html.empty()) {
            std::cout << "Failed to get HTML Content or content is empty" << std::endl;
            return;
        }

        // Извлекаем ссылки только если глубина позволяет
        if (depth > 0) {
            std::vector<std::string> urlStrings = extractUrls(html);

            std::vector<Link> links;
            for (const auto& url : urlStrings) {
                if (!url.empty() && url[0] != '#') {
                    UrlComponents components = parseUrl(url, fullUrl);
                    if (isImageUrl(components.query)) {
                        continue;
                    }
                    links.push_back(toLink(components));
                }
            }

            std::lock_guard<std::mutex> lock(mtx);
            for (auto& subLink : links) {
                tasks.push([subLink, depth, &dbConn]() { parseLink(subLink, depth - 1, dbConn); });
            }
            cv.notify_all();
        }

        std::string clean = cleanText(html);
        if (clean.empty()) {
            std::cout << "Cleaned text is empty, skipping processing" << std::endl;
            return;
        }

        std::map<std::string, int> frequency = calculateWordFrequency(clean);

        // Записываем в БД и выводим логи в консоль
        auto dbStart = std::chrono::high_resolution_clock::now();
        saveDocumentAndFrequency(fullUrl, clean, frequency, dbConn);
        auto dbEnd = std::chrono::high_resolution_clock::now();
        double dbTime = std::chrono::duration_cast<std::chrono::microseconds>(dbEnd - dbStart).count() / 1000.0;
        std::cout << "Processed URL: " << fullUrl << std::endl << "    dbSave duration: " << dbTime << " ms" << std::endl;                                           
    }
    catch (const std::exception& e) {
        std::cout << "Error in parseLink: " << e.what() << std::endl;
    }
}



int main() {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IOFBF, 1000);

    Config config;
    try {
        config = load_config("../../config.ini");
    }
    catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    // Формируем строку подключения к БД
    std::string connectionString = "host=" + config.db_host + 
                                   " port=" + std::to_string(config.db_port) +
                                   " dbname=" + config.db_name + 
                                   " user=" + config.db_user +
                                   " password=" + config.db_password;

    pqxx::connection mainDbConn(connectionString);
    if (!mainDbConn.is_open()) {
        std::cerr << "Failed to connect to database\n";
        return 1;
    }

    create_tables(mainDbConn);
    prepareStatements(mainDbConn);

    // Парсим начальный URL из конфигурации
    UrlComponents startComponents = parseUrl(config.start_url, "");
    Link startLink{
        (startComponents.protocol == "https" ? ProtocolType::HTTPS : ProtocolType::HTTP),
        startComponents.host,
        startComponents.query
    };

    try {
        int numThreads = std::thread::hardware_concurrency();
        std::vector<std::thread> threadPool;
        std::vector<pqxx::connection> threadConnections;

        // Запускаем пул потоков
        for (int i = 0; i < numThreads; ++i) {
          threadConnections.emplace_back(connectionString);
            if (!threadConnections.back().is_open()) {
                std::cerr << "Failed to connect thread " << i << " to database\n";
                return 1;
            }
            prepareStatements(threadConnections.back());
            threadPool.emplace_back(threadPoolWorker, std::move(threadConnections[i]));
        }

        {
            std::lock_guard<std::mutex> lock(mtx);
            tasks.push([startLink, &mainDbConn, depth = config.depth]() { parseLink(startLink, depth, mainDbConn); });
            cv.notify_one();
        }

        // Ожидаем завершения задач или достижения лимита
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, []() { return tasksCompleted || (tasks.empty() && visitedUrls.size() >= MAX_URLS); });
            exitThreadPool = true;
            cv.notify_all();
        }

        {
            std::lock_guard<std::mutex> lock(mtx);
            exitThreadPool = true;
            cv.notify_all();
        }

        // Завершаем потоки
        for (auto& t : threadPool) {
            t.join();
        }
        flushBuffer(mainDbConn); // Записываем остатки буфера

        std::cout << "Total URLs processed: " << visitedUrls.size() << std::endl;
    }
    catch (const std::exception& e) {
        std::cout << "Main error: " << e.what() << std::endl;
    }
    return 0;
}
