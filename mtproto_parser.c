/**
 * @file mtproto_parser.c
 * @author OXXYEN STORAGE
 * @brief The script allows you to autonomously parse all sources with MTPROTO proxy Telegram in the background,
 ******** as well as create .txt and .json files with these proxies, 
 ******** making it easy to read and understand the proxies.
 * @version 2.0
 * @date 2025-10-21
 * 
 * @copyright Copyright (c) 2025
 * 
 */

//** My name is Oxxyen, i'm developer C/C++, Python and Node.js. Thank you for deciding to take my script apart
//** I think there are still some things that need to be adjusted and more functionality added. 
//** If you want to help with the development, please create a branch and push your changes there. 
//** If you encounter any errors while using the script or have any questions, please contact me in Telegram @oxxy3n 
//** The script autonomously parses multiple added sources and extracts data from them in .json and .txt formats, after which it correctly writes them,
//** which can help you when creating a script that will take data from a file and make it readable. 
//** For security, I use User-Agent Rotation, Request Throttling & Random Delays, Connection Hardening, as described in detail in README.md 
//* Start: "gcc -o mtpro_parser mtproto_parser.c -lcurl -lpcre2-8-ljansson"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <time.h>
#include <ctype.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdatomic.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <jansson.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define PROXY_CAPACITY 1000000 //** Maximum number of unique proxies to store in memory
#define URL_CAPACITY 800 //** Maximum number of source URLs to parse
#define BUFFER_CAPACITY (100 * 1024 * 1024) //** Max download buffer size per request(100MB)
#define MAX_THREAD_COUNT 50 //** Maximum number of worker threads
#define CONCURRENT_DOWNLOADS 20 //** Max parallel download per batch
#define SAVE_INTERVAL 10 //** Auto-save result every N seconds
#define MAX_RETRY_ATTEMPTS 3 //** (Reselved requiest timeout retry logic)
#define CONNECTION_TIMEOUT 25 //** Total request timeout in seconds
#define USER_AGENT_POOL_SIZE 30 //** Number of User-Agent strings to rotate
#define MAX_PATTERNS 45 //** Number of regex patterns for proxy extraction
#define PROXY_BATCH_SIZE 5000 //** Max proxies to hold in temporary batch during parsing 
#define ROTATION_DELAY_MS 100 //** Max random delay(ms) before each request

//** =============== DATA STRUCTURES ===============
/**
 * @brief Represents a single validated MTProto proxy record.
 *        Includes metadata for tracking, deduplication, and export.
 */

typedef struct {
    char server[256];              //* Proxy hostname or IP address
    char port[16];                //* Port number as string(1-65535)
    char secret[512];            //* MTProto secret key (hex or base64)
    char connection_url[512];   //* Full tg:// URL for direct use in Telegram
    char source[256];          //* Original URL where this proxy was found
    char country[3];          //* ISO country code (currently defaults to "UN")
    char type[16];           //* "IPv4" or "Domain"
    uint64_t hash_value;    //* FNV-1a hash for fast deduplication
    time_t discovery_time; //* Timestamp when proxy was first found
    time_t last_verified; //* Last time proxy was confirmed valid
    atomic_int active;   //* Whether this proxy is currently usable
    atomic_int verified;//* Reserved for future active probing
    int speed_score;   //* Proxy perfomance rating (default: 50)
} ProxyRecord;

/**
 * @brief Dynamically resizable buffer for HTTP response content.
 */

typedef struct {
    char *data;          //* Pinter to allocated memory
    size_t size;        //* Current data size (bytes)
    size_t capacity;   //* Allocated buffer size
} DynamicBuffer; 

/**
 * @brief Task descriptor for a single download job.
 */
typedef struct {
    char *url;              //* URL fo fetch (dynamically allocated)
    int retry_count;       //* Number of retry attempts (not yes used)
    int priority;         //* Priority level (reselved for future)
    int use_proxy;       //* Whether to route this request through an external proxy (reserved)
} DownloadTask;
/**
 * @brief Global statistics tracker for monitoring parser performance.
 */
typedef struct {
    atomic_uint total_proxies;                  //* Total proxies stored (including duplicates before debup)
    atomic_uint processed_urls;                //* Successfully fetched URLs
    atomic_uint completed_cycles;             //*  Full Parsing cycles completed
    atomic_uint network_errors;              //*  Failed HTTP requests
    atomic_uint parse_errors;               //* Regex or parsing failures (not currently incremented)
    atomic_uint unique_proxies;            //* Count of truly unique proxies (after dedup)
    atomic_int active_workers;            //* Number of currently running worker threads
    atomic_uint total_requests;          //* Total HTTP requests attempted
    atomic_uint successful_proxies;     //* Proxies that passed validation
    atomic_uint total_bytes;           //* Total downloaded data (for bandwidth tracking)
    time_t initialization_time;       //* Start time of the parser
    atomic_uint last_cycle_proxies;  //* New proxies found in the most recent cycle
} SystemStatistics;

//* =============== GLOBAL STATE ===============


static atomic_int program_active = 1;  //* Flag to control main loop(set to 0 on shutdown)
static ProxyRecord *proxy_storage = NULL; //* Global array of discovered proxies
static pthread_mutex_t storage_mutex = PTHREAD_MUTEX_INITIALIZER;   //* Protectors proxy_storage   
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;     //* Protects file I/0
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;     //* Ensures clean console logs
static SystemStatistics stats = {0}; //* Zero-initialized global stats


//* =============== USER-AGENT POOL ===============
//* Rotating pool of realistic browser/device identifiers to avoid fingerprinting

