
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

// Discard response body.
static std::size_t discard_body(
    char*,
    std::size_t size,
    std::size_t nmemb,
    void*
) {
    return size * nmemb;
}

// Stores one request's result.
struct RequestResult {
    int endpoint_index = 0;
    CURLcode curl_code = CURLE_OK;
    long http_status = 0;
    std::optional<double> latency_ms;
    double dns_lookup_ms = 0.0;
    double tcp_connect_ms = 0.0;
    double tls_handshake_ms = 0.0;
    double server_wait_ms = 0.0;

    bool succeeded() const {
        return curl_code == CURLE_OK &&
               http_status >= 200 &&
               http_status < 300;
    }
};

// Sends one HTTP request.
static RequestResult send_request(
    const std::string& url,
    const std::string& method,
    const std::string& body,
    const std::vector<std::string>& custom_headers,
    long connect_timeout_ms,
    long timeout_ms
) {
    RequestResult result;

    CURL* curl = curl_easy_init();

    if (curl == nullptr) {
        result.curl_code = CURLE_FAILED_INIT;
        return result;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // Configurable timeouts.
    curl_easy_setopt(
        curl,
        CURLOPT_CONNECTTIMEOUT_MS,
        connect_timeout_ms
    );

    curl_easy_setopt(
        curl,
        CURLOPT_TIMEOUT_MS,
        timeout_ms
    );

    struct curl_slist* headers = nullptr;

    // Add custom headers.
    for (const std::string& header : custom_headers) {
        curl_slist* updated_headers =
            curl_slist_append(headers, header.c_str());

        if (updated_headers == nullptr) {
            result.curl_code = CURLE_OUT_OF_MEMORY;
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return result;
        }

        headers = updated_headers;
    }

    // Add JSON content type unless already supplied.
    if (!body.empty()) {
        const bool has_content_type =
            std::any_of(
                custom_headers.begin(),
                custom_headers.end(),
                [](const std::string& header) {
                    const std::size_t colon = header.find(':');

                    if (colon == std::string::npos) {
                        return false;
                    }

                    std::string name = header.substr(0, colon);

                    std::transform(
                        name.begin(),
                        name.end(),
                        name.begin(),
                        [](unsigned char c) {
                            return static_cast<char>(std::tolower(c));
                        }
                    );

                    return name == "content-type";
                }
            );

        if (!has_content_type) {
            curl_slist* updated_headers =
                curl_slist_append(
                    headers,
                    "Content-Type: application/json"
                );

            if (updated_headers == nullptr) {
                result.curl_code = CURLE_OUT_OF_MEMORY;
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                return result;
            }

            headers = updated_headers;
        }
    }

    if (headers != nullptr) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    // Configure HTTP method.
    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
    } else if (method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    }

    // Send a body for POST, PUT, PATCH, and DELETE (including an empty body).
    if (method != "GET") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());

        curl_easy_setopt(
            curl,
            CURLOPT_POSTFIELDSIZE_LARGE,
            static_cast<curl_off_t>(body.size())
        );
    }

    const auto request_start =
        std::chrono::steady_clock::now();

    result.curl_code = curl_easy_perform(curl);

    const auto request_end =
        std::chrono::steady_clock::now();

    result.latency_ms =
        std::chrono::duration<double, std::milli>(
            request_end - request_start
        ).count();

    if (result.curl_code == CURLE_OK) {
        curl_easy_getinfo(
            curl,
            CURLINFO_RESPONSE_CODE,
            &result.http_status
        );
    }

    // Capture libcurl timing milestones (reported in seconds).
    double dns_seconds = 0.0;
    double connect_seconds = 0.0;
    double appconnect_seconds = 0.0;
    double starttransfer_seconds = 0.0;

    curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &dns_seconds);
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connect_seconds);
    curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &appconnect_seconds);
    curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &starttransfer_seconds);

    result.dns_lookup_ms = dns_seconds * 1000.0;
    result.tcp_connect_ms = std::max(0.0, connect_seconds - dns_seconds) * 1000.0;
    result.tls_handshake_ms = appconnect_seconds > 0.0
        ? std::max(0.0, appconnect_seconds - connect_seconds) * 1000.0
        : 0.0;

    const double ready_seconds = appconnect_seconds > 0.0
        ? appconnect_seconds
        : connect_seconds;
    result.server_wait_ms =
        std::max(0.0, starttransfer_seconds - ready_seconds) * 1000.0;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return result;
}

