#include "netbuffer.h"
#include "mailuser.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <ctype.h>

#define MAX_LINE_LENGTH 1024

static void handle_client(int fd);

void handle_HELO(int fd);
void handle_MAIL(int fd, char *parts[]);
void handle_RCPT(int fd, char *parts[]);
void handle_DATA(int fd);
void handle_DATA_Content(int fd, char recvbuf[], char *cpy, net_buffer_t nb);
void handle_RSET(int fd);
void handle_VRFY(int fd, char *parts[]);
void handle_NOOP(int fd);
void handle_QUIT(int fd, net_buffer_t nb);
char* retrieve_path(char* command, char* param);

// Global variables
static char* domain_name;
static int num_of_param;
static int sequence_cnt;
static user_list_t forward_paths;
static int temporary_file;
static int crlf;
static char tmp_file[]="tmp_XXXXXX";

int main(int argc, char *argv[]) {

    if (argc != 2) {
        fprintf(stderr, "Invalid arguments. Expected: %s <port>\n", argv[0]);
        return 1;
    }

    run_server(argv[1], handle_client);

    return 0;
}

void handle_client(int fd) {

    char recvbuf[MAX_LINE_LENGTH + 1];
    net_buffer_t nb = nb_create(fd, MAX_LINE_LENGTH);

    struct utsname my_uname;
    int un = uname(&my_uname);

    /* TO BE COMPLETED BY THE STUDENT */

    // handle error for uname
    // source: https://stackoverflow.com/questions/3596310/c-how-to-use-the-function-uname
    if(un < 0) {
        perror("Failed to uname");
        exit(EXIT_FAILURE);
    }

    crlf = 1;
    sequence_cnt = 1;
    forward_paths = create_user_list();

    // create a temporary file
    // source: https://blog.csdn.net/simon_dong618/article/details/1604739

    strcpy(tmp_file, "tmp_XXXXXX");

    // Session Initiation
    domain_name = my_uname.nodename; // initial message content: domain_name
    send_formatted(fd, "%d %s Simple Mail Transfer Service Ready\r\n", 220, domain_name);

    // read if there is a line from the socket/buffer
    while ((nb_read_line(nb, recvbuf)) > 0) {

        char *cpy = strdup(recvbuf);
        int len = strlen(recvbuf);
        char *buf = &recvbuf[0];
        char *parts[len];
        // Split input - command + parameter
        num_of_param = split(buf, parts);
        // if no input, check if it's under DATA command or not
        if (parts[0] == NULL) {
            // if under DATA command, write empty line
            if (sequence_cnt == 5) {
                handle_DATA_Content(fd, recvbuf, cpy, nb);
            }
            continue;
        }
        // get input command and parameter
        char *cmd = parts[0];

        // Check if command line is too long
        if (len > MAX_LINE_LENGTH) {
            send_formatted(fd, "%d %s\r\n", 500, "Line too long");
            continue;
        }

        // handler commands
        if (strcasecmp(cmd, "HELO") == 0 || strcasecmp(cmd, "EHLO") == 0) {
            handle_HELO(fd);
        } else if (strcasecmp(cmd, "MAIL") == 0) {
            handle_MAIL(fd, parts);
        } else if (strcasecmp(cmd, "RCPT") == 0) {
            handle_RCPT(fd, parts);
        } else if (strcasecmp(cmd, "DATA") == 0) {
            handle_DATA(fd);
        } else if (sequence_cnt == 5) {
            handle_DATA_Content(fd, recvbuf, cpy, nb);
        } else if (strcasecmp(cmd, "RSET") == 0) {
            handle_RSET(fd);
        } else if (strcasecmp(cmd, "VRFY") == 0) {
            handle_VRFY(fd, parts);
        } else if (strcasecmp(cmd, "NOOP") == 0) {
            handle_NOOP(fd);
        } else if (strcasecmp(cmd, "QUIT") == 0) {
            handle_QUIT(fd, nb);
        } else {
            send_formatted(fd, "%d %s\r\n", 500, "Syntax error, command unrecognized");
        }
    }

}

// HELO and EHLO (client identification)
void handle_HELO(int fd) {
    // check sequence count 1->2
    if (sequence_cnt != 0 && sequence_cnt != 1) {
        send_formatted(fd, "%d %s\r\n", 503, "Bad sequence of commands");
    } else {
        // destroy current user list and create a new one
        destroy_user_list(forward_paths);
        forward_paths = create_user_list();

        send_formatted(fd, "%d %s\r\n", 250, domain_name);
        sequence_cnt = 2;
    }
}

// First Step in Mail Transactions
// MAIL (message initialization)
void handle_MAIL(int fd, char *parts[]) {
    // check MAIL syntax
    if (num_of_param != 2) {
        send_formatted(fd, "%d %s\r\n", 501, "Syntax error in MAIL parameters or arguments");
        return;
    }
    // check sequence count 2->3
    if (sequence_cnt == 1) {
        send_formatted(fd, "%d %s\r\n", 503, "Bad sequence of commands");
    } else {
        // check path validity
        char* param = parts[1];
        char* path = retrieve_path("MAIL", param);
        if (path == NULL) {
            send_formatted(fd, "%d %s\r\n", 501, "Syntax error in MAIL parameters or arguments");
            return;
        }
        send_formatted(fd, "%d %s\r\n", 250, "OK");
        sequence_cnt = 3;
    }
}