static const char *USER_AGENTS[USER_AGENT_POOL_SIZE] = {
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36",
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:123.0) Gecko/20100101 Firefox/123.0",
    "Mozilla/5.0 (iPhone; CPU iPhone OS 17_4 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.4 Mobile/15E148 Safari/604.1",
    "Mozilla/5.0 (Linux; Android 14; SM-S928B) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Mobile Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36",
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:122.0) Gecko/20100101 Firefox/122.0",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:123.0) Gecko/20100101 Firefox/123.0",
    "Mozilla/5.0 (iPad; CPU OS 17_4 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.4 Mobile/15E148 Safari/604.1",
    "Mozilla/5.0 (Linux; Android 13; SM-S901B) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Mobile Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Edge/122.0.0.0 Safari/537.36",
    "Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:123.0) Gecko/20100101 Firefox/123.0",
    "Mozilla/5.0 (Linux; Android 12; Pixel 6) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Mobile Safari/537.36",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/119.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Linux; Android 11; SM-G991B) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Mobile Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/118.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_14_6) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36",
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Windows NT 6.1; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Linux; Android 10; SM-G973F) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Mobile Safari/537.36",
    "Mozilla/5.0 (iPhone; CPU iPhone OS 16_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/16.6 Mobile/15E148 Safari/604.1",
    "Mozilla/5.0 (Linux; Android 9; SM-G960F) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Mobile Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Edge/121.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
    "Mozilla/5.0 (X11; Linux x86_64; rv:122.0) Gecko/20100101 Firefox/122.0",
    "Mozilla/5.0 (Linux; Android 8.0.0; SM-G950F) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Mobile Safari/537.36"
};

//* =============== TARGET SOURCES ===============
//* List of public URLs known to contain MTProto proxy configurations

static const char *TARGET_URLS[URL_CAPACITY] = {
    "https://t.me/s/ProxyMTProto",
    "https://t.me/s/proxymtproto", 
    "https://t.me/s/proxymtprotoe",
    "https://t.me/s/mtprotoproxy",
    "https://t.me/s/mtproxy",
    "https://t.me/s/MTProxyu",
    "https://t.me/s/proxies_mtproto",
    "https://t.me/s/mtproxypro",
    "https://t.me/s/mtproxyz",
    "https://t.me/s/MTProxy_center",
    "https://t.me/s/proxy",
    "https://t.me/s/proxies",
    "https://t.me/s/goodproxies",
    "https://t.me/s/freeproxy",
    "https://t.me/s/mtproxy_socks5",
    "https://t.me/s/proxymaster",
    "https://t.me/s/proxyprovider",
    "https://t.me/s/proxyhub",
    "https://t.me/s/free_proxy_socks5",
    "https://t.me/s/proxystoree",
    "https://t.me/s/proxylist_mtproto",
    "https://t.me/s/mtproxylist",
    "https://t.me/s/proxymtprotolist",
    "https://t.me/s/freemtp",
    "https://t.me/s/mtproxyfree",
    
    "https://raw.githubusercontent.com/hookzof/socks5_list/master/tg/mtproto.json",
    "https://raw.githubusercontent.com/ALIILAPRO/Proxy/main/mtproto.json",
    "https://raw.githubusercontent.com/rosklyar/telegram-proxies/main/proxies.json",
    "https://raw.githubusercontent.com/jetkai/proxy-list/main/online-proxies/json/proxies-mtproto.json",
    "https://raw.githubusercontent.com/ShiftyTR/Proxy-List/master/mtproto.txt",
    "https://raw.githubusercontent.com/MuRongPIG/Proxy-Master/main/mtproto/mtproto.txt",
    "https://raw.githubusercontent.com/ProxyScraper/ProxyScraper/main/mtproto.txt",
    "https://raw.githubusercontent.com/saschazesiger/Free-Proxies/master/proxies/mtproto.txt",
    "https://raw.githubusercontent.com/elliottophellia/yakumo/master/results/mtproto/telegram/mtproto.txt",
    "https://raw.githubusercontent.com/rdavydov/proxy-list/main/proxies/mtproto.txt",
    "https://raw.githubusercontent.com/roma8ok/proxy-list/main/proxies/mtproto.txt",
    "https://raw.githubusercontent.com/roosterkid/openproxylist/main/mtproto.txt",
    "https://raw.githubusercontent.com/speedfighter/proxy-list/main/mtproto.txt",
    "https://raw.githubusercontent.com/t1m0n/proxy-list/main/mtproto.txt",
    "https://raw.githubusercontent.com/TheSpeedX/PROXY-List/master/mtproto.txt",
    "https://raw.githubusercontent.com/mertguvencli/http-proxy-list/main/proxy-list/data-with-geolocation.json",
    "https://raw.githubusercontent.com/Volodichev/proxy-list/main/mtproto.txt",
    "https://raw.githubusercontent.com/ProxyWorld/proxy-list/main/mtproto.txt",
    "https://raw.githubusercontent.com/aslisk/proxy-list/main/mtproto.txt",
    
    "https://mtpro.xyz/api/?type=mtproto",
    "https://mtpro.xyz/proxy-list",
    "https://api.proxyscrape.com/v3/free-proxy-list/get?request=displayproxies&proxy_format=protocol&format=json&protocol=mtproto",
    "https://www.proxy-list.download/api/v2/get?l=en&t=mtproto",
    "https://api.proxyscrape.com/v2/?request=getproxies&protocol=mtproto&timeout=10000&country=all",
    "https://api.proxyscrape.com/?request=displayproxies&proxytype=mtproto",
    "https://www.proxyscan.io/download?type=mtproto",
    "https://api.openproxylist.xyz/mtproto.txt",
    
    "https://proxyspace.pro/mtproto.txt",
    "https://openproxylist.xyz/mtproto.txt",
    "https://multiproxy.org/txt_all/proxy.txt",
    "https://spys.me/proxy.txt",
    "https://www.proxy-list.download/api/v1/get?type=mtproto",
    "https://www.proxyserverlist24.top/mtproto.txt",
    "https://proxylist.to/download/mtproto",
    "https://advanced.name/freeproxy/mtproto",
    NULL
};

//* =============== PARSING PATTERNS ===============
//* Comprehensive regex patterns to extract proxies from diverse formats:
//* - JSON, INI, plain text, inline, URL parameters, etc.