// Calculates percentile using nearest-rank method.
static double calculate_percentile(
    const std::vector<double>& sorted_values,
    double percentile
) {
    if (sorted_values.empty()) {
        return 0.0;
    }

    const std::size_t rank =
        static_cast<std::size_t>(
            std::ceil(percentile * sorted_values.size())
        );

    return sorted_values[rank - 1];
}

// Escapes a value so it is safe to write as one CSV field.
static std::string csv_escape(const std::string& value) {
    std::string escaped = "\"";

    for (char c : value) {
        if (c == '"') {
            escaped += "\"\"";
        } else {
            escaped += c;
        }
    }

    escaped += "\"";
    return escaped;
}

// Escapes text before inserting it into an HTML document.
static std::string html_escape(const std::string& value) {
    std::string escaped;

    for (char c : value) {
        switch (c) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&#39;"; break;
            default: escaped += c; break;
        }
    }

    return escaped;
}

// Reads and validates configuration.
static bool load_config(
    const std::string& filename,
    std::vector<std::string>& endpoints,
    std::string& method,
    int& total_requests,
    int& concurrency,
    std::string& body,
    std::vector<std::string>& headers,
    long& connect_timeout_ms,
    long& timeout_ms,
    int& ramp_up_seconds
) {
    try {
        std::ifstream file(filename);

        if (!file.is_open()) {
            throw std::runtime_error(
                "Could not open config file: " + filename
            );
        }

        json config;
        file >> config;

        if (!config.is_object()) {
            throw std::runtime_error(
                "Configuration must be a JSON object"
            );
        }

        if (!config.contains("endpoints") &&
            (!config.contains("url") || !config["url"].is_string())) {
            throw std::runtime_error(
                "Provide either 'url' or an 'endpoints' array of URL strings"
            );
        }

        if (
            !config.contains("requests") ||
            !config["requests"].is_number_integer()
        ) {
            throw std::runtime_error(
                "'requests' is required and must be an integer"
            );
        }

        endpoints.clear();
        if (config.contains("endpoints")) {
            if (!config["endpoints"].is_array() || config["endpoints"].empty()) {
                throw std::runtime_error("'endpoints' must be a non-empty array of URL strings");
            }
            for (const auto& endpoint : config["endpoints"]) {
                if (!endpoint.is_string() || endpoint.get<std::string>().empty()) {
                    throw std::runtime_error("Every item in 'endpoints' must be a non-empty URL string");
                }
                endpoints.push_back(endpoint.get<std::string>());
            }
        } else {
            endpoints.push_back(config["url"].get<std::string>());
        }

        total_requests = config["requests"].get<int>();

        if (total_requests < 1) {
            throw std::runtime_error(
                "'requests' must be at least 1"
            );
        }

        // Default method.
        method = "GET";

        if (config.contains("method")) {
            if (!config["method"].is_string()) {
                throw std::runtime_error(
                    "'method' must be a string"
                );
            }

            method = config["method"].get<std::string>();

            std::transform(
                method.begin(),
                method.end(),
                method.begin(),
                [](unsigned char c) {
                    return static_cast<char>(std::toupper(c));
                }
            );
        }

        if (
            method != "GET" &&
            method != "POST" &&
            method != "PUT" &&
            method != "PATCH" &&
            method != "DELETE"
        ) {
            throw std::runtime_error(
                "Supported methods: GET, POST, PUT, PATCH, DELETE"
            );
        }

        // Optional JSON body. GET requests do not accept a body.
        body.clear();

        if (config.contains("body") && !config["body"].is_null()) {
            if (method == "GET") {
                throw std::runtime_error(
                    "'body' is not supported for GET requests"
                );
            }

            body = config["body"].dump();
        }

        // Optional custom headers.
        headers.clear();

        if (config.contains("headers")) {
            if (!config["headers"].is_object()) {
                throw std::runtime_error(
                    "'headers' must be a JSON object"
                );
            }

            for (auto it = config["headers"].begin();
                 it != config["headers"].end();
                 ++it) {

                if (!it.value().is_string()) {
                    throw std::runtime_error(
                        "Header values must be strings"
                    );
                }

                headers.push_back(
                    it.key() + ": " +
                    it.value().get<std::string>()
                );
            }
        }

        // Configurable concurrency.
        concurrency = 10;

        if (config.contains("concurrency")) {
            // Accept both signed and unsigned JSON integers.
            if (!config["concurrency"].is_number_integer()) {
                throw std::runtime_error(
                    "'concurrency' must be a positive integer"
                );
            }

            concurrency = config["concurrency"].get<int>();
        }

        if (concurrency < 1) {
            throw std::runtime_error(
                "'concurrency' must be at least 1"
            );
        }

        // Optional gradual worker startup duration.
        ramp_up_seconds = 0;

        if (config.contains("ramp_up_seconds")) {
            if (!config["ramp_up_seconds"].is_number_integer()) {
                throw std::runtime_error(
                    "'ramp_up_seconds' must be a non-negative integer"
                );
            }

            ramp_up_seconds = config["ramp_up_seconds"].get<int>();

            if (ramp_up_seconds < 0) {
                throw std::runtime_error(
                    "'ramp_up_seconds' cannot be negative"
                );
            }
        }

        // Default timeouts.
        connect_timeout_ms = 5000;
        timeout_ms = 10000;

        if (config.contains("connect_timeout_ms")) {
            if (!config["connect_timeout_ms"].is_number_integer()) {
                throw std::runtime_error(
                    "'connect_timeout_ms' must be an integer"
                );
            }

            connect_timeout_ms =
                config["connect_timeout_ms"].get<long>();
        }

        if (config.contains("timeout_ms")) {
            if (!config["timeout_ms"].is_number_integer()) {
                throw std::runtime_error(
                    "'timeout_ms' must be an integer"
                );
            }

            timeout_ms = config["timeout_ms"].get<long>();
        }

        if (connect_timeout_ms <= 0 || timeout_ms <= 0) {
            throw std::runtime_error(
                "Timeout values must be greater than zero"
            );
        }

        return true;
    }
    catch (const std::exception& error) {
        std::cerr << "Configuration error: "
                  << error.what() << "\n";

        return false;
    }
}

