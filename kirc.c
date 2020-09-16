/* See LICENSE file for license details. */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdarg.h>
#include <netdb.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <sys/socket.h>

#define MSG_MAX      512                 /* guaranteed max message length */
#define CHA_MAX      200                 /* guaranteed max channel length */
#define VERSION      "0.0.9"             /* software version */
#define USAGE        "kirc [-s hostname] [-p port] [-c channel] [-n nick] \
[-r real name] [-u username] [-k password] [-x init command] [-w columns] \
[-W columns] [-o path] [-h|v|V]"

static int    conn;                      /* connection socket */
static int    verb = 0;                  /* verbose output (e.g. raw stream) */
static size_t cmax = 80;                 /* max number of chars per line */
static size_t gutl = 10;                 /* max char width of left column */
static char * host = "irc.freenode.org"; /* irc host address */
static char * chan = "kirc";             /* channel */
static char * port = "6667";             /* server port */
static char * nick = NULL;               /* nickname */
static char * pass = NULL;               /* server password */
static char * user = NULL;               /* server user name */
static char * real = NULL;               /* server user real name */
static char * olog = NULL;               /* log irc stream path */
static char * inic = NULL;               /* server command after connection */

static void
log_append(char *str, char *path) {

    FILE *out = fopen(path, "a");

    fprintf(out, "%s\n", str);
    fclose(out);
}

static void
raw(char *fmt, ...) {

    va_list ap;
    char *cmd_str = malloc(MSG_MAX);

    va_start(ap, fmt);
    vsnprintf(cmd_str, MSG_MAX, fmt, ap);
    va_end(ap);

    if (verb) printf("<< %s", cmd_str);
    if (olog) log_append(cmd_str, olog);
    if (write(conn, cmd_str, strlen(cmd_str)) < 0) {
        perror("Write to socket");
        exit(1);
    }

    free(cmd_str);
}

static int
irc_init() {

    int gai_status;
    struct addrinfo *res, *p, hints;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM; 

    if ((gai_status = getaddrinfo(host, port, &hints, &res)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai_status));
        return -1;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        if ((conn = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            perror("socket");
            continue;
        }
        if (connect(conn, p->ai_addr, p->ai_addrlen) == -1) {
            close(conn);
            perror("connect");
            continue;
        }
        break;
    }

    freeaddrinfo(res);

    if (p == NULL) {
        fputs("Failed to connect\n", stderr);
        return -1;
    }

    fcntl(conn, F_SETFL, O_NONBLOCK);
    return 0;
}

static void
printw(const char *format, ...) {

    va_list argptr;
    char    *tok, line[MSG_MAX];
    size_t  i, wordwidth, spaceleft, spacewidth = 1;

    va_start(argptr, format);
    vsnprintf(line, MSG_MAX, format, argptr);
    va_end(argptr);

    if (olog) log_append(line, olog);

    for (i = 0; isspace(line[i]); ++i) putchar(line[i]);

    spaceleft = cmax + gutl - (i - 1);

    for(tok = strtok(&line[i], " "); tok != NULL; tok = strtok(NULL, " ")) {
        wordwidth = strlen(tok);

        if ((wordwidth + spacewidth) > spaceleft) {
            printf("\n%*.s%s ", (int) gutl + 1, "", tok);
            spaceleft = cmax - (gutl + 1 + wordwidth);
        } else {
            printf("%s ", tok);
            spaceleft = spaceleft - (wordwidth + spacewidth);
        }
    }

    putchar('\n');
}

static void
raw_parser(char *usrin) {

    char *prefix, *suffix, *message, *nickname, *command, *channel;
    int g, s;

    if (verb) printf(">> %s\n", usrin);

    if (!strncmp(usrin, "PING", 4)) {
        usrin[1] = 'O';
        raw("%s\r\n", usrin);
        return;
    }

    if (usrin[0] != ':') return;

    prefix = strtok(usrin, " ") + 1;
    suffix = strtok(NULL, ":");
    message = strtok(NULL, "\r");
    nickname = strtok(prefix, "!");
    command = strtok(suffix, "#& ");
    channel = strtok(NULL, " ");

    g = gutl, s = gutl - (strlen(nickname) <= gutl ? strlen(nickname) : gutl);

    if (!strncmp(command, "001", 3)) {
        raw("JOIN #%s\r\n", chan);
    } else if (!strncmp(command, "QUIT", 4)) {
        printw("%*s<-- \x1b[34;1m%s\x1b[0m", g - 3, "", nickname);
    } else if (!strncmp(command, "JOIN", 4)) {
        printw("%*s--> \x1b[32;1m%s\x1b[0m", g - 3, "", nickname);
    } else if (!strncmp(command, "PRIVMSG", 7) && strcmp(channel, nick) == 0) {
        printw("%*s\x1b[43;1m%-.*s\x1b[0m %s", s, "", g, nickname, message);
    } else if (!strncmp(command, "PRIVMSG", 7) && strstr(channel, chan) == NULL) {
        printw("%*s\x1b[33;1m%-.*s\x1b[0m [%s] %s", s, "", g, nickname, channel, message);
    } else printw("%*s\x1b[33;1m%-.*s\x1b[0m %s", s, "", g, nickname, message);
}