static const char *PARSE_PATTERNS[MAX_PATTERNS] = {
    //* Standard labeld format: "Server: ... Port: ... Secret: ..."
    "Server:[\\s\\r\\n]*([^\\r\\n]+?)[\\s\\r\\n]*Port:[\\s\\r\\n]*([0-9]{1,5})[\\s\\r\\n]*Secret:[\\s\\r\\n]*([0-9a-fA-F=]{16,512})",
    "server[\\s]*:[\\s]*([^\\r\\n]+?)[\\s]*port[\\s]*:[\\s]*([0-9]{1,5})[\\s]*secret[\\s]*:[\\s]*([0-9a-fA-F=]{16,512})",
    "Host:[\\s]*([^\\r\\n]+?)[\\s]*Port:[\\s]*([0-9]{1,5})[\\s]*Key:[\\s]*([0-9a-fA-F=]{16,512})",
    
    "\"server\"[\\s]*:[\\s]*\"([^\"]+?)\"[\\s]*,[\\s]*\"port\"[\\s]*:[\\s]*([0-9]+)[\\s]*,[\\s]*\"secret\"[\\s]*:[\\s]*\"([^\"]+?)\"",
    "\"host\"[\\s]*:[\\s]*\"([^\"]+?)\"[\\s]*,[\\s]*\"port\"[\\s]*:[\\s]*([0-9]+)[\\s]*,[\\s]*\"secret\"[\\s]*:[\\s]*\"([^\"]+?)\"",
    
    "tg://proxy\\?server=([^&]+?)&port=([0-9]+?)&secret=([^&\\s]+?)",
    "tg://socks\\?server=([^&]+?)&port=([0-9]+?)&secret=([^&\\s]+?)",
    
    "server=([^&\\s]+?)&port=([0-9]+?)&secret=([^&\\s]+?)",
    "host=([^&\\s]+?)&port=([0-9]+?)&key=([^&\\s]+?)",
    
    "([0-9a-zA-Z.-]+)[\\s\\-:]+([0-9]{1,5})[\\s\\-:]+([0-9a-fA-F\\s\\-=]{16,512})",
    "([0-9a-zA-Z._-]+):([0-9]{1,5}):([0-9a-fA-F=]{16,512})",
    
    "([0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3})[^0-9]*([0-9]{1,5})[^0-9a-fA-F]*([0-9a-fA-F\\s\\-=]{16,512})",
    "([0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}):([0-9]{1,5}):([0-9a-fA-F=]{16,512})",
    
    "([0-9a-fA-F]{32,512})[\\s@]+([^:\\s]+):([0-9]{1,5})",
    "([0-9a-fA-F=]+)[\\s@]+([^:\\s]+):([0-9]{1,5})",
    
    "address[\\s]*=[\\s]*([^\\r\\n]+?)[\\s]*port[\\s]*=[\\s]*([0-9]+)[\\s]*secret[\\s]*=[\\s]*([0-9a-fA-F=]+)",
    "Server[\\s]*=[\\s]*([^\\r\\n]+?)[\\s]*Port[\\s]*=[\\s]*([0-9]+)[\\s]*Secret[\\s]*=[\\s]*([0-9a-fA-F=]+)",
    
    "proxy[\\s]*:[\\s]*([^:]+):([0-9]+)[\\s]*key[\\s]*:[\\s]*([0-9a-fA-F]+)",
    "mtproto[\\s]*:[\\s]*([^:]+):([0-9]+)[\\s]*secret[\\s]*:[\\s]*([0-9a-fA-F]+)",
    
    "\"endpoint\"[\\s]*:[\\s]*\"([^:]+):([0-9]+)\"[\\s]*,[\\s]*\"secret\"[\\s]*:[\\s]*\"([^\"]+)\"",
    "([0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+)[\\s|\\-]+([0-9]+)[\\s|\\-]+([0-9a-fA-F]+)",
    "([a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}):([0-9]+):([0-9a-fA-F]{32,})",
    "([0-9a-fA-F]{32,})@([0-9a-zA-Z.-]+):([0-9]{1,5})",
    
    "([A-Za-z0-9+/=]{20,})[\\s@]+([^:\\s]+):([0-9]{1,5})",
    "([A-Za-z0-9_-]{20,})[\\s@]+([^:\\s]+):([0-9]{1,5})",
    
    "mtproxy://([^:]+):([0-9]+)\\?secret=([0-9a-fA-F]+)",
    "socks5://([^:]+):([0-9]+)\\?secret=([0-9a-fA-F]+)",
    "\\{\\s*\"s\"\\s*:\\s*\"([^\"]+)\"\\s*,\\s*\"p\"\\s*:\\s*([0-9]+)\\s*,\\s*\"k\"\\s*:\\s*\"([^\"]+)\"\\s*\\}",
    "\\[\\s*\"([^\"]+)\"\\s*,\\s*([0-9]+)\\s*,\\s*\"([^\"]+)\"\\s*\\]",
    
    "proxy_server[:=]\\s*([^\\s,]+)\\s*proxy_port[:=]\\s*([0-9]+)\\s*proxy_secret[:=]\\s*([^\\s,]+)",
    "\\|\\s*([^|]+)\\s*\\|\\s*([0-9]+)\\s*\\|\\s*([^|]+)\\s*\\|",
    "\\b([0-9a-fA-F]{64})\\b[^0-9a-fA-F]*([0-9a-zA-Z.-]+):([0-9]+)",
    
    "Server\\s*[=:]\\s*([^\\r\\n]+)[\\r\\n]+Port\\s*[=:]\\s*([0-9]+)[\\r\\n]+Secret\\s*[=:]\\s*([0-9a-fA-F=]+)",
    "Host\\s*[=:]\\s*([^\\r\\n]+)[\\r\\n]+Port\\s*[=:]\\s*([0-9]+)[\\r\\n]+Key\\s*[=:]\\s*([0-9a-fA-F=]+)",
    
    NULL //* Sentinel
};

