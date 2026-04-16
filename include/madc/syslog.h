// madc embedded syslog.h — system logging constants
// Functions (openlog, syslog, closelog, setlogmask) available via dlsym fallback

// Priorities (severity levels)
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7

// Facilities
#define LOG_KERN    0
#define LOG_USER    8
#define LOG_MAIL    16
#define LOG_DAEMON  24
#define LOG_AUTH    32
#define LOG_SYSLOG  40
#define LOG_LPR     48
#define LOG_NEWS    56
#define LOG_UUCP    64
#define LOG_CRON    72
#define LOG_AUTHPRIV 80
#define LOG_FTP     88
#define LOG_LOCAL0  128
#define LOG_LOCAL1  136
#define LOG_LOCAL2  144
#define LOG_LOCAL3  152
#define LOG_LOCAL4  160
#define LOG_LOCAL5  168
#define LOG_LOCAL6  176
#define LOG_LOCAL7  184

// openlog() options
#define LOG_PID     1
#define LOG_CONS    2
#define LOG_ODELAY  4
#define LOG_NDELAY  8
#define LOG_NOWAIT  16
#define LOG_PERROR  32