// Second Step in Mail Transactions
// RCPT (recipient specification)
void handle_RCPT(int fd, char *parts[]) { // <forward-path> <rcpt-parameters>
    // check RCPT syntax
    if (num_of_param != 2) {
        send_formatted(fd, "%d %s\r\n", 501, "Syntax error in RCPT parameters or arguments");
        return;
    }
    // check sequence count 3->3/4
    if (sequence_cnt != 3 && sequence_cnt != 4) {
        send_formatted(fd, "%d %s\r\n", 503, "Bad sequence of commands");
    } else {
        // check path validity
        char* param = parts[1];
        char* path = retrieve_path("RCPT", param);
        if (path == NULL) {
            send_formatted(fd, "%d %s\r\n", 501, "Syntax error in RCPT parameters or arguments");
            return;
        }
        // check path validity
        if (is_valid_user(path, NULL) == 0) { // how to get password: NULL is fine.
            send_formatted(fd, "%d no such user - %s\r\n", 550, "the mailbox name");
            return;
        }
        // store to the forward-path
        add_user_to_list(&forward_paths, path);
        send_formatted(fd, "%d %s\r\n", 250, "OK");
        sequence_cnt = 4;
    }
}

// Third Step in Mail Transactions
// DATA (message contents)
void handle_DATA(int fd) {
    // check RCPT syntax
    if (num_of_param != 1) {
        send_formatted(fd, "%d %s\r\n", 501, "Syntax error in DATA parameters or arguments");
        return;
    }
    // check sequence count
    if (sequence_cnt != 4) {
        send_formatted(fd, "%d %s\r\n", 503, "Bad sequence of commands");
    } else {
        strcpy(tmp_file, "tmp_XXXXXX");
        temporary_file = mkstemp(tmp_file);
        if(temporary_file == -1) {
            printf("Create temp file fail.\n");
            exit(1);
        }
        send_formatted(fd, "%d %s\r\n", 354, "Start mail input; end with <CRLF>.<CRLF>");
        sequence_cnt = 5;
        crlf = 1;
    }
}

// v3: handle the DATA content
// submision 36: try add last dot to file, removing leading period - WRONG
// submission 37: initalize a new temp array for file - WRONG
// submision 38: add unlink, destroy&reate user list, remove save_user_mail(tmp_file, forward_paths) in the else{if{}} - WRONG
void handle_DATA_Content(int fd, char recvbuf[], char *cpy, net_buffer_t nb) {
    // DATA write finished
    if (crlf == 1 && strcasecmp(recvbuf, ".") == 0) {
        save_user_mail(tmp_file, forward_paths);

        // close temporary file
        unlink(tmp_file); // source: https://www.thegeekstuff.com/2012/06/c-temporary-files/
        close(temporary_file); //  change order? not help

        send_formatted(fd, "%d %s\r\n", 250, "OK");
        sequence_cnt = 1;
    } // DATA write not finished
    else {
        // remove the first dot on each line?
        char* content = cpy[0] == '.' ? cpy+1 : cpy;
        if ((write(temporary_file, content, strlen(content))) < 0) { // write not succeed
            send_formatted(fd, "%d %s\r\n", 250, "OK");
            sequence_cnt = 1;

            nb_destroy(nb);
            destroy_user_list(forward_paths);
            close(fd);
        } else {
            int len = strlen(cpy);
            // check if the line is end with <crlf>
            crlf = cpy[len-1]=='\n' ? 1 : 0;
        }
    }
}

// RSET (reset transmission)
void handle_RSET(int fd) {
    // close temporary file and create a new one
    close(temporary_file);
    strcpy(tmp_file, "tmp_XXXXXX");
    temporary_file = mkstemp(tmp_file);
    if(temporary_file == -1) {
        printf("Create temp file fail.\n");
        exit(1);
    }
    // destroy current user list and create a new one
    destroy_user_list(forward_paths);
    forward_paths = create_user_list();

    send_formatted(fd, "%d %s\r\n", 250, "OK");
    sequence_cnt = 0;
}

// VRFY (check username)
void handle_VRFY(int fd, char *parts[]) {
    // check VRFY syntax
    if (num_of_param != 2) {
        send_formatted(fd, "%d %s\r\n", 501, "Syntax error in MAIL parameters or arguments");
        return;
    }
    // check username validity
    char* param = parts[1];
    if (is_valid_user(param, NULL) == 0) { // NULL for password
        send_formatted(fd, "%d no such user\r\n", 550);
        return;
    }

    send_formatted(fd, "%d %s\r\n", 250, param);
}

// NOOP (no operation)
void handle_NOOP(int fd) {
    send_formatted(fd, "%d %s\r\n", 250, "OK");
}

// QUIT (session termination)
void handle_QUIT(int fd, net_buffer_t nb) {
    send_formatted(fd, "%d %s %s\r\n", 221, domain_name, "Service closing transmission channel");

    nb_destroy(nb);
    destroy_user_list(forward_paths);
    close(fd);
    exit(0);
}


// v3 extract mail address from MAIL and RCPT command
// test: MAIL FROM:<Smith@bar.com>
// test: RCPT TO:<Jones@foo.com>
char* retrieve_path(char *command, char* param) {
    char *cpy = strdup(param);
    char *res = NULL;
    // check "MAIL TO:<" syntax
    if (strcasecmp(command, "MAIL") == 0) {
        res = strtok(cpy, ":<");
        if (res == NULL || strcasecmp(res, "FROM") != 0) {
            return NULL;
        }
    } // check "RCPT FROM:<" syntax
    else if (strcasecmp(command, "RCPT") == 0) {
        res = strtok(cpy, ":<");
        if (res == NULL || strcasecmp(res, "TO") != 0) {
            return NULL;
        }
    }
    // check '>' syntax
    res = strchr(param, '>');
    if (res == NULL) {
        return NULL;
    }
    // Extract mail address inside <>
    res = strtok(param, ">");
    res = strstr(res, "<") + 1;
    // return mail address
    return res;
}