//* =============== SIGNAL HANDLER ===============
//* Gracefully shuts down on Ctrl+C or kill signal

void handle_interrupt(int signal) {
    printf("\n[SYSTEM] Received signal %d. Performing graceful shutdown...\n", signal);
    atomic_store(&program_active, 0); //* Signal all thread to stop
}

//* =============== THREAD-SAFE LOGGING ===============
//* Prints timestamped messages without interleaving in multi-threaded context

void log_message(const char* format, ...) {
    pthread_mutex_lock(&log_mutex);
    
    time_t now = time(NULL);
    struct tm *timeinfo = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);
    
    printf("[%s] ", timestamp);
    
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    
    printf("\n");
    pthread_mutex_unlock(&log_mutex);
}

//* =============== SECURITY: USER-AGENT ROTATION ===============
//* Returns a random User-Agent from the pool to mimic real users

const char* get_random_user_agent() {
    return USER_AGENTS[rand() % USER_AGENT_POOL_SIZE];
}

//* =============== SECURITY: REQUEST THROTTLING ===============
//* Adds random micro-delays to avoid burst traffic patterns
void random_delay() {
    usleep((rand() % ROTATION_DELAY_MS + 50) * 1000);
}

//* =============== DEDUPLICATION: FAST HASHING ===============
//* Computes a 64-bit FNV-1a hash from server:port:secret for O(1) duplicate checks

uint64_t compute_hash(const char* server, const char* port, const char* secret) {
    uint64_t hash = 14695981039346656037ULL;
    //* Hash server
    for (const char* p = server; *p; p++) 
        hash = (hash ^ (uint64_t)(*p)) * 1099511628211ULL; //* FNV prime
    
    hash = (hash ^ (uint64_t)':') * 1099511628211ULL;
    //* Hash port
    for (const char* p = port; *p; p++) 
        hash = (hash ^ (uint64_t)(*p)) * 1099511628211ULL;
    
    hash = (hash ^ (uint64_t)':') * 1099511628211ULL;
    //* Hash first 64 chars of secret (enough for uniqueness)
    for (int i = 0; i < 64 && secret[i]; i++) 
        hash = (hash ^ (uint64_t)(secret[i])) * 1099511628211ULL;
    
    return hash;
}

//* =============== VALIDATION: PROXY SANITY CHECK ===============
//* Ensures server, port, and secret meet MTProto requirements

int validate_proxy(const char* server, const char* port, const char* secret) {
    if (!server || !port || !secret) 
        return 0;
    
    size_t server_len = strlen(server);
    size_t port_len = strlen(port);
    size_t secret_len = strlen(secret);
    
    if (server_len < 4 || server_len > 253) 
        return 0;
    
    if (port_len < 1 || port_len > 15)
        return 0;
    
    if (secret_len < 16 || secret_len > 511)
        return 0;
    //* Validate port is number and in range
    char* endptr;
    long port_value = strtol(port, &endptr, 10);
    if (endptr == port || *endptr != '\0' || port_value < 1 || port_value > 65535) 
        return 0;
    //* Validate secret contains only hex chars and optional padding (=)
    int valid_chars = 0;
    int hex_chars = 0;
    for (size_t i = 0; i < secret_len && i < 128; i++) {
        unsigned char c = secret[i];
        if ((c >= '0' && c <= '9') || 
            (c >= 'a' && c <= 'f') || 
            (c >= 'A' && c <= 'F')) {
            valid_chars++;
            hex_chars++;
        } else if (c == '=') {
            valid_chars++;
        } else if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            return 0; //* Ivanlud character
        }
    }
    
    return (valid_chars >= 16) && (hex_chars >= 8);
}


//* =============== SANITIZATION: CLEAN EXTRACTED STRINGS ===============
//*          Removes control chars, normalizes whitespace, trims ends

void sanitize_string(char *str) {
    if (!str) return;
    
    char *read_ptr = str;
    char *write_ptr = str;
    int space_flag = 0;
    
    while (*read_ptr) {
        if ((unsigned char)*read_ptr >= 0x20 && (unsigned char)*read_ptr < 0x7F) {
            if (*read_ptr == ' ' || *read_ptr == '\t' || *read_ptr == '\n' || *read_ptr == '\r') {
                if (!space_flag && write_ptr > str) {
                    *write_ptr++ = ' ';
                    space_flag = 1;
                }
            } else {
                *write_ptr++ = *read_ptr;
                space_flag = 0;
            }
        }
        read_ptr++;
    }
    *write_ptr = '\0';
    //* Trim trainig spaces
    while (write_ptr > str && (*(write_ptr-1) == ' ' || *(write_ptr-1) == '\t')) {
        *(--write_ptr) = '\0';
    }
}

//* =============== HTTP: CURL WRITE CALLBACK ===============
//*          Appends downloaded data to a dynamic buffer
size_t write_callback(void *content, size_t element_size, size_t element_count, void *user_buffer) {
    size_t total_size = element_size * element_count;
    DynamicBuffer *buffer = (DynamicBuffer *)user_buffer;

    if (!buffer || !atomic_load(&program_active)) 
        return 0;
    //* Grow buffer is needed (capped at BUFFER_CAPACIRY)
    if (buffer->size + total_size + 1 > buffer->capacity) {
        size_t new_capacity = buffer->capacity * 2;
        if (new_capacity < buffer->size + total_size + 1) {
            new_capacity = buffer->size + total_size + 1;
        }
        if (new_capacity > BUFFER_CAPACITY) {
            new_capacity = BUFFER_CAPACITY;
        }
        
        char *new_data = realloc(buffer->data, new_capacity);
        if (!new_data) {
            return 0;
        }
        
        buffer->data = new_data;
        buffer->capacity = new_capacity;
    }

    if (buffer->data) {
        memcpy(buffer->data + buffer->size, content, total_size);
        buffer->size += total_size;
        buffer->data[buffer->size] = '\0';
    }

    return total_size;
}