int main(int argc, char* argv[]) {

    if (argc != 2) {
        std::cerr
            << "Usage: " << argv[0]
            << " <config.json>\n";

        return 1;
    }

    const std::string config_file = argv[1];

    std::vector<std::string> endpoints;
    std::string method;
    std::string body;

    int total_requests = 0;
    int concurrency = 10;

    long connect_timeout_ms = 5000;
    long timeout_ms = 10000;
    int ramp_up_seconds = 0;

    std::vector<std::string> headers;

    // Load configuration.
    if (!load_config(
        config_file,
        endpoints,
        method,
        total_requests,
        concurrency,
        body,
        headers,
        connect_timeout_ms,
        timeout_ms,
        ramp_up_seconds
    )) {
        return 1;
    }

    // Never create more workers than requests.
    const int worker_count =
        std::min(concurrency, total_requests);

    // Initialize libcurl.
    const CURLcode global_result =
        curl_global_init(CURL_GLOBAL_DEFAULT);

    if (global_result != CURLE_OK) {
        std::cerr << "Failed to initialize libcurl\n";
        return 1;
    }

    std::atomic<int> successes{0};
    std::atomic<int> failures{0};
    std::atomic<int> completed_requests{0};

    // Each worker claims the next available request.
    std::atomic<int> next_request{0};

    std::mutex breakdown_mutex;

    std::map<long, int> http_error_counts;
    std::map<CURLcode, int> curl_error_counts;

    // One result slot per request. Each worker writes to a unique index.
    std::vector<RequestResult> request_results(total_requests);

    // One latency slot per request.
    std::vector<std::optional<double>> latencies(total_requests);

    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    const auto start = std::chrono::steady_clock::now();

    // Worker function.
    auto worker = [&](int worker_index) {
        // Start workers gradually across the configured ramp-up period.
        if (ramp_up_seconds > 0 && worker_count > 1) {
            const auto delay = std::chrono::milliseconds(
                (static_cast<long long>(ramp_up_seconds) * 1000 *
                 worker_index) / (worker_count - 1)
            );
            std::this_thread::sleep_for(delay);
        }

        while (true) {

            // Atomically claim a request index.
            const int request_index =
                next_request.fetch_add(1);

            if (request_index >= total_requests) {
                break;
            }

            const int endpoint_index = request_index % static_cast<int>(endpoints.size());
            RequestResult result = send_request(
                endpoints[endpoint_index],
                method,
                body,
                headers,
                connect_timeout_ms,
                timeout_ms
            );

            result.endpoint_index = endpoint_index;

            // Each worker writes to its own request-result slot.
            request_results[request_index] = result;
            latencies[request_index] = result.latency_ms;

            if (result.succeeded()) {
                ++successes;
            } else {
                ++failures;

                std::lock_guard<std::mutex> lock(breakdown_mutex);

                if (result.curl_code != CURLE_OK) {
                    ++curl_error_counts[result.curl_code];
                } else {
                    ++http_error_counts[result.http_status];
                }
            }

            // Mark this request complete after updating its result counters.
            ++completed_requests;
        }
    };

    // Display live progress while the workers process requests.
    std::thread progress_thread([&]() {
        constexpr int bar_width = 30;

        while (true) {
            const int completed = completed_requests.load();
            const int percent =
                static_cast<int>(
                    (static_cast<double>(completed) / total_requests) * 100.0
                );

            const int filled =
                (percent * bar_width) / 100;

            std::cout << "\rLoadForge " << (ramp_up_seconds > 0 ? "ramping up [" : "running   [")
                      << std::string(filled, '#')
                      << std::string(bar_width - filled, '-')
                      << "] "
                      << std::setw(3) << percent << "% | "
                      << completed << "/" << total_requests
                      << " requests | Success: " << successes.load()
                      << " | Failed: " << failures.load()
                      << std::flush;

            if (completed >= total_requests) {
                break;
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(100)
            );
        }

        std::cout << "\rLoadForge complete  ["
                  << std::string(bar_width, '#')
                  << "] 100% | "
                  << total_requests << "/" << total_requests
                  << " requests | Success: " << successes.load()
                  << " | Failed: " << failures.load()
                  << "\n";
    });

    // Create a fixed number of workers.
    for (int i = 0; i < worker_count; ++i) {
        workers.emplace_back(worker, i);
    }

    // Wait for every worker to finish.
    for (std::thread& t : workers) {
        t.join();
    }

    progress_thread.join();

    const auto end = std::chrono::steady_clock::now();

    const std::chrono::duration<double> elapsed = end - start;

    // Export one row per request for later analysis in Excel or other tools.
    const std::string report_filename = "loadforge_report.csv";
    bool report_created = false;

    std::ofstream report(report_filename);

    if (report.is_open()) {
        report << "request_number,url,method,http_status,result,latency_ms,dns_lookup_ms,tcp_connect_ms,tls_handshake_ms,server_wait_ms,error\n";
        report << std::fixed << std::setprecision(2);

        for (int i = 0; i < total_requests; ++i) {
            const RequestResult& result = request_results[i];
            const bool succeeded = result.succeeded();

            std::string error;

            if (result.curl_code != CURLE_OK) {
                error = curl_easy_strerror(result.curl_code);
            } else if (!succeeded) {
                error = "HTTP " + std::to_string(result.http_status);
            }

            report << (i + 1) << "," << csv_escape(endpoints[result.endpoint_index])
                   << "," << method << ",";

            if (result.http_status != 0) {
                report << result.http_status;
            }

            report << "," << (succeeded ? "SUCCESS" : "FAILED") << ",";

            if (result.latency_ms.has_value()) {
                report << *result.latency_ms;
            }

            report << "," << result.dns_lookup_ms
                   << "," << result.tcp_connect_ms
                   << "," << result.tls_handshake_ms
                   << "," << result.server_wait_ms
                   << "," << csv_escape(error) << "\n";
        }

        report_created = report.good();
        report.close();
        report_created = report_created && !report.fail();
    }

    curl_global_cleanup();

    // Results summary.
    std::cout << "\n========== LoadForge Results ==========\n";

    std::cout << "Config file:    " << config_file << "\n";
    std::cout << "Endpoints:      " << endpoints.size() << "\n";
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << endpoints[i] << "\n";
    }
    std::cout << "HTTP method:    " << method << "\n";

    if (method != "GET") {
        std::cout << "Request body:   "
                  << (body.empty() ? "Empty" : "JSON supplied")
                  << "\n";
    }

    std::cout << "Total requests: " << total_requests << "\n";
    std::cout << "Concurrency:    " << concurrency << "\n";
    std::cout << "Active workers: " << worker_count << "\n";
    std::cout << "Ramp-up time:   " << ramp_up_seconds << " s\n";
    std::cout << "CSV report:     "
              << (report_created ? report_filename : "Could not create report")
              << "\n";
    std::cout << "Successes:      " << successes.load() << "\n";
    std::cout << "Failures:       " << failures.load() << "\n";
    std::cout << "Total duration: " << elapsed.count() << " s\n";

    std::cout << "Connect timeout:" << connect_timeout_ms << " ms\n";
    std::cout << "Request timeout:" << timeout_ms << " ms\n";

    std::cout << std::fixed << std::setprecision(2);

    // Requests per second.
    const int total_completed =
        successes.load() + failures.load();

    if (elapsed.count() > 0.0) {
        std::cout << "Requests/sec:   "
                  << total_completed / elapsed.count()
                  << "\n";
    }

    // Latency statistics.
    double latency_sum = 0.0;
    double latency_min = 0.0;
    double latency_max = 0.0;

    int latency_count = 0;

    std::vector<double> sorted_latencies;
    sorted_latencies.reserve(total_requests);

    for (const std::optional<double>& latency : latencies) {

        if (!latency.has_value()) {
            continue;
        }

        const double ms = *latency;

        if (latency_count == 0 || ms < latency_min) {
            latency_min = ms;
        }

        if (latency_count == 0 || ms > latency_max) {
            latency_max = ms;
        }

        latency_sum += ms;
        ++latency_count;

        sorted_latencies.push_back(ms);
    }

    if (latency_count > 0) {
        double dns_total = 0.0;
        double tcp_total = 0.0;
        double tls_total = 0.0;
        double server_wait_total = 0.0;

        for (const RequestResult& result : request_results) {
            if (!result.latency_ms.has_value()) {
                continue;
            }

            dns_total += result.dns_lookup_ms;
            tcp_total += result.tcp_connect_ms;
            tls_total += result.tls_handshake_ms;
            server_wait_total += result.server_wait_ms;
        }

        std::cout << "\nAverage response timing breakdown ("
                  << latency_count << " measured requests):\n";
        std::cout << "  DNS lookup:    " << dns_total / latency_count << " ms\n";
        std::cout << "  TCP connect:   " << tcp_total / latency_count << " ms\n";
        std::cout << "  TLS handshake: " << tls_total / latency_count << " ms\n";
        std::cout << "  Server wait:   " << server_wait_total / latency_count << " ms\n";

        std::cout << "\nLatency (requests measured: "
                  << latency_count << "):\n";

        std::cout << "  Average: "
                  << latency_sum / latency_count << " ms\n";

        std::cout << "  Minimum: "
                  << latency_min << " ms\n";

        std::cout << "  Maximum: "
                  << latency_max << " ms\n";

        std::sort(
            sorted_latencies.begin(),
            sorted_latencies.end()
        );

        std::cout << "\nLatency percentiles (nearest-rank):\n";

        std::cout << "  P50: "
                  << calculate_percentile(sorted_latencies, 0.50)
                  << " ms\n";

        std::cout << "  P90: "
                  << calculate_percentile(sorted_latencies, 0.90)
                  << " ms\n";

        std::cout << "  P95: "
                  << calculate_percentile(sorted_latencies, 0.95)
                  << " ms\n";

        std::cout << "  P99: "
                  << calculate_percentile(sorted_latencies, 0.99)
                  << " ms\n";
    }

    // Save this run and compare it with the previous run for the same endpoint list and method.
    const std::string history_filename = "loadforge_history.json";

    const double current_rps =
        elapsed.count() > 0.0
            ? total_completed / elapsed.count()
            : 0.0;

    const double current_average_latency =
        latency_count > 0
            ? latency_sum / latency_count
            : 0.0;

    const double current_p50 =
        latency_count > 0
            ? calculate_percentile(sorted_latencies, 0.50)
            : 0.0;

    const double current_p90 =
        latency_count > 0
            ? calculate_percentile(sorted_latencies, 0.90)
            : 0.0;

    const double current_p95 =
        latency_count > 0
            ? calculate_percentile(sorted_latencies, 0.95)
            : 0.0;

    const double current_p99 =
        latency_count > 0
            ? calculate_percentile(sorted_latencies, 0.99)
            : 0.0;

    const std::time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now()
    );

    std::tm local_time{};
