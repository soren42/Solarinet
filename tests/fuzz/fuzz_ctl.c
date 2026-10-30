/* Fuzz the ctl stream-line boundary and side-effect-free request dispatch
 * classification. Inputs are bytes only; this target never opens sockets. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { LINE_CAP = 16384 };
enum { CLASS_ORDINARY, CLASS_ENQUEUE, CLASS_PRIVILEGED };

static int is_word_char(uint8_t c)
{
    return c != 0 && c != '\n' && c != '\r' && c != ' ' && c != '\t';
}

static int classify(const char *verb)
{
    static const char *const ordinary[] = {
        "PING", "DISCOVER_STATUS", "SURVEY", "ADOPT", "ASSET_SET",
        "IGNORE", "POOL_NEW", "POOL_SET", "RULE_SET", "ALERT_ACK",
        "CRIT_SET", "ENROLL_SUBMIT", "ENROLL_ACK"
    };
    size_t i;
    if (strcmp(verb, "REQUEST_SUBMIT") == 0 || strcmp(verb, "REQUEST_GET") == 0)
        return CLASS_ENQUEUE;
    for (i = 0; i < sizeof ordinary / sizeof ordinary[0]; ++i)
        if (strcmp(verb, ordinary[i]) == 0) return CLASS_ORDINARY;
    return CLASS_PRIVILEGED;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char line[LINE_CAP];
    size_t n = size < sizeof line - 1 ? size : sizeof line - 1;
    size_t i, verb_len = 0;
    int newline = 0;
    if (n) memcpy(line, data, n);
    line[n] = '\0';
    for (i = 0; i < n; ++i) {
        if (data[i] == 0) return 0; /* production reader rejects embedded NUL */
        if (data[i] == '\n') {
            newline = (i == n - 1);
            break;
        }
    }
    if (!newline || n == 0) return 0;
    while (verb_len < n && is_word_char((uint8_t)line[verb_len])) ++verb_len;
    if (verb_len > 0 && verb_len < 128) {
        char verb[128];
        memcpy(verb, line, verb_len);
        verb[verb_len] = '\0';
        (void)classify(verb);
    }
    return 0;
}