//* =============== HTTP: CURL HANDLE SETUP ===============
//* Configures libcurl with security and stealth options

CURL* setup_curl_handle() {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    
    curl_easy_setopt(curl, CURLOPT_USERAGENT, get_random_user_agent());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, CONNECTION_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);         //* Follow redirects
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);            
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);       //* Disable cert verification (common )
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);    //* Fast connect timeout
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);//* About slow transefts
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate"); //* Save bandwith
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);              //* Maintan connection
    
    return curl;
}

//* =============== CORE: PROXY EXTRACTION ENGINE ===============
//* Applies all regex patterns to content and validates discovered proxies

void extract_proxies_from_content(const char *content, size_t content_length, const char* source) {
    if (!content || content_length == 0 || !atomic_load(&program_active)) 
        return;

    log_message("Parsing content from %s (%zu bytes)", source, content_length);
    //* Allocate temporary batch storage   
    ProxyRecord *discovered_proxies = malloc(PROXY_BATCH_SIZE * sizeof(ProxyRecord));
    if (!discovered_proxies) {
        log_message("Memory allocation failed for proxy batch");
        return;
    }
    
    int discovery_count = 0;
    int total_discovered = 0;
    //* Try every regex pattern
    for (int pattern_index = 0; PARSE_PATTERNS[pattern_index] != NULL && atomic_load(&program_active); pattern_index++) {
        const char *pattern = PARSE_PATTERNS[pattern_index];
        pcre2_code *compiled_pattern = NULL;
        pcre2_match_data *match_data = NULL;
        
        compiled_pattern = pcre2_compile(
            (PCRE2_SPTR8)pattern,
            PCRE2_ZERO_TERMINATED,
            PCRE2_MULTILINE | PCRE2_DOTALL | PCRE2_CASELESS, // * Flexiblae matching
            NULL, NULL, NULL
        );
        
        if (!compiled_pattern) {
            continue;
        }

        match_data = pcre2_match_data_create_from_pattern(compiled_pattern, NULL);
        if (!match_data) {
            pcre2_code_free(compiled_pattern);
            continue;
        }

        PCRE2_SIZE current_offset = 0;
        int pattern_matches = 0;
        //* Find all matches in content
        while (current_offset < content_length && discovery_count < PROXY_BATCH_SIZE && atomic_load(&program_active)) {
            int match_result = pcre2_match(
                compiled_pattern, 
                (PCRE2_SPTR8)content, 
                content_length, 
                current_offset, 0, 
                match_data, NULL
            );
            
            if (match_result < 4) break; //* Need at least 3 capture groups

            PCRE2_SIZE *match_vector = pcre2_get_ovector_pointer(match_data);
            
            int valid_match = 1;
            for (int i = 2; i <= 7; i += 2) {
                if (match_vector[i] == PCRE2_UNSET || match_vector[i+1] == PCRE2_UNSET) {
                    valid_match = 0;
                    break;
                }
            }
            //* Extract matchet substrings
            if (valid_match) {
                size_t server_length = match_vector[3] - match_vector[2];
                size_t port_length = match_vector[5] - match_vector[4];
                size_t secret_length = match_vector[7] - match_vector[6];
                
                if (server_length > 0 && server_length < 256 && 
                    port_length > 0 && port_length < 16 && 
                    secret_length >= 16 && secret_length < 512) {
                    
                    ProxyRecord new_proxy = {0};
                    
                    strncpy(new_proxy.server, content + match_vector[2], server_length);
                    strncpy(new_proxy.port, content + match_vector[4], port_length);
                    strncpy(new_proxy.secret, content + match_vector[6], secret_length);
                    
                    new_proxy.server[server_length] = '\0';
                    new_proxy.port[port_length] = '\0';
                    new_proxy.secret[secret_length] = '\0';
                    //* Clean extracted fields
                    sanitize_string(new_proxy.server);
                    sanitize_string(new_proxy.port);
                    sanitize_string(new_proxy.secret);
                  //* Remove accidental label prefixes (e.g., "Server: 1.2.3.4" → "1.2.3.4")                  
                    const char* field_prefixes[] = {"Server:", "server:", "SERVER:", "Host:", "host:", "HOST:"};
                    for (int i = 0; i < 6; i++) {
                        size_t prefix_len = strlen(field_prefixes[i]);
                        if (strncasecmp(new_proxy.server, field_prefixes[i], prefix_len) == 0) {
                            memmove(new_proxy.server, new_proxy.server + prefix_len, strlen(new_proxy.server) - prefix_len + 1);
                            sanitize_string(new_proxy.server);
                        }
                    }
                    //* Similar cleanup for port and secret...                 
                    const char* port_prefixes[] = {"Port:", "port:", "PORT:"};
                    for (int i = 0; i < 3; i++) {
                        size_t prefix_len = strlen(port_prefixes[i]);
                        if (strncasecmp(new_proxy.port, port_prefixes[i], prefix_len) == 0) {
                            memmove(new_proxy.port, new_proxy.port + prefix_len, strlen(new_proxy.port) - prefix_len + 1);
                            sanitize_string(new_proxy.port);
                        }
                    }
                    
                    const char* secret_prefixes[] = {"Secret:", "secret:", "SECRET:", "Key:", "key:", "KEY:"};
                    for (int i = 0; i < 6; i++) {
                        size_t prefix_len = strlen(secret_prefixes[i]);
                        if (strncasecmp(new_proxy.secret, secret_prefixes[i], prefix_len) == 0) {
                            memmove(new_proxy.secret, new_proxy.secret + prefix_len, strlen(new_proxy.secret) - prefix_len + 1);
                            sanitize_string(new_proxy.secret);
                        }
                    }
                    //* Validate and finalize proxy
                    if (validate_proxy(new_proxy.server, new_proxy.port, new_proxy.secret)) {
                        new_proxy.hash_value = compute_hash(new_proxy.server, new_proxy.port, new_proxy.secret);
                        new_proxy.discovery_time = time(NULL);
                        new_proxy.last_verified = time(NULL);
                        new_proxy.active = 1;
                        new_proxy.verified = 0;
                        new_proxy.speed_score = 50;
                        strncpy(new_proxy.source, source, sizeof(new_proxy.source) - 1);
                        //* Classify as IP or Domain
                        int is_ip = 1;
                        for (const char *s = new_proxy.server; *s; s++) {
                            if ((*s < '0' || *s > '9') && *s != '.') {
                                is_ip = 0;
                                break;
                            }
                        }
                        strcpy(new_proxy.type, is_ip ? "IPv4" : "Domain");
                        strcpy(new_proxy.country, "UN");
                        //* Build Telegram-ready URL
                        snprintf(new_proxy.connection_url, sizeof(new_proxy.connection_url),
                                "tg://proxy?server=%s&port=%s&secret=%s",
                                new_proxy.server, new_proxy.port, new_proxy.secret);
                        
                        int duplicate_found = 0;
                        for (int i = 0; i < discovery_count; i++) {
                            if (discovered_proxies[i].hash_value == new_proxy.hash_value) {
                                duplicate_found = 1;
                                break;
                            }
                        }
                        
                        if (!duplicate_found) {
                            if (discovery_count < PROXY_BATCH_SIZE) {
                                discovered_proxies[discovery_count++] = new_proxy;
                                pattern_matches++;
                                total_discovered++;
                                
                                log_message("Found proxy: %s:%s (secret: %.32s...) from pattern %d", 
                                           new_proxy.server, new_proxy.port, new_proxy.secret, pattern_index);
                            }
                        }
                    }
                }
            }
            
            current_offset = match_vector[1] + 1;
            if (current_offset >= content_length) 
                break;
        }
        
        pcre2_match_data_free(match_data);
        pcre2_code_free(compiled_pattern);
        
        if (pattern_matches > 0) {
            log_message("Pattern %d: Found %d proxies", pattern_index, pattern_matches);
        }
    }
    
    if (discovery_count > 0) {
        pthread_mutex_lock(&storage_mutex);
        
        int current_total = atomic_load(&stats.total_proxies);
        int added_count = 0;
        
        for (int i = 0; i < discovery_count && current_total < PROXY_CAPACITY; i++) {
            int duplicate_found = 0;
            for (int j = 0; j < current_total; j++) {
                if (proxy_storage[j].hash_value == discovered_proxies[i].hash_value) {
                    duplicate_found = 1;
                    break;
                }
            }
            
            if (!duplicate_found) {
                proxy_storage[current_total++] = discovered_proxies[i];
                added_count++;
                atomic_fetch_add(&stats.unique_proxies, 1);
                atomic_fetch_add(&stats.successful_proxies, 1);
            }
        }
        
        atomic_store(&stats.total_proxies, current_total);
        atomic_fetch_add(&stats.last_cycle_proxies, added_count);
        pthread_mutex_unlock(&storage_mutex);
        
        log_message("Added %d new proxies | Total: %d", added_count, current_total);
    }
    
    free(discovered_proxies);
    
    if (total_discovered > 0) {
        log_message("Total proxies discovered from %s: %d", source, total_discovered);
    }
}