#ifdef _WIN32
    localtime_s(&local_time, &now);
#else
    localtime_r(&now, &local_time);
#endif

    std::ostringstream timestamp_stream;
    timestamp_stream << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");

    json history = json::array();

    {
        std::ifstream history_file(history_filename);

        if (history_file.is_open()) {
            try {
                history_file >> history;

                if (!history.is_array()) {
                    history = json::array();
                }
            } catch (const std::exception&) {
                std::cerr
                    << "Warning: Existing history file is invalid. "
                    << "Starting a new history.\n";
                history = json::array();
            }
        }
    }

    // Find the most recent previous run for this same endpoint list and method.
    std::optional<json> previous_run;

    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (
            it->is_object() &&
            it->contains("endpoints") &&
            it->at("endpoints") == endpoints &&
            it->value("method", std::string{}) == method
        ) {
            previous_run = *it;
            break;
        }
    }

    json current_run = {
        {"timestamp", timestamp_stream.str()},
        {"endpoints", endpoints},
        {"method", method},
        {"requests", total_requests},
        {"concurrency", concurrency},
        {"ramp_up_seconds", ramp_up_seconds},
        {"successes", successes.load()},
        {"failures", failures.load()},
        {"duration_seconds", elapsed.count()},
        {"requests_per_second", current_rps},
        {"average_latency_ms", current_average_latency},
        {"p50_ms", current_p50},
        {"p90_ms", current_p90},
        {"p95_ms", current_p95},
        {"p99_ms", current_p99}
    };

    history.push_back(current_run);

    bool history_saved = false;

    {
        std::ofstream history_file(
            history_filename,
            std::ios::out | std::ios::trunc
        );

        if (history_file.is_open()) {
            history_file << std::setw(2) << history << "\n";
            history_saved = history_file.good();
        }
    }

    std::cout << "\nTest history:\n";
    std::cout << "  History file: "
              << (history_saved ? history_filename : "Could not save history")
              << "\n";
    std::cout << "  Saved runs:   " << history.size() << "\n";

    if (previous_run.has_value()) {
        const json& previous = *previous_run;

        const double previous_rps =
            previous.value("requests_per_second", 0.0);
        const double previous_average =
            previous.value("average_latency_ms", 0.0);
        const double previous_failures =
            previous.value("failures", 0.0);

        std::cout << "\nComparison with previous run:\n";
        std::cout << "  Previous run: "
                  << previous.value("timestamp", std::string{"unknown"})
                  << "\n";

        std::cout << std::fixed << std::setprecision(2);

        std::cout << "  Requests/sec: "
                  << previous_rps << " -> " << current_rps;

        if (previous_rps > 0.0) {
            const double change =
                ((current_rps - previous_rps) / previous_rps) * 100.0;

            std::cout << " (" << (change >= 0.0 ? "+" : "")
                      << change << "%)";
        }
        std::cout << "\n";

        std::cout << "  Avg latency:  "
                  << previous_average << " -> "
                  << current_average_latency << " ms";

        if (previous_average > 0.0) {
            const double change =
                ((current_average_latency - previous_average) /
                 previous_average) * 100.0;

            std::cout << " (" << (change >= 0.0 ? "+" : "")
                      << change << "%)";
        }
        std::cout << "\n";

        std::cout << "  Failures:     "
                  << previous_failures << " -> "
                  << failures.load() << "\n";
    } else {
        std::cout
            << "  No previous run for this endpoint list and method to compare yet.\n";
    }

    // Generate a self-contained HTML performance report.
    const std::string html_filename = "loadforge_report.html";
    bool html_created = false;

    const double success_rate =
        total_completed > 0
            ? (static_cast<double>(successes.load()) / total_completed) * 100.0
            : 0.0;

    const double failure_rate =
        total_completed > 0
            ? (static_cast<double>(failures.load()) / total_completed) * 100.0
            : 0.0;

    const double average_latency =
        latency_count > 0 ? latency_sum / latency_count : 0.0;

    const double p50 = latency_count > 0
        ? calculate_percentile(sorted_latencies, 0.50) : 0.0;
    const double p90 = latency_count > 0
        ? calculate_percentile(sorted_latencies, 0.90) : 0.0;
    const double p95 = latency_count > 0
        ? calculate_percentile(sorted_latencies, 0.95) : 0.0;
    const double p99 = latency_count > 0
        ? calculate_percentile(sorted_latencies, 0.99) : 0.0;

    double dns_average = 0.0;
    double tcp_average = 0.0;
    double tls_average = 0.0;
    double server_wait_average = 0.0;

    if (latency_count > 0) {
        for (const RequestResult& result : request_results) {
            if (!result.latency_ms.has_value()) {
                continue;
            }

            dns_average += result.dns_lookup_ms;
            tcp_average += result.tcp_connect_ms;
            tls_average += result.tls_handshake_ms;
            server_wait_average += result.server_wait_ms;
        }

        dns_average /= latency_count;
        tcp_average /= latency_count;
        tls_average /= latency_count;
        server_wait_average /= latency_count;
    }

    {
        std::ofstream html(html_filename, std::ios::out | std::ios::trunc);

        if (html.is_open()) {
            html << R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>LoadForge Performance Report</title>
<style>
:root{color-scheme:dark;--bg:#0b1020;--panel:#131b2e;--panel2:#19243a;--text:#edf2ff;--muted:#9aa8c1;--line:#293650;--green:#35d39a;--red:#ff6b78;--blue:#70a7ff;--purple:#b69cff}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.5 Segoe UI,Arial,sans-serif}
.wrap{max-width:1120px;margin:0 auto;padding:36px 22px 60px}.top{display:flex;justify-content:space-between;align-items:flex-start;gap:20px;margin-bottom:28px}
h1{font-size:28px;margin:0 0 5px}.sub{color:var(--muted);overflow-wrap:anywhere}.tag{border:1px solid var(--line);padding:7px 12px;border-radius:999px;color:var(--green);font-size:13px}
.grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:14px;margin:18px 0}
.card{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:18px;min-width:0}
.k{color:var(--muted);font-size:13px}.v{font-size:27px;font-weight:700;margin-top:5px;overflow-wrap:anywhere}.unit{font-size:13px;color:var(--muted);font-weight:400}
section{margin-top:22px}h2{font-size:17px;margin:0 0 14px}.two{display:grid;grid-template-columns:1fr 1fr;gap:14px}
.track{height:13px;background:#27334a;border-radius:99px;overflow:hidden;display:flex;margin:14px 0 8px}.good{background:var(--green)}.bad{background:var(--red)}
.legend{display:flex;gap:18px;color:var(--muted);font-size:13px}.dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:6px}
.metric{display:flex;justify-content:space-between;gap:12px;padding:10px 0;border-bottom:1px solid var(--line)}.metric:last-child{border:0}.metric span:first-child{color:var(--muted)}
table{width:100%;border-collapse:collapse}td,th{text-align:left;padding:11px 9px;border-bottom:1px solid var(--line)}th{color:var(--muted);font-size:12px;font-weight:600}
footer{margin-top:28px;color:var(--muted);font-size:12px;text-align:center}
@media(max-width:760px){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}.two{grid-template-columns:1fr}.top{display:block}.tag{display:inline-block;margin-top:12px}}
@media(max-width:420px){.wrap{padding:24px 14px}.v{font-size:22px}}
</style>
</head>
<body><main class="wrap">
)HTML";

            html << "<div class=\"top\"><div><h1>LoadForge Performance Report</h1>"
                 << "<div class=\"sub\">" << endpoints.size() << " endpoint(s) &middot; "
                 << html_escape(timestamp_stream.str())
                 << " &middot; " << html_escape(method) << "</div>"
                 << "<div class=\"sub\">";
            for (std::size_t i = 0; i < endpoints.size(); ++i) {
                if (i > 0) html << "<br>";
                html << html_escape(endpoints[i]);
            }
            html << "</div></div>"
                 << "<div class=\"tag\">TEST COMPLETE</div></div>";

            html << "<div class=\"grid\">"
                 << "<div class=\"card\"><div class=\"k\">Total requests</div><div class=\"v\">"
                 << total_requests << "</div></div>"
                 << "<div class=\"card\"><div class=\"k\">Success rate</div><div class=\"v\" style=\"color:var(--green)\">"
                 << std::fixed << std::setprecision(2) << success_rate
                 << "<span class=\"unit\">%</span></div></div>"
                 << "<div class=\"card\"><div class=\"k\">Requests / sec</div><div class=\"v\">"
                 << current_rps << "</div></div>"
                 << "<div class=\"card\"><div class=\"k\">Avg latency</div><div class=\"v\">"
                 << average_latency << "<span class=\"unit\"> ms</span></div></div>"
                 << "</div>";

            html << "<section class=\"two\"><div class=\"card\"><h2>Request outcome</h2>"
                 << "<div class=\"v\">" << successes.load() << " successful"
                 << " <span class=\"unit\">/ " << failures.load() << " failed</span></div>"
                 << "<div class=\"track\"><div class=\"good\" style=\"width:" << success_rate
                 << "%\"></div><div class=\"bad\" style=\"width:" << failure_rate
                 << "%\"></div></div><div class=\"legend\"><span><i class=\"dot\" style=\"background:var(--green)\"></i>Success "
                 << success_rate << "%</span><span><i class=\"dot\" style=\"background:var(--red)\"></i>Failure "
                 << failure_rate << "%</span></div></div>";

            html << "<div class=\"card\"><h2>Test configuration</h2>"
                 << "<div class=\"metric\"><span>Endpoints</span><strong>" << endpoints.size() << "</strong></div>"
                 << "<div class=\"metric\"><span>HTTP method</span><strong>" << html_escape(method) << "</strong></div>"
                 << "<div class=\"metric\"><span>Concurrency</span><strong>" << concurrency << "</strong></div>"
                 << "<div class=\"metric\"><span>Ramp-up time</span><strong>" << ramp_up_seconds << " s</strong></div>"
                 << "<div class=\"metric\"><span>Duration</span><strong>" << elapsed.count() << " s</strong></div>"
                 << "<div class=\"metric\"><span>Timeout</span><strong>" << timeout_ms << " ms</strong></div>"
                 << "</div></section>";

            html << "<section class=\"two\"><div class=\"card\"><h2>Latency percentiles</h2>"
                 << "<div class=\"metric\"><span>Minimum</span><strong>" << latency_min << " ms</strong></div>"
                 << "<div class=\"metric\"><span>Average</span><strong>" << average_latency << " ms</strong></div>"
                 << "<div class=\"metric\"><span>Maximum</span><strong>" << latency_max << " ms</strong></div>"
                 << "<div class=\"metric\"><span>P50</span><strong>" << p50 << " ms</strong></div>"
                 << "<div class=\"metric\"><span>P90</span><strong>" << p90 << " ms</strong></div>"
                 << "<div class=\"metric\"><span>P95</span><strong>" << p95 << " ms</strong></div>"
                 << "<div class=\"metric\"><span>P99</span><strong>" << p99 << " ms</strong></div>"
                 << "</div><div class=\"card\"><h2>Connection timing averages</h2>"
                 << "<div class=\"metric\"><span>DNS lookup</span><strong>" << dns_average << " ms</strong></div>"
                 << "<div class=\"metric\"><span>TCP connect</span><strong>" << tcp_average << " ms</strong></div>"
                 << "<div class=\"metric\"><span>TLS handshake</span><strong>" << tls_average << " ms</strong></div>"
                 << "<div class=\"metric\"><span>Server wait</span><strong>" << server_wait_average << " ms</strong></div>"
                 << "</div></section>";

            html << "<section class=\"card\"><h2>Request results</h2><table><thead><tr>"
                 << "<th>Request</th><th>Endpoint</th><th>HTTP status</th><th>Result</th><th>Latency</th>"
                 << "</tr></thead><tbody>";

            for (int i = 0; i < total_requests; ++i) {
                const RequestResult& result = request_results[i];
                const bool ok = result.succeeded();

                html << "<tr><td>" << (i + 1) << "</td><td>"
                     << html_escape(endpoints[result.endpoint_index]) << "</td><td>"
                     << (result.http_status == 0 ? std::string{"—"} : std::to_string(result.http_status))
                     << "</td><td style=\"color:" << (ok ? "var(--green)" : "var(--red)") << "\">"
                     << (ok ? "SUCCESS" : "FAILED") << "</td><td>";

                if (result.latency_ms.has_value()) {
                    html << *result.latency_ms << " ms";
                } else {
                    html << "—";
                }

                html << "</td></tr>";
            }

            html << "</tbody></table></section>"
                 << "<footer>Generated by LoadForge &middot; Self-contained report</footer>"
                 << "</main></body></html>";

            html_created = html.good();
            html.close();
            html_created = html_created && !html.fail();
        }
    }

    std::cout << "HTML report:    "
              << (html_created ? html_filename : "Could not create report")
              << "\n";

    // Failure breakdown.
    if (failures.load() > 0) {

        std::cout << "\nFailure breakdown:\n";

        if (!curl_error_counts.empty()) {

            std::cout
                << "  libcurl errors (no HTTP response received):\n";

            for (const auto& [code, count] : curl_error_counts) {

                std::cout
                    << "    " << curl_easy_strerror(code)
                    << " (code " << static_cast<int>(code)
                    << "): " << count << "\n";
            }
        }

        if (!http_error_counts.empty()) {

            std::cout
                << "  HTTP errors (server responded with non-2xx):\n";

            for (const auto& [status, count] : http_error_counts) {

                std::cout
                    << "    HTTP " << status
                    << ": " << count << "\n";
            }
        }
    }

    std::cout << "=======================================\n";

    return 0;
}