static char message_buffer[MSG_MAX + 1];
/* The first character after the end */
static size_t message_end = 0;

static int
handle_server_message(void) {

    size_t i;
    size_t old_message_end;

    for (;;) {
        ssize_t sl = read(conn, &message_buffer[message_end], MSG_MAX - message_end);
        if (sl == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Let's wait for new input */
                return 0;
            } else {
                perror("read");
                return -2;
            }
        }
        if (sl == 0) {
            fputs("Connection closed\n", stderr);
            return -1;
        }

        old_message_end = message_end;
        message_end += sl;

        /* Iterate over the added part to find \r\n */
        for (i = old_message_end; i < message_end; ++i) {
            if (i != 0 && message_buffer[i - 1] == '\r' && message_buffer[i] == '\n') {
                /* Cut the buffer here for parsing */
                char saved_char = message_buffer[i + 1];
                message_buffer[i + 1] = '\0';
                raw_parser(message_buffer);
                message_buffer[i + 1] = saved_char;

                /* Move the part after the parsed line to the beginning */
                memmove(&message_buffer, &message_buffer[i + 1], message_end - i - 1);

                /* There might still be other lines to be read */
                message_end = message_end - i - 1;
                i = 0;
            }
        }
        if (message_end == MSG_MAX) {
            message_end = 0;
        }
    }
}

static void
handle_user_input(void) {
    char usrin[MSG_MAX], v1[MSG_MAX - CHA_MAX], v2[CHA_MAX], c1;
    if (fgets(usrin, MSG_MAX, stdin) != NULL &&
        (sscanf(usrin, "/%[m] %s %[^\n]\n", &c1, v2, v1) > 2 ||
         sscanf(usrin, "/%[xMQqnjp] %[^\n]\n", &c1, v1) > 0)) {
        switch (c1) {
        case 'x': raw("%s\r\n", v1);                   break;
        case 'q': raw("quit\r\n");                     break;
        case 'Q': raw("quit %s\r\n", v1);              break;
        case 'j': raw("join %s\r\n", v1);              break;
        case 'p': raw("part %s\r\n", v1);              break;
        case 'n': raw("names #%s\r\n", chan);          break;
        case 'M': raw("privmsg nickserv :%s\r\n", v1); break;
        case 'm': raw("privmsg %s :%s\r\n", v2, v1);   break;
        }
    } else {
        size_t msg_len = strlen(usrin);
        if (usrin[msg_len - 1] == '\n') {
            usrin[msg_len - 1] = '\0';
        } 
        raw("privmsg #%s :%s\r\n", chan, usrin);
    }
}

int
main(int argc, char **argv) {

    int cval;
    struct pollfd fds[2];

    while ((cval = getopt(argc, argv, "s:p:o:n:k:c:u:r:x:w:W:hvV")) != -1) {
        switch (cval) {
            case 'V' : verb = 1;                     break;
            case 's' : host = optarg;                break;
            case 'w' : gutl = atoi(optarg);          break;
            case 'W' : cmax = atoi(optarg);          break;
            case 'p' : port = optarg;                break;
            case 'r' : real = optarg;                break;
            case 'u' : user = optarg;                break;
            case 'o' : olog = optarg;                break;
            case 'n' : nick = optarg;                break;
            case 'k' : pass = optarg;                break;
            case 'c' : chan = optarg;                break;
            case 'x' : inic = optarg;                break;
            case 'v' : printf("kirc %s\n", VERSION); return EXIT_SUCCESS;
            case 'h' : printf("usage: %s\n", USAGE); return EXIT_SUCCESS;
            case '?' :                               return EXIT_FAILURE;
        }
    }

    if (!nick) {
        perror("Nick not specified");
        return EXIT_FAILURE;
    }

    if (irc_init() != 0) {
	return EXIT_FAILURE;
    }

    raw("NICK %s\r\n", nick);
    raw("USER %s - - :%s\r\n", (user ? user : nick), (real ? real : nick));

    if (pass) raw("PASS %s\r\n", pass);
    if (inic) raw("%s\r\n", inic);

    fds[0].fd = STDIN_FILENO;
    fds[1].fd = conn;
    fds[0].events = POLLIN;
    fds[1].events = POLLIN;

    for (;;) {
        int poll_res = poll(fds, 2, -1);

        if (poll_res != -1) {
            if (fds[0].revents & POLLIN) {
                handle_user_input();
            }

            if (fds[1].revents & POLLIN) {
                int rc = handle_server_message();
                if (rc != 0) {
                    if (rc == -2) return EXIT_FAILURE;
                    return EXIT_SUCCESS;
                };
            }
        } else {
            if (errno == EAGAIN) continue;
            perror("poll");
            return EXIT_FAILURE;
        }
    }
}