//* =============== HTTP: FETCH SINGLE URL ===============
//* Downloads content from a URL and triggers parsing

int fetch_url_content(const char *url) {
    if (!atomic_load(&program_active)) 
        return 0;
    
    CURL *curl_handle = setup_curl_handle();
    if (!curl_handle) {
        atomic_fetch_add(&stats.network_errors, 1);
        return 0;
    }
    
    DynamicBuffer content_buffer = {0};
    content_buffer.capacity = 1 * 1024 * 1024; //* 1MB initial
    content_buffer.data = malloc(content_buffer.capacity);
    if (!content_buffer.data) {
        curl_easy_cleanup(curl_handle);
        return 0;
    }
    content_buffer.data[0] = '\0';
    
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &content_buffer);
    
    atomic_fetch_add(&stats.total_requests, 1);
    log_message("Fetching: %s", url);
    
    double start_time = (double)clock() / CLOCKS_PER_SEC;
    CURLcode result = curl_easy_perform(curl_handle);
    double end_time = (double)clock() / CLOCKS_PER_SEC;
    
    int success = 0;
    
    if (result == CURLE_OK && content_buffer.size > 0) {
        long http_status = 0;
        curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_status);
        
        if (http_status == 200) {
            atomic_fetch_add(&stats.total_bytes, content_buffer.size);
            extract_proxies_from_content(content_buffer.data, content_buffer.size, url);
            success = 1;
            atomic_fetch_add(&stats.processed_urls, 1);
            log_message("Success: %s (%zu bytes, %.2f seconds)", url, content_buffer.size, end_time - start_time);
        } else {
            log_message("HTTP %ld: %s", http_status, url);
            atomic_fetch_add(&stats.network_errors, 1);
        }
    } else {
        log_message("CURL error %d: %s", result, url);
        atomic_fetch_add(&stats.network_errors, 1);
    }
    
    if (content_buffer.data) 
        free(content_buffer.data);
    
    curl_easy_cleanup(curl_handle);
    
    return success;
}
//* =============== THREAD WORKER ===============
//* Entry point for each download thread
void* url_worker(void *task_data) {
    DownloadTask *task = (DownloadTask *)task_data;
    
    if (atomic_load(&program_active) && task && task->url) {
        random_delay();
        fetch_url_content(task->url);
    }
    //* clean up dynamically allocated task
    if (task) {
        if (task->url) free(task->url);
        free(task);
    }
    
    atomic_fetch_sub(&stats.active_workers, 1);
    return NULL;
}
//* =============== OUTPUT: SAVE TO JSON + TXT ===============
//* Exports all proxies in structured JSON and simple text formats
void save_proxies_to_json() {
    pthread_mutex_lock(&file_mutex);
    
    time_t current_time = time(NULL);
    struct tm *time_info = localtime(&current_time);
    char time_string[64];
    strftime(time_string, sizeof(time_string), "%Y-%m-%d %H:%M:%S", time_info);
    
    int current_total = atomic_load(&stats.total_proxies);
    //* Buld json root object
    json_t *root = json_object();
    if (!root) {
        pthread_mutex_unlock(&file_mutex);
        return;
    }
    
    json_object_set_new(root, "version", json_string("2.0"));
    json_object_set_new(root, "updated", json_string(time_string));
    json_object_set_new(root, "total_proxies", json_integer(current_total));
    json_object_set_new(root, "unique_proxies", json_integer(atomic_load(&stats.unique_proxies)));
    json_object_set_new(root, "sources_processed", json_integer(atomic_load(&stats.processed_urls)));
    
    json_t *proxies_array = json_array();
    int saved_count = 0;
    
    for (int i = 0; i < current_total; i++) {
        if (proxy_storage[i].active) {
            json_t *proxy_obj = json_object();
            json_object_set_new(proxy_obj, "server", json_string(proxy_storage[i].server));
            json_object_set_new(proxy_obj, "port", json_string(proxy_storage[i].port));
            json_object_set_new(proxy_obj, "secret", json_string(proxy_storage[i].secret));
            json_object_set_new(proxy_obj, "url", json_string(proxy_storage[i].connection_url));
            json_object_set_new(proxy_obj, "source", json_string(proxy_storage[i].source));
            json_object_set_new(proxy_obj, "type", json_string(proxy_storage[i].type));
            json_object_set_new(proxy_obj, "country", json_string(proxy_storage[i].country));
            json_object_set_new(proxy_obj, "speed_score", json_integer(proxy_storage[i].speed_score));
            
            char discovered_str[64];
            strftime(discovered_str, sizeof(discovered_str), "%Y-%m-%d %H:%M:%S", localtime(&proxy_storage[i].discovery_time));
            json_object_set_new(proxy_obj, "discovered", json_string(discovered_str));
            
            char verified_str[64];
            strftime(verified_str, sizeof(verified_str), "%Y-%m-%d %H:%M:%S", localtime(&proxy_storage[i].last_verified));
            json_object_set_new(proxy_obj, "last_verified", json_string(verified_str));
            
            char hash_str[17];
            snprintf(hash_str, sizeof(hash_str), "%016llx", proxy_storage[i].hash_value);
            json_object_set_new(proxy_obj, "hash", json_string(hash_str));
            
            json_array_append_new(proxies_array, proxy_obj);
            saved_count++;
        }
    }
    
    json_object_set_new(root, "proxies", proxies_array);
    //* Write JSON file
    FILE *json_file = fopen("proxies.json", "w");
    if (json_file) {
        json_dumpf(root, json_file, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
        fclose(json_file);
        log_message("Saved %d proxies to proxies.json", saved_count);
    }
    
    json_decref(root);
    //* Write simple text file (tg:// URLs only)
    FILE *simple_file = fopen("proxies.txt", "w");
    if (simple_file) {
        fprintf(simple_file, "# MTPROTO PROXY LIST\n");
        fprintf(simple_file, "# Updated: %s\n", time_string);
        fprintf(simple_file, "# Total proxies: %d\n", current_total);
        fprintf(simple_file, "# Sources: %u URLs processed\n", atomic_load(&stats.processed_urls));
        fprintf(simple_file, "# Unique proxies: %u\n\n", atomic_load(&stats.unique_proxies));
        
        int txt_saved = 0;
        for (int i = 0; i < current_total; i++) {
            if (proxy_storage[i].active) {
                fprintf(simple_file, "%s\n", proxy_storage[i].connection_url);
                txt_saved++;
            }
        }
        fclose(simple_file);
        log_message("Saved %d proxies to proxies.txt", txt_saved);
    }
    
    pthread_mutex_unlock(&file_mutex);
}
//* =============== CONSOLE: REAL-TIME STATS ===============
//* Prints current performance metrics
void display_statistics() {
    time_t uptime = time(NULL) - stats.initialization_time;
    int hours = uptime / 3600;
    int minutes = (uptime % 3600) / 60;
    int seconds = uptime % 60;
    
    double mb_processed = atomic_load(&stats.total_bytes) / (1024.0 * 1024.0);
    
    printf("\n=== SYSTEM STATISTICS ===\n");
    printf("Uptime: %02d:%02d:%02d\n", hours, minutes, seconds);
    printf("Total proxies: %u\n", atomic_load(&stats.total_proxies));
    printf("Unique proxies: %u\n", atomic_load(&stats.unique_proxies));
    printf("Successful proxies: %u\n", atomic_load(&stats.successful_proxies));
    printf("URLs processed: %u/%u\n", atomic_load(&stats.processed_urls), atomic_load(&stats.total_requests));
    printf("Data processed: %.2f MB\n", mb_processed);
    printf("Completed cycles: %u\n", atomic_load(&stats.completed_cycles));
    printf("Network errors: %u\n", atomic_load(&stats.network_errors));
    printf("Active workers: %d\n", atomic_load(&stats.active_workers));
    printf("Last cycle: +%u proxies\n", atomic_load(&stats.last_cycle_proxies));
    printf("=========================\n\n");
}

void autonomous_operation() {
    log_message("STARTING ADVANCED PROXY PARSER v2.0");
    printf("==========================================\n");
    printf("🚀 ADVANCED MTPROTO PROXY PARSER v2.0\n");
    printf("Capacity: %d proxies, %d URLs, %d patterns\n", PROXY_CAPACITY, URL_CAPACITY, MAX_PATTERNS);
    printf("Threads: %d workers, %d concurrent\n", MAX_THREAD_COUNT, CONCURRENT_DOWNLOADS);
    printf("Output: JSON + Text formats\n");
    printf("Save interval: %d seconds\n", SAVE_INTERVAL);
    printf("==========================================\n");
    
    stats.initialization_time = time(NULL);
    time_t last_save = time(NULL);
    time_t last_stats = time(NULL);
    int cycle_number = 0;
    
    save_proxies_to_json();
    
    while (atomic_load(&program_active)) {
        cycle_number++;
        atomic_store(&stats.completed_cycles, cycle_number);
        atomic_store(&stats.last_cycle_proxies, 0);
        
        log_message("Starting cycle #%d", cycle_number);
        
        int url_count = 0;
        while (url_count < URL_CAPACITY && TARGET_URLS[url_count] != NULL) {
            url_count++;
        }
        
        int initial_proxy_count = atomic_load(&stats.total_proxies);
        
        pthread_t workers[MAX_THREAD_COUNT];
        int workers_launched = 0;
        int current_url_index = 0;
        
        while (current_url_index < url_count && atomic_load(&program_active)) {
            int batch_size = MIN(CONCURRENT_DOWNLOADS, url_count - current_url_index);
            
            for (int i = 0; i < batch_size && current_url_index < url_count; i++, current_url_index++) {
                DownloadTask *task = malloc(sizeof(DownloadTask));
                if (!task) continue;
                
                task->url = strdup(TARGET_URLS[current_url_index]);
                if (!task->url) {
                    free(task);
                    continue;
                }
                
                task->retry_count = 0;
                task->priority = 1;
                task->use_proxy = 0;
                
                atomic_fetch_add(&stats.active_workers, 1);
                if (pthread_create(&workers[workers_launched], NULL, url_worker, task) == 0) {
                    workers_launched++;
                } else {
                    if (task->url) free(task->url);
                    free(task);
                    atomic_fetch_sub(&stats.active_workers, 1);
                }
                
                usleep(10000 + (rand() % 15000));
            }
            
            for (int i = 0; i < workers_launched; i++) {
                pthread_join(workers[i], NULL);
            }
            workers_launched = 0;
            
            if (!atomic_load(&program_active)) 
                break;
        }
        
        time_t now = time(NULL);
        if (difftime(now, last_save) >= SAVE_INTERVAL) {
            save_proxies_to_json();
            last_save = now;
        }
        
        if (difftime(now, last_stats) >= 30) {
            display_statistics();
            last_stats = now;
        }
        
        int new_proxies = atomic_load(&stats.total_proxies) - initial_proxy_count;
        if (new_proxies > 0) {
            log_message("Cycle #%d: +%d new proxies", cycle_number, new_proxies);
        } else {
            log_message("Cycle #%d: No new proxies found", cycle_number);
        }
        
        log_message("Pausing for 8 seconds before next cycle...");
        for (int i = 0; i < 8 && atomic_load(&program_active); i++) {
            sleep(1);
        }
    }
}

void cleanup_resources() {
    atomic_store(&program_active, 0);
    log_message("Cleaning up resources...");
    
    int wait_count = 0;
    while (atomic_load(&stats.active_workers) > 0 && wait_count < 30) {
        log_message("Waiting for %d workers to finish...", atomic_load(&stats.active_workers));
        sleep(1);
        wait_count++;
    }
    
    save_proxies_to_json();
    
    pthread_mutex_destroy(&storage_mutex);
    pthread_mutex_destroy(&file_mutex);
    pthread_mutex_destroy(&log_mutex);
    
    if (proxy_storage) {
        free(proxy_storage);
        proxy_storage = NULL;
    }
    
    curl_global_cleanup();
    
    log_message("Cleanup completed. Total proxies found: %u", atomic_load(&stats.total_proxies));
}

//* Main

int main(int argc, char *argv[]) {
    printf("🚀 ADVANCED MTPROTO PROXY PARSER v2.0\n");
    printf("==========================================\n");
    
    int url_count = 0;
    while (url_count < URL_CAPACITY && TARGET_URLS[url_count] != NULL) {
        url_count++;
    }
    
    int pattern_count = 0;
    while (pattern_count < MAX_PATTERNS && PARSE_PATTERNS[pattern_count] != NULL) {
        pattern_count++;
    }
    
    printf("URL sources: %d\n", url_count);
    printf("Parse patterns: %d\n", pattern_count);
    printf("Proxy capacity: %d\n", PROXY_CAPACITY);
    printf("Thread workers: %d\n", MAX_THREAD_COUNT);
    printf("Concurrent downloads: %d\n", CONCURRENT_DOWNLOADS);
    printf("Output format: JSON + Text\n");
    printf("==========================================\n");
    
    signal(SIGINT, handle_interrupt);
    signal(SIGTERM, handle_interrupt);
    
    srand(time(NULL));
    
    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
        fprintf(stderr, "CURL initialization failed\n");
        return 1;
    }
    
    proxy_storage = calloc(PROXY_CAPACITY, sizeof(ProxyRecord));
    if (!proxy_storage) {
        fprintf(stderr, "Memory allocation failed for proxy storage\n");
        curl_global_cleanup();
        return 1;
    }
    
    if (pthread_mutex_init(&storage_mutex, NULL) != 0 ||
        pthread_mutex_init(&file_mutex, NULL) != 0 ||
        pthread_mutex_init(&log_mutex, NULL) != 0) {
        fprintf(stderr, "Mutex initialization failed\n");
        if (proxy_storage) free(proxy_storage);
        curl_global_cleanup();
        return 1;
    }
    
    autonomous_operation();
    
    cleanup_resources();
    
    printf("\n🎉 PARSER COMPLETED SUCCESSFULLY!\n");
    printf("Total proxies found: %u\n", atomic_load(&stats.total_proxies));
    printf("Unique proxies: %u\n", atomic_load(&stats.unique_proxies));
    printf("Check proxies.json and proxies.txt for results.\n");
    
    return 0;
}