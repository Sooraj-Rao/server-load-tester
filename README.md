# LoadForge

**A concurrent HTTP API load-testing tool built with C++20.**

LoadForge is a command-line tool for sending concurrent HTTP requests to
one or more API endpoints and measuring how they perform under load.

## Features

-   **Multiple endpoints** --- test several API URLs in a single run.
-   **Concurrent execution** --- configure the number of worker threads.
-   **Ramp-up** --- stagger worker startup across a configurable period.
-   **HTTP methods** --- supports `GET`, `POST`, `PUT`, `PATCH`, and
    `DELETE`.
-   **Custom requests** --- configure headers, JSON request bodies, and
    connection/request timeouts.
-   **Performance metrics** --- requests per second,
    average/minimum/maximum latency, and P50, P90, P95, and P99 latency
    percentiles.
-   **Connection timing breakdown** --- average DNS lookup, TCP
    connection, TLS handshake, and server-wait timings.
-   **Failure tracking** --- track successful requests, HTTP failures,
    and request errors.
-   **Reports** --- export per-request results to CSV and generate a
    self-contained HTML report.
-   **Test history** --- save run summaries to JSON and compare a run
    with the previous run for the same URL and HTTP method.

## Tech stack

-   C++20
-   libcurl
-   CMake
-   nlohmann/json
-   C++ threads

## Project structure

``` text
LoadForge/
├── src/
│   └── main.cpp
├── CMakeLists.txt
├── r.json
└── README.md
```

Generated build files and test reports are created locally and should
not be committed.

## Build

LoadForge requires a C++20 compiler, CMake, libcurl, and nlohmann/json.

### Windows (MSYS2 UCRT64)

Install the required packages from the MSYS2 UCRT64 terminal:

``` bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-make mingw-w64-ucrt-x86_64-curl mingw-w64-ucrt-x86_64-nlohmann-json
```

Make sure `C:\msys64\ucrt64\bin` is available on your PATH.

From the project directory, configure and build:

``` powershell
cmake -S . -B build
cmake --build build
```

Run the executable with a configuration file:

``` powershell
.\build\loadtester.exe r.json
```

> The executable target is currently named `loadtester`.

## Configuration

LoadForge reads its test settings from a JSON configuration file.

Example using multiple endpoints:

``` json
{
  "endpoints": [
    "https://httpbin.org/get?route=users",
    "https://httpbin.org/get?route=products",
    "https://httpbin.org/get?route=orders"
  ],
  "method": "GET",
  "requests": 12,
  "concurrency": 3,
  "ramp_up_seconds": 3,
  "connect_timeout_ms": 5000,
  "timeout_ms": 10000
}
```

### Configuration fields

  -----------------------------------------------------------------------
  Field                               Description
  ----------------------------------- -----------------------------------
  `endpoints`                         List of endpoint URLs to test.

  `method`                            HTTP method: `GET`, `POST`, `PUT`,
                                      `PATCH`, or `DELETE`.

  `requests`                          Total number of requests in the
                                      test.

  `concurrency`                       Maximum number of worker threads.

  `ramp_up_seconds`                   Time over which worker threads are
                                      staggered at startup.

  `connect_timeout_ms`                Maximum time allowed to establish a
                                      connection.

  `timeout_ms`                        Maximum total time allowed for a
                                      request.
  -----------------------------------------------------------------------

LoadForge also supports the earlier single-URL configuration using the
`url` field.

For POST, PUT, PATCH, and DELETE requests, JSON request bodies and
custom headers can be configured. If a JSON body is supplied without a
content type, LoadForge applies `application/json` by default.

## Reports and output

A test run generates:

-   `loadforge_report.csv` --- per-request results, including endpoint,
    HTTP status, outcome, latency, and error details.
-   `loadforge_report.html` --- a self-contained HTML performance
    report.
-   `loadforge_history.json` --- saved test-run summaries used for
    historical comparisons.

These files are generated in the working directory and may be updated by
later test runs.

## Example test target

The example configuration uses [httpbin](https://httpbin.org), a public
HTTP request and response testing service. Use conservative request
counts when testing public services, since they may apply rate limits.

Only run load tests against systems you own or have explicit permission
to test. Avoid sending high traffic to production services without
authorization.

## Current scope

LoadForge currently focuses on HTTP load generation, concurrent
execution, performance measurement, reporting, and run history.
Authentication profiles, response assertions, and live dashboards are
not included in the current documented feature set.

## License

No license has been added yet. Add a license before encouraging reuse or
redistribution.
