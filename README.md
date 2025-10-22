<div align="center">

![OXXYEN MTProto Proxy Parser](https://i.pinimg.com/736x/21/d4/d8/21d4d8f44a1d2f0bf3a9e5569641ab05.jpg)

# 🌀 OXXYEN MTProto Proxy Parser

**High-performance, autonomous C parser for discovering and validating MTProto proxies**

</div>
An autonomous, high-performance C-based parser designed to discover, validate, and store MTProto proxy configurations from a wide variety of online sources including Telegram channels, GitHub repositories, and public proxy APIs.

![C](https://img.shields.io/badge/language-C-blue)
![License](https://img.shields.io/badge/license-MIT-green)
![Concurrency](https://img.shields.io/badge/concurrency-multithreaded-brightgreen)

## 📌 Features

- **Massive Source Coverage**: Parses over **100+ URLs** including Telegram public channels, GitHub raw files, and proxy APIs.
- **Robust Pattern Matching**: Uses **PCRE2 regex engine** with **40+ comprehensive patterns** to extract MTProto proxies in any known format.
- **Multi-threaded Architecture**: Supports up to **60 worker threads** with configurable concurrency (`CONCURRENT_DOWNLOADS`).
- **Smart Deduplication**: Uses **64-bit FNV-1a hashing** to avoid storing duplicate proxies.
- **Validation & Sanitization**: Validates IP/domain, port range (1–65535), and secret format; sanitizes malformed strings.
- **Graceful Shutdown**: Handles `SIGINT`/`SIGTERM` for safe termination.
- **Periodic Auto-Save**: Saves results every **10 seconds** (configurable) to:
  - `proxies.txt` – Simple `tg://proxy?...` list
  - `proxies_detailed.txt` – Full metadata (source, discovery time, hash, etc.)
  - `parser_stats.txt` – Runtime statistics
- **Real-time Logging & Stats**: Timestamped logs and periodic console statistics.
- **User-Agent Rotation**: Uses a pool of **35 realistic user agents** (desktop, mobile, tablet) to bypass basic blocking.

- ## 🔒 Anti-Detection & Protection Mechanisms

The parser employs multiple advanced techniques to avoid blocking, rate limiting, and fingerprinting by target servers:

### 1. **User-Agent Rotation**
- Uses a pool of **50+ realistic, up-to-date user agents** covering:
  - Desktop browsers (Chrome, Firefox, Edge, Safari)
  - Mobile devices (iOS, Android)
  - Tablets (iPad, Android tablets)
  - Niche browsers (Brave, Vivaldi, Yandex, Opera)
- Random UA is selected for **every request**, mimicking diverse real-world traffic.

### 2. **Request Throttling & Random Delays**
- Enforces **micro-delays** (`10–25 ms`) between concurrent requests.
- Adds **random sleep intervals** (`50–200 ms`) before each download task.
- Implements **10–12 second pauses** between full parsing cycles to avoid aggressive scraping patterns.

### 3. **Connection Hardening**
- Configures **low-speed timeouts** (`15s @ 1KB/s`) to drop stalled connections.
- Sets **connection timeout** (`10s`) and **total request timeout** (`30s`).
- Enables **TCP keep-alive** and **HTTP compression** (`gzip, deflate`) for natural traffic appearance.
- Allows **up to 5 HTTP redirects** to handle URL chains like real browsers.

### 4. **TLS/SSL Fingerprint Obfuscation**
- Disables strict certificate verification (`CURLOPT_SSL_VERIFYPEER = 0`) to bypass MITM inspection (common in proxy lists).
- Uses standard TLS stacks via `libcurl`, inheriting OS-level cipher suite diversity.

### 5. **Proxy Chain Support (Experimental)**
- Built-in infrastructure for **rotating through external proxy chains** (e.g., residential proxies).
- 1/3 of requests can optionally route through backup proxies (configurable).
- Tracks proxy success/failure rates for future optimization.

### 6. **Stealth Headers & Behavior**
- No custom or suspicious HTTP headers are added.
- Accepts standard encodings and mimics browser-like `Accept`/`Accept-Language` implicitly via User-Agent.
- Avoids JavaScript execution or DOM parsing — operates purely at the HTTP/text layer.

### 7. **Resilient Error Handling**
- Automatically retries failed requests (configurable up to 7 attempts).
- Gracefully handles network errors, timeouts, and malformed responses without crashing.
- Continues operation even if individual sources become unavailable.

These measures collectively ensure the parser operates **stealthily, reliably, and sustainably** across hundreds of heterogeneous sources while minimizing the risk of IP bans or CAPTCHA challenges.

## 🛠️ Requirements

- **Compiler**: GCC or Clang (C11 support required)
- **Libraries**:
  - `libcurl` (for HTTP requests)
  - `pcre2` (for regex parsing)
  - POSIX threads (`pthread`)
- **OS**: Linux (tested on Arch Linux), macOS, or any POSIX-compliant system

### Install Dependencies (Arch Linux)

```bash
sudo pacman -S gcc make curl pcre2
```

### Install Dependencies (Ubuntu/Debian)
```bash
sudo apt update
sudo apt install build-essential libcurl4-openssl-dev libpcre2-dev
```

### 🚀 Build & Run
1. Compile the program:
   ```bash
   gcc -O2 -std=c11 -Wall -lpthread -lcurl -lpcre2-8 mtproto_parser.c -o mtproto_parser
   ```
   > 💡 Note: The -lpcre2-8 flag assumes 8-bit PCRE2. Adjust if using 16/32-bit. 

2. Run:
```bash
./mtproto_parser
```

## 🛑 Stop gracefully

Press `Ctrl+C` — the parser will finish active tasks and save all data before exiting.

## 📁 Output Files

| File | Description |
|------|-------------|
| `proxies.txt` | Clean list of `tg://proxy?server=...&port=...&secret=...` URLs |
| `proxies_detailed.txt` | Full proxy records with source, hash, timestamps, and validation info |
| `parser_stats.txt` | Live statistics: uptime, total proxies, errors, cycles, etc. |

## ⚙️ Configuration (via Source)

All key parameters are defined at the top of `mtproto_parser.c`:

```c
#define PROXY_CAPACITY 1000000      // Max proxies to store
#define URL_CAPACITY 1000           // Max source URLs
#define MAX_THREAD_COUNT 60         // Max worker threads
#define CONCURRENT_DOWNLOADS 25     // Max parallel downloads
#define SAVE_INTERVAL 10            // Auto-save every N seconds
#define MAX_RETRY_ATTEMPTS 5        // Not yet used (reserved)
```

Edit these values before recompiling to tune performance for your system.

## 🔒 Safety & Ethics

- This tool **only reads public data**.
- It respects `robots.txt` implicitly by using standard HTTP clients and delays.
- No data is sent to third parties — everything runs **locally**.
- Use responsibly and in compliance with Telegram’s [Terms of Service](https://telegram.org/tos).

## 📜 License

MIT License. See [LICENSE](LICENSE) for details.

## 💬 Author

- **oxxyen** (`@oxxy3n` on Telegram)  
- Project: **OXXYEN STORAGE**  
- For support or collaboration, contact via Telegram: [@oxxy3n](https://t.me/oxxy3n)

> “Parse the world, one proxy at a time.” — OXXYEN AI
